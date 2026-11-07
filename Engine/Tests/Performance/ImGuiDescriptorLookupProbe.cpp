#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "ImGuiDescriptorLookupStudy.h"

namespace
{
inline constexpr std::uint64_t k_base = 0x100000;
inline constexpr std::uint32_t k_stride = 32;

/// @brief Native API を含めず、本番 Descriptor と同じ有効性判定に使う状態を保持する
struct Handle final
{
    std::uint32_t index = 0;
    std::uint64_t generation = 1;
    std::uint64_t allocatorId = 1;

    /// @brief 本番 Handle と同じ無効化条件を判定する
    [[nodiscard]] bool is_valid() const noexcept
    {
        return generation != 0 && allocatorId != 0;
    }
};

/// @brief 本番 Slot と同じ探索対象と Member 配置を保持する
struct Descriptor final
{
    Handle handle;
    struct Native final
    {
        std::uint64_t ptr = 0;
    };
    Native cpu;
    Native gpu;
    bool isReserved = false;
    std::uint64_t lastFence = 0;
    bool isRecorded = false;
};

/// @brief 再利用順が Logical Slot と異なる Heap を合成し、測定入力を実行時に生成する
struct Input final
{
    std::vector<Descriptor> descriptors;
    std::vector<std::size_t> map;
    std::vector<std::uint64_t> ids;
};

/// @brief 2 の累乗容量で全 Slot を異なる Physical Index に対応付ける
[[nodiscard]] Input make_input(std::size_t a_capacity, bool a_isDistributed)
{
    Input input;
    input.descriptors.resize(a_capacity);
    input.map.resize(a_capacity, cue::study::k_noDescriptor);
    for (std::size_t index = 0; index < a_capacity; ++index)
    {
        const auto physical = (index * 5 + 3) % a_capacity;
        auto &slot = input.descriptors[index];
        slot.handle.index = static_cast<std::uint32_t>(physical);
        slot.gpu.ptr = k_base + physical * k_stride;
        slot.cpu.ptr = 0x80000 + physical * k_stride;
        input.map[physical] = index;
    }
    input.ids.resize(2048);
    for (std::size_t index = 0; index < input.ids.size(); ++index)
    {
        // Font 相当は先頭 Logical Slot を繰り返し、Distributed は全 Slot を循環する
        const auto slot = a_isDistributed ? (index * 13 + 7) % a_capacity : 0;
        input.ids[index] = input.descriptors[slot].gpu.ptr;
    }
    return input;
}

/// @brief 従来の有効 Slot の線形探索を比較対象として維持する
[[nodiscard]] std::size_t find_linear(const Input &a_input, std::uint64_t a_id) noexcept
{
    const auto found =
        std::find_if(a_input.descriptors.begin(), a_input.descriptors.end(), [a_id](const Descriptor &a_slot)
                     { return a_slot.handle.is_valid() && !a_slot.isReserved && a_slot.gpu.ptr == a_id; });
    return found == a_input.descriptors.end() ? cue::study::k_noDescriptor
                                              : static_cast<std::size_t>(found - a_input.descriptors.begin());
}

/// @brief 本番の調査分岐と同じ Helper で Heap 範囲と登録状態を検証する
[[nodiscard]] std::size_t find_indexed(const Input &a_input, std::uint64_t a_id) noexcept
{
    return cue::study::find_descriptor_index(a_id, k_base, k_stride, std::span<const std::size_t>(a_input.map),
                                             std::span<const Descriptor>(a_input.descriptors));
}

/// @brief Pointer 範囲、Alignment、予約、無効化、Map 再利用の判定を測定前に比較する
void verify_lookup()
{
    auto input = make_input(32, true);
    /// @brief 線形探索と逆引きの受理・拒否結果が異なれば計測に進まない
    auto agrees = [&](std::uint64_t a_id)
    {
        if (find_linear(input, a_id) != find_indexed(input, a_id))
        {
            throw std::runtime_error("descriptor lookup mismatch");
        }
    };
    for (const auto &slot : input.descriptors)
    {
        agrees(slot.gpu.ptr);
    }
    agrees(k_base - 1);
    agrees(k_base + 1);
    agrees(k_base + 32 * k_stride);
    input.descriptors[4].isReserved = true;
    agrees(input.descriptors[4].gpu.ptr);
    input.descriptors[8].handle.generation = 0;
    agrees(input.descriptors[8].gpu.ptr);
    const auto physical = input.descriptors[11].handle.index;
    const auto oldId = input.descriptors[11].gpu.ptr;
    input.map[physical] = cue::study::k_noDescriptor;
    input.descriptors[11] = {};
    input.descriptors[11].handle.generation = 0;
    agrees(oldId);
    // Native ID の再利用は Snapshot / Fence 契約で隔離し、現在登録中の世代だけを参照する
    input.descriptors[11] = {{physical, 2, 1}, {0x80000 + physical * k_stride}, {oldId}, false, 0, false};
    input.map[physical] = 11;
    agrees(oldId);
}

using lookupFunction = std::size_t (*)(const Input &, std::uint64_t) noexcept;

/// @brief 実行時の関数選択と可視 Checksum により検索そのものの除去を防ぐ
[[nodiscard]] std::uint64_t run_lookup(const Input &a_input, lookupFunction a_lookup, unsigned a_rounds)
{
    std::uint64_t checksum = 0;
    for (unsigned round = 0; round < a_rounds; ++round)
    {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        for (const auto id : a_input.ids)
        {
            checksum += a_lookup(a_input, id);
        }
    }
    return checksum;
}

/// @brief 従来の全 Slot 登録と逆引きによる重複登録の結果を同じ単位で比較する
[[nodiscard]] std::uint64_t run_mark(Input &a_input, bool a_isIndexed, unsigned a_rounds)
{
    std::uint64_t checksum = 0;
    for (unsigned round = 0; round < a_rounds; ++round)
    {
        for (auto &slot : a_input.descriptors)
        {
            slot.isRecorded = false;
        }
        std::atomic_signal_fence(std::memory_order_seq_cst);
        for (const auto id : a_input.ids)
        {
            if (a_isIndexed)
            {
                const auto slot = find_indexed(a_input, id);
                if (slot == cue::study::k_noDescriptor)
                {
                    throw std::runtime_error("unregistered descriptor");
                }
                a_input.descriptors[slot].isRecorded = true;
            }
            else
            {
                for (auto &slot : a_input.descriptors)
                {
                    slot.isRecorded |= slot.gpu.ptr == id;
                }
            }
        }
        for (std::size_t index = 0; index < a_input.descriptors.size(); ++index)
        {
            checksum += a_input.descriptors[index].isRecorded ? index + 1 : 0;
        }
    }
    return checksum;
}

/// @brief 各条件を反復ごとに逆順で測定し、測定中の File IO を避ける
void measure(std::ostream &a_output, std::size_t a_capacity, bool a_isDistributed, unsigned a_rounds)
{
    auto input = make_input(a_capacity, a_isDistributed);
    const auto expectedLookup = run_lookup(input, &find_linear, 1);
    const auto expectedMark = run_mark(input, false, 1);
    for (unsigned repeat = 1; repeat <= 7; ++repeat)
    {
        for (unsigned order = 0; order < 2; ++order)
        {
            const bool isIndexed = ((repeat + order) % 2) != 0;
            for (unsigned phase = 0; phase < 2; ++phase)
            {
                const auto started = std::chrono::steady_clock::now();
                const auto checksum = phase == 0 ? run_lookup(input, isIndexed ? &find_indexed : &find_linear, a_rounds)
                                                 : run_mark(input, isIndexed, a_rounds);
                const auto nanoseconds =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                        .count();
                const auto expected = (phase == 0 ? expectedLookup : expectedMark) * a_rounds;
                if (checksum != expected)
                {
                    throw std::runtime_error("measurement checksum mismatch");
                }
                const auto operations = input.ids.size() * a_rounds;
                a_output << (phase == 0 ? "lookup" : "mark") << ',' << (isIndexed ? "indexed" : "linear") << ','
                         << a_capacity << ',' << (a_isDistributed ? "distributed" : "font") << ',' << repeat << ','
                         << operations << ',' << nanoseconds << ','
                         << static_cast<double>(nanoseconds) / static_cast<double>(operations) << ',' << checksum
                         << '\n';
            }
        }
    }
}
} // namespace

/// @brief GPU を使わず Descriptor 検索と登録の CPU 費用だけを分離して CSV に保存する
int main(int a_count, char **a_arguments)
{
    try
    {
        const std::filesystem::path output = a_count >= 2 ? a_arguments[1] : "DescriptorLookup.csv";
        unsigned rounds = 32;
        if (a_count >= 3)
        {
            const std::string_view text = a_arguments[2];
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), rounds);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || rounds == 0 || rounds > 1024)
            {
                return 2;
            }
        }
        verify_lookup();
        if (!output.parent_path().empty())
        {
            std::filesystem::create_directories(output.parent_path());
        }
        std::ofstream stream(output);
        if (!stream)
        {
            return 3;
        }
        stream << "phase,algorithm,capacity,pattern,repeat,operations,nanoseconds,ns_per_command,checksum\n";
        for (const auto capacity : {32u, 256u, 1024u})
        {
            measure(stream, capacity, false, rounds);
            measure(stream, capacity, true, rounds);
        }
        return stream.good() ? 0 : 4;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}

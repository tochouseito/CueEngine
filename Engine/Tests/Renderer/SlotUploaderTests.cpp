#include <Cue/Renderer/RHI/SlotUploader.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#define CHECK(a_condition) do { if (!(a_condition)) { return __LINE__; } } while (false)

/// @brief 整列配置、重複スロット、Frame 更新、範囲外入力を確認する
int main()
{
    std::array<std::byte, 32> mapped{};
    auto uploaderResult = cue::SlotUploader<std::uint32_t>::create(4, 8, mapped.data(), mapped.size());
    CHECK(uploaderResult.has_value());
    auto uploader = uploaderResult.take_value();
    CHECK(uploader.stride() == 8);
    CHECK(uploader.push(1, 10).has_value());
    CHECK(uploader.push(2, 20).has_value());
    CHECK(uploader.push(1, 30).has_value());
    CHECK(!uploader.push(4, 99).has_value());
    CHECK(uploader.commit().has_value());
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    std::memcpy(&first, mapped.data() + 8, sizeof(first));
    std::memcpy(&second, mapped.data() + 16, sizeof(second));
    CHECK(first == 30 && second == 20);
    uploader.begin_frame();
    CHECK(uploader.push(0, 40).has_value());
    CHECK(uploader.commit().has_value());
    std::memcpy(&first, mapped.data(), sizeof(first));
    CHECK(first == 40);
    CHECK(!cue::SlotUploader<std::uint32_t>::create(4, 8, mapped.data(), 24).has_value());
    CHECK(!cue::SlotUploader<std::uint32_t>::create(4, 0, mapped.data(), 32).has_value());
    return 0;
}

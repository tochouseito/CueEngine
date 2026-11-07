#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <Windows.h>

#include <evntcons.h>
#include <evntrace.h>
#include <tdh.h>

namespace
{
constexpr GUID k_spikeProvider{0x9d765397, 0xa1ac, 0x4d0f, {0x9a, 0xe5, 0x0f, 0xa7, 0x79, 0xd4, 0x7f, 0x90}};
constexpr GUID k_threadProvider{0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};

enum Field : std::size_t
{
    Qpc,
    Pid,
    Tid,
    Cpu,
    Frame,
    BeginNs,
    EndNs,
    Detail,
    Outcome,
    Sequence,
    OldTid,
    NewTid,
    OldState,
    WaitReason,
    ReadyTid,
    QpcBefore,
    QpcAfter,
    SteadyNs,
    Frequency,
    Count
};

struct Row final
{
    const char *kind = nullptr;
    std::string name;
    std::array<std::uint64_t, Count> values{};
};

/// @brief Metadata の名前で Property を読む。固定 Offset に依存せず Kernel の Version 差を検出する
bool read_property(EVENT_RECORD &a_event, const wchar_t *a_name, std::vector<BYTE> &a_bytes)
{
    PROPERTY_DATA_DESCRIPTOR descriptor{};
    descriptor.PropertyName = reinterpret_cast<ULONGLONG>(a_name);
    descriptor.ArrayIndex = (std::numeric_limits<ULONG>::max)();
    ULONG size = 0;
    if (TdhGetPropertySize(&a_event, 0, nullptr, 1, &descriptor, &size) != ERROR_SUCCESS || size == 0)
    {
        return false;
    }
    a_bytes.resize(size);
    return TdhGetProperty(&a_event, 0, nullptr, 1, &descriptor, size, a_bytes.data()) == ERROR_SUCCESS;
}

/// @brief Scalar の幅を検証し、取得失敗を実測の 0 として扱わない
bool read_number(EVENT_RECORD &a_event, const wchar_t *a_name, std::uint64_t &a_value)
{
    std::vector<BYTE> bytes;
    if (!read_property(a_event, a_name, bytes) || bytes.size() > sizeof(a_value))
    {
        return false;
    }
    a_value = 0;
    std::memcpy(&a_value, bytes.data(), bytes.size());
    return true;
}

/// @brief Probe が定義した ANSI の固定名だけを読む。CSV の構造を壊す値は拒否する
bool read_name(EVENT_RECORD &a_event, const wchar_t *a_name, std::string &a_value)
{
    std::vector<BYTE> bytes;
    if (!read_property(a_event, a_name, bytes) || bytes.back() != 0)
    {
        return false;
    }
    a_value.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size() - 1);
    return a_value.find_first_of(",\r\n\"") == std::string::npos;
}

/// @brief ETL を停止後に読む。Decode 不足と ETW 欠落を成功として報告しない
class Exporter final
{
  public:
    std::FILE *output = nullptr;
    std::size_t markers = 0;
    std::size_t switches = 0;
    std::size_t readyEvents = 0;
    std::size_t errors = 0;
    std::size_t version5Switches = 0;
    std::size_t unclassifiedStates = 0;

    struct ProviderRange final
    {
        GUID provider;
        const char *name;
        std::uint64_t first = (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t last = 0;
        std::uint64_t count = 0;
    };
    std::array<ProviderRange, 3> ranges{
        {{{0x802ec45a, 0x1e99, 0x4b83, {0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d}}, "DxgKrnl"},
         {{0xca11c036, 0x0102, 0x4a2d, {0xa6, 0xad, 0xf0, 0x3c, 0xfe, 0xd5, 0xd3, 0xc9}}, "DXGI"},
         {{0x9e9bba3c, 0x2e38, 0x40cb, {0x99, 0xf4, 0x9e, 0x82, 0x81, 0x42, 0x51, 0x64}}, "DWM"}}};

    /// @brief GPU / 表示 Provider の最初と最後の Event を明示し、循環保持の時間範囲を検証可能にする
    void write_ranges()
    {
        for (const auto &range : ranges)
        {
            if (range.count)
            {
                Row row;
                row.kind = "ProviderRange";
                row.name = range.name;
                row.values[Qpc] = range.first;
                row.values[Detail] = range.last;
                row.values[Sequence] = range.count;
                write(row);
            }
        }
    }

    /// @brief Native Callback の例外を境界内に閉じ、借用先は ProcessTrace 完了まで保持する
    static void WINAPI on_event(EVENT_RECORD *a_event) noexcept
    {
        auto &owner = *static_cast<Exporter *>(a_event->UserContext);
        try
        {
            owner.process(*a_event);
        }
        catch (...)
        {
            ++owner.errors;
        }
    }

    /// @brief 必要な Kernel / 独自 Marker だけを選び、その他の Event は元 ETL に保持する
    void process(EVENT_RECORD &a_event)
    {
        for (auto &range : ranges)
        {
            if (IsEqualGUID(a_event.EventHeader.ProviderId, range.provider))
            {
                const auto qpc = static_cast<std::uint64_t>(a_event.EventHeader.TimeStamp.QuadPart);
                range.first = (std::min)(range.first, qpc);
                range.last = (std::max)(range.last, qpc);
                ++range.count;
            }
        }
        Row row;
        row.values[Qpc] = a_event.EventHeader.TimeStamp.QuadPart;
        row.values[Pid] = a_event.EventHeader.ProcessId;
        row.values[Tid] = a_event.EventHeader.ThreadId;
        row.values[Cpu] = a_event.BufferContext.ProcessorNumber;
        bool valid = true;
        if (IsEqualGUID(a_event.EventHeader.ProviderId, k_threadProvider))
        {
            if (a_event.EventHeader.EventDescriptor.Opcode == 36)
            {
                row.kind = "CSwitch";
                valid = read_number(a_event, L"OldThreadId", row.values[OldTid]) &&
                        read_number(a_event, L"NewThreadId", row.values[NewTid]) &&
                        read_number(a_event, L"OldThreadState", row.values[OldState]) &&
                        read_number(a_event, L"OldThreadWaitReason", row.values[WaitReason]);
                // Windows の V5 は SDK の TDH Metadata にない。公開 CSwitch / PerfView と同じ Prefix を読む
                // 未知 Version へ一般化せず、28 byte の V5 だけを許可して復号件数を別に報告する
                if (!valid && a_event.EventHeader.EventDescriptor.Version == 5 && a_event.UserDataLength == 28)
                {
                    const auto *data = static_cast<const BYTE *>(a_event.UserData);
                    std::uint32_t oldThread = 0, newThread = 0;
                    std::memcpy(&newThread, data, sizeof(newThread));
                    std::memcpy(&oldThread, data + 4, sizeof(oldThread));
                    row.values[OldTid] = oldThread;
                    row.values[NewTid] = newThread;
                    row.values[OldState] = data[14];
                    row.values[WaitReason] = data[12];
                    // 未分類の State 値も生値で残す。解析側で Ready / Waiting へ勝手に割り当てない
                    unclassifiedStates += data[14] > 7 ? 1 : 0;
                    valid = true;
                    ++version5Switches;
                }
                ++switches;
            }
            else if (a_event.EventHeader.EventDescriptor.Opcode == 50)
            {
                row.kind = "ReadyThread";
                valid = read_number(a_event, L"TThreadId", row.values[ReadyTid]);
                ++readyEvents;
            }
            else
            {
                return;
            }
        }
        else if (IsEqualGUID(a_event.EventHeader.ProviderId, k_spikeProvider))
        {
            if (read_number(a_event, L"Frame", row.values[Frame]))
            {
                row.kind = "TimingSpan";
                // 遅延出力では Header は出力元 Thread。旧 Marker は Header の Thread ID を使用する
                std::uint64_t originalThread = 0;
                if (read_number(a_event, L"OriginalThreadId", originalThread))
                {
                    row.values[Tid] = originalThread;
                }
                valid = read_name(a_event, L"Name", row.name) &&
                        read_number(a_event, L"BeginNs", row.values[BeginNs]) &&
                        read_number(a_event, L"EndNs", row.values[EndNs]) &&
                        read_number(a_event, L"Detail", row.values[Detail]) &&
                        read_number(a_event, L"Outcome", row.values[Outcome]) &&
                        read_number(a_event, L"Sequence", row.values[Sequence]);
            }
            else
            {
                row.kind = "ClockSync";
                valid = read_name(a_event, L"Phase", row.name) &&
                        read_number(a_event, L"QpcBefore", row.values[QpcBefore]) &&
                        read_number(a_event, L"QpcAfter", row.values[QpcAfter]) &&
                        read_number(a_event, L"SteadyNs", row.values[SteadyNs]) &&
                        read_number(a_event, L"Frequency", row.values[Frequency]);
            }
            ++markers;
        }
        else
        {
            return;
        }
        if (!valid)
        {
            if (errors == 0)
            {
                ULONG size = 0;
                auto result = TdhGetEventInformation(&a_event, 0, nullptr, nullptr, &size);
                std::vector<BYTE> bytes(size);
                auto *information = reinterpret_cast<TRACE_EVENT_INFO *>(bytes.data());
                if (result == ERROR_INSUFFICIENT_BUFFER)
                {
                    result = TdhGetEventInformation(&a_event, 0, nullptr, information, &size);
                }
                std::fprintf(stderr, "Metadata error=%lu opcode=%u version=%u bytes=%u properties=", result,
                             a_event.EventHeader.EventDescriptor.Opcode, a_event.EventHeader.EventDescriptor.Version,
                             a_event.UserDataLength);
                std::fprintf(stderr, "old_state=%llu payload=", static_cast<unsigned long long>(row.values[OldState]));
                for (USHORT index = 0; index < a_event.UserDataLength; ++index)
                {
                    std::fprintf(stderr, "%02x", static_cast<const BYTE *>(a_event.UserData)[index]);
                }
                if (result == ERROR_SUCCESS)
                {
                    for (ULONG index = 0; index < information->TopLevelPropertyCount; ++index)
                    {
                        std::fprintf(stderr, "%ls;",
                                     reinterpret_cast<const wchar_t *>(
                                         bytes.data() + information->EventPropertyInfoArray[index].NameOffset));
                    }
                }
                std::fprintf(stderr, "\n");
            }
            ++errors;
            return;
        }
        write(row);
    }

    /// @brief 全ての Row を同じ列定義で出し、解析器が不明な種類を安全に読み飛ばせるようにする
    void write(const Row &a_row)
    {
        std::fprintf(output, "%s,%s", a_row.kind, a_row.name.c_str());
        for (const auto value : a_row.values)
        {
            std::fprintf(output, ",%llu", static_cast<unsigned long long>(value));
        }
        std::fputc('\n', output);
    }
};
} // namespace

/// @brief 生 QPC のまま Kernel と独自 Marker を出力し、異なる Clock の誤比較を防ぐ
int wmain(int a_argc, wchar_t **a_argv)
{
    if (a_argc != 3)
    {
        std::fprintf(stderr, "input.etl output.csv\n");
        return 1;
    }
    Exporter exporter;
    if (_wfopen_s(&exporter.output, a_argv[2], L"w") != 0)
    {
        return 2;
    }
    std::fprintf(exporter.output,
                 "kind,name,qpc,pid,tid,cpu,frame,begin_ns,end_ns,detail,outcome,sequence,old_tid,new_tid,"
                 "old_state,wait_reason,ready_tid,qpc_before,qpc_after,steady_ns,frequency\n");
    EVENT_TRACE_LOGFILEW log{};
    log.LogFileName = a_argv[1];
    log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log.EventRecordCallback = Exporter::on_event;
    log.Context = &exporter;
    TRACEHANDLE handle = OpenTraceW(&log);
    if (handle == INVALID_PROCESSTRACE_HANDLE)
    {
        std::fprintf(stderr, "OpenTrace error=%lu\n", GetLastError());
        std::fclose(exporter.output);
        return 3;
    }
    const auto result = ProcessTrace(&handle, 1, nullptr, nullptr);
    CloseTrace(handle);
    exporter.write_ranges();
    const bool hadWriteError = std::ferror(exporter.output) != 0;
    const bool closed = std::fclose(exporter.output) == 0;
    std::printf("process_trace=%lu markers=%zu cswitch=%zu ready=%zu decode_errors=%zu "
                "events_lost=%lu buffers_lost=%lu frequency=%lld cswitch_v5_prefix=%zu unclassified_states=%zu\n",
                result, exporter.markers, exporter.switches, exporter.readyEvents, exporter.errors,
                log.LogfileHeader.EventsLost, log.LogfileHeader.BuffersLost, log.LogfileHeader.PerfFreq.QuadPart,
                exporter.version5Switches, exporter.unclassifiedStates);
    return result == ERROR_SUCCESS && exporter.errors == 0 && !hadWriteError && closed &&
                   log.LogfileHeader.EventsLost == 0 && log.LogfileHeader.BuffersLost == 0
               ? 0
               : 4;
}

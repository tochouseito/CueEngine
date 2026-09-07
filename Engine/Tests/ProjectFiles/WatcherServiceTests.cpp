#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Windows/WindowsWorkspaceFilesystem.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/ProjectFiles/Error.h>
#include <Cue/ProjectFiles/Service.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の予期しないFatalをProcess失敗にする
    [[noreturn]] void terminate() noexcept override
    {
        std::abort();
    }

    /// @brief Test中の予期しない詳細付きFatalをProcess失敗にする
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

class UnusedOperationIdSource final : public cue::project_files::ProjectFileOperationIdSource
{
  public:
    /// @brief Watcher専用Testで使用されないOperation ID Sourceを構築する
    explicit UnusedOperationIdSource(const cue::AssertContext &a_assertContext) noexcept
        : m_assertContext(&a_assertContext)
    {
    }

    /// @brief Watcher専用Testが誤ってMutationを開始した場合に失敗を返す
    [[nodiscard]] cue::Result<std::string> next_operation_id() noexcept override
    {
        return cue::Result<std::string>::failure(cue::project_files::make_project_file_error(
            *m_assertContext, cue::project_files::ProjectFileError::InvalidRequest,
            "Watcher service test does not permit mutations"));
    }

  private:
    const cue::AssertContext *m_assertContext;
};

/// @brief Test用Native Fileへ指定Byte列を書き込む
[[nodiscard]] bool write_file(std::wstring_view a_path, std::span<const std::byte> a_bytes) noexcept
{
    const std::wstring path(a_path);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0U;
    const BOOL succeeded = WriteFile(file, a_bytes.data(), static_cast<DWORD>(a_bytes.size()), &written, nullptr);
    const BOOL flushed = FlushFileBuffers(file);
    CloseHandle(file);
    return succeeded != FALSE && flushed != FALSE && written == static_cast<DWORD>(a_bytes.size());
}

class TestProject final
{
  public:
    /// @brief ProjectFileService Watcher Test用の一意なBlank Projectを作成する
    explicit TestProject(const cue::AssertContext &a_assertContext)
    {
        std::array<wchar_t, MAX_PATH> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0U || length >= temporary.size())
        {
            return;
        }
        static std::atomic_uint64_t sequence{0U};
        m_root = temporary.data();
        m_root += L"CueProjectFilesWatcherTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64()) + L"-" +
                  std::to_wstring(sequence.fetch_add(1U, std::memory_order_relaxed));
        if (CreateDirectoryW(m_root.c_str(), nullptr) == FALSE ||
            CreateDirectoryW(child(L"Assets").c_str(), nullptr) == FALSE ||
            CreateDirectoryW(child(L"Assets\\Source").c_str(), nullptr) == FALSE ||
            CreateDirectoryW(child(L"Assets\\Runtime").c_str(), nullptr) == FALSE ||
            CreateDirectoryW(child(L"Generated").c_str(), nullptr) == FALSE ||
            CreateDirectoryW(child(L"Saved").c_str(), nullptr) == FALSE)
        {
            return;
        }
        cue::Result<cue::ProjectId> id = cue::ProjectId::parse("12345678-1234-4234-8234-123456789abc", a_assertContext);
        if (!id)
        {
            return;
        }
        cue::Result<cue::ProjectDescriptor> descriptor = cue::create_blank_project_descriptor(
            *id.try_value(), "Watcher Service Test",
            cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}}, a_assertContext);
        if (!descriptor)
        {
            return;
        }
        cue::Result<std::string> serialized =
            cue::serialize_project_descriptor(*descriptor.try_value(), a_assertContext);
        if (!serialized)
        {
            return;
        }
        m_created =
            write_file(child(L"CueProject.json"),
                       std::as_bytes(std::span(serialized.try_value()->data(), serialized.try_value()->size())));
        if (m_created)
        {
            m_descriptor.emplace(std::move(*descriptor.try_value()));
        }
    }

    /// @brief Test Projectの複製を禁止する
    TestProject(const TestProject &) = delete;
    /// @brief Test ProjectのCopy代入を禁止する
    TestProject &operator=(const TestProject &) = delete;
    /// @brief Test終了時にProject Directory Treeを削除する
    ~TestProject()
    {
        std::error_code error;
        std::filesystem::remove_all(m_root, error);
    }

    /// @brief Blank Project生成が完了したか返す
    [[nodiscard]] bool is_created() const noexcept
    {
        return m_created;
    }

    /// @brief 作成済みProject Descriptorを返す
    [[nodiscard]] const cue::ProjectDescriptor &descriptor() const noexcept
    {
        return *m_descriptor;
    }

    /// @brief Project RootのUTF-8 Pathを返す
    [[nodiscard]] std::string root_utf8() const
    {
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, m_root.data(),
                                              static_cast<int>(m_root.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(count), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, m_root.data(), static_cast<int>(m_root.size()),
                                result.data(), count, nullptr, nullptr) != count)
        {
            return {};
        }
        return result;
    }

    /// @brief Project Root配下のNative Child Pathを返す
    [[nodiscard]] std::wstring child(std::wstring_view a_relative) const
    {
        return m_root + L"\\" + std::wstring(a_relative);
    }

  private:
    std::wstring m_root;
    std::optional<cue::ProjectDescriptor> m_descriptor;
    bool m_created = false;
};

/// @brief Debounce確定済みBatchが得られるまで上限付きで待つ
[[nodiscard]] std::optional<cue::WorkspaceChangeBatch> wait_for_batch(cue::WorkspaceWatcher &a_watcher) noexcept
{
    for (std::size_t attempt = 0U; attempt < 300U; ++attempt)
    {
        cue::Result<std::optional<cue::WorkspaceChangeBatch>> drained = a_watcher.drain_changes();
        if (!drained)
        {
            return std::nullopt;
        }
        if (drained.try_value()->has_value())
        {
            return std::move(**drained.try_value());
        }
        Sleep(10U);
    }
    return std::nullopt;
}

/// @brief ProjectFileServiceがSource Asset外部変更だけをHeadless Hintとして公開するか検証する
[[nodiscard]] bool test_watcher_service(const cue::AssertContext &a_assertContext)
{
    TestProject project(a_assertContext);
    if (!project.is_created())
    {
        return false;
    }
    cue::Result<std::unique_ptr<cue::WorkspaceFilesystem>> workspace =
        cue::create_windows_workspace_filesystem(project.root_utf8(), a_assertContext);
    if (!workspace)
    {
        return false;
    }
    cue::Result<cue::project_files::ProjectFileService> service = cue::project_files::ProjectFileService::create(
        project.descriptor(), std::move(*workspace.try_value()),
        std::make_unique<UnusedOperationIdSource>(a_assertContext), a_assertContext);
    if (!service)
    {
        return false;
    }
    constexpr cue::WorkspaceWatchLimits k_limits{128U, 32U * 1024U, 64U, 25U, 250U};
    cue::Result<std::unique_ptr<cue::WorkspaceWatcher>> watcher =
        service.try_value()->create_watcher(cue::project_files::ProjectFileArea::SourceAssets, k_limits);
    cue::Result<std::unique_ptr<cue::WorkspaceWatcher>> protectedArea =
        service.try_value()->create_watcher(cue::project_files::ProjectFileArea::RuntimeAssets, k_limits);
    const std::array<std::byte, 5U> content{std::byte{'s'}, std::byte{'c'}, std::byte{'e'}, std::byte{'n'},
                                            std::byte{'e'}};
    if (!watcher || protectedArea || !write_file(project.child(L"Assets\\Source\\External.cuescene"), content))
    {
        return false;
    }
    std::optional<cue::WorkspaceChangeBatch> batch = wait_for_batch(**watcher.try_value());
    if (!batch || batch->state != cue::WorkspaceChangeBatchState::ChangesAvailable)
    {
        return false;
    }
    /// @brief Source Asset Area相対の外部Scene作成Hintを検索する
    const auto change = std::ranges::find_if(batch->changes,
                                             [](const cue::WorkspaceChangeHint &a_change) noexcept
                                             {
                                                 return a_change.kind == cue::WorkspaceChangeHintKind::Created &&
                                                        a_change.locator.text() == "External.cuescene";
                                             });
    return change != batch->changes.end() && (*watcher.try_value())->stop();
}
} // namespace

/// @brief Project Files Serviceと外部File WatcherのHeadless接続Contractを実行する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    const cue::AssertContext assertContext(logger, fatalHandler);
    return test_watcher_service(assertContext) ? 0 : 1;
}

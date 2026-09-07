#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/Project/Error.h>
#include <Cue/Project/Registry.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
    /// @brief Test 中の回復不能失敗を即座に終了 Code へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }

    /// @brief Message 付き回復不能失敗を即座に終了 Code へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

class WorkspaceFilesystem final : public cue::FilesystemRoot
{
  public:
    /// @brief User Workspace File の未作成状態から始まる Memory Root を構築する
    WorkspaceFilesystem(bool a_shouldFailWrite, const cue::AssertContext &a_assertContext) noexcept
        : m_assertContext(&a_assertContext), m_shouldFailWrite(a_shouldFailWrite)
    {
    }

    /// @brief Memory Workspace 状態の複製を禁止する
    WorkspaceFilesystem(const WorkspaceFilesystem &) = delete;
    /// @brief Memory Workspace 状態の複製代入を禁止する
    WorkspaceFilesystem &operator=(const WorkspaceFilesystem &) = delete;
    /// @brief Memory Workspace 状態の移動を禁止する
    WorkspaceFilesystem(WorkspaceFilesystem &&) = delete;
    /// @brief Memory Workspace 状態の移動代入を禁止する
    WorkspaceFilesystem &operator=(WorkspaceFilesystem &&) = delete;
    /// @brief Memory Workspace Byte 列を解放する
    ~WorkspaceFilesystem() override = default;

    /// @brief Test Double InstanceをMemory Root Identityとして返す
    [[nodiscard]] cue::Result<cue::FilesystemIdentity> root_identity() const noexcept override
    {
        return cue::Result<cue::FilesystemIdentity>::success(make_filesystem_identity(
            0x544553544D454D35ULL, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this))));
    }

    /// @brief CueWorkspace.json の可視状態だけを返す
    [[nodiscard]] cue::Result<cue::EntryType> query_entry(const cue::RelativePath &a_path) noexcept override
    {
        if (a_path.text() != "CueWorkspace.json")
        {
            return cue::Result<cue::EntryType>::success(cue::EntryType::Missing);
        }
        return cue::Result<cue::EntryType>::success(m_bytes.empty() ? cue::EntryType::Missing
                                                                    : cue::EntryType::RegularFile);
    }

    /// @brief Atomic 保存済み Workspace Byte 列を上限内で返す
    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file(const cue::RelativePath &a_path,
                                                                std::size_t a_maxBytes) noexcept override
    {
        if (a_path.text() != "CueWorkspace.json" || m_bytes.empty())
        {
            return cue::Result<std::vector<std::byte>>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::NotFound, "Workspace file was not found"));
        }
        if (m_bytes.size() > a_maxBytes)
        {
            return cue::Result<std::vector<std::byte>>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::CapacityExceeded, "Workspace file is too large"));
        }
        return cue::Result<std::vector<std::byte>>::success(std::vector<std::byte>(m_bytes));
    }

    /// @brief Registry Test で使用しない Directory 生成を明示的に拒否する
    [[nodiscard]] cue::Result<void> create_directories(const cue::RelativePath &) noexcept override
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Directory creation is unexpected"));
    }

    /// @brief Workspace File の Atomic 置換を Memory 上の一回の代入として表現する
    [[nodiscard]] cue::Result<void> write_file_atomic(const cue::RelativePath &a_path,
                                                      std::span<const std::byte> a_bytes) noexcept override
    {
        if (a_path.text() != "CueWorkspace.json")
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::InvalidPath, "Unexpected workspace path"));
        }
        if (m_shouldFailWrite)
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected workspace write failure"));
        }
        m_bytes.assign(a_bytes.begin(), a_bytes.end());
        return cue::Result<void>::success();
    }

    /// @brief Registry Test対象外のRecovery Backup公開を明示的に拒否する
    [[nodiscard]] cue::Result<void> write_recovery_backup_atomic(
        const cue::RelativePath &, std::span<const std::byte>, const cue::AssertContext &) noexcept override
    {
        return cue::Result<void>::failure(cue::make_io_error(
            *m_assertContext, cue::IoError::IoFailure, "Recovery backup is not used by registry tests"));
    }

    [[nodiscard]] cue::Result<cue::FileWriteLease> acquire_file_write_lease(const cue::RelativePath &) noexcept override
    {
        return cue::Result<cue::FileWriteLease>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Write lease is not used by registry tests"));
    }

    [[nodiscard]] cue::Result<void> write_file_atomic_if_unchanged(cue::FileWriteLease &, const cue::RelativePath &,
                                                                   cue::FileFingerprint, std::size_t,
                                                                   std::span<const std::byte>) noexcept override
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::IoFailure,
                                                             "Conditional write is not used by registry tests"));
    }

    [[nodiscard]] cue::Result<void> remove_file(const cue::RelativePath &) noexcept override
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Remove is not used by registry tests"));
    }

    /// @brief Registry Test 対象外の Staging 生成を明示的に拒否する
    [[nodiscard]] cue::Result<cue::StagingArea> create_staging_area(const cue::RelativePath &) noexcept override
    {
        return cue::Result<cue::StagingArea>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Staging creation is unexpected"));
    }

    /// @brief Registry Test 対象外の Staging 公開を明示的に拒否する
    [[nodiscard]] cue::Result<void> publish_staging_area(cue::StagingArea &&,
                                                         const cue::RelativePath &) noexcept override
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Staging publish is unexpected"));
    }

    /// @brief Registry Test 対象外の Staging Rollback を明示的に拒否する
    [[nodiscard]] cue::Result<void> rollback_staging_area(cue::StagingArea &&) noexcept override
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Staging rollback is unexpected"));
    }

  private:
    const cue::AssertContext *m_assertContext;
    bool m_shouldFailWrite;
    std::vector<std::byte> m_bytes;
};

/// @brief 指定 UUID を持つ Blank Descriptor を Registry 操作用に構築する
[[nodiscard]] cue::Result<cue::ProjectDescriptor> make_descriptor(std::string_view a_id,
                                                                  const cue::AssertContext &a_assertContext) noexcept
{
    auto projectId = cue::ProjectId::parse(a_id, a_assertContext);
    if (!projectId)
    {
        return cue::Result<cue::ProjectDescriptor>::failure(std::move(*projectId.try_error()));
    }
    return cue::create_blank_project_descriptor(
        *projectId.try_value(), "Registry Test",
        cue::EngineCompatibility{cue::EngineVersion{0U, 1U, 0U}, std::nullopt}, a_assertContext);
}

/// @brief Result の Project Error 分類が期待値と一致するか判定する
[[nodiscard]] bool has_project_error(const cue::Result<void> &a_result, cue::ProjectError a_error) noexcept
{
    return !a_result && a_result.try_error()->code().domain() == "Cue.Project" &&
           a_result.try_error()->code().value() == static_cast<std::int64_t>(a_error);
}

/// @brief Registry Result の Project Error 分類が期待値と一致するか判定する
[[nodiscard]] bool has_registry_error(const cue::Result<cue::RecentProjectRegistry> &a_result,
                                      cue::ProjectError a_error) noexcept
{
    return !a_result && a_result.try_error()->code().domain() == "Cue.Project" &&
           a_result.try_error()->code().value() == static_cast<std::int64_t>(a_error);
}

/// @brief Entry上限Test用のIndexをcanonical UUID Version 4へ変換する
[[nodiscard]] std::string make_indexed_project_id(std::uint64_t a_index)
{
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string id = "00000000-0000-4000-8000-000000000000";
    for (std::size_t digit = 0U; digit < 12U; ++digit)
    {
        id[id.size() - 1U - digit] = hexadecimal[a_index & 0x0FU];
        a_index >>= 4U;
    }
    return id;
}

/// @brief Recent 登録、重複、移動、欠損、Pin、除外の状態遷移を検証する
[[nodiscard]] bool test_registry_operations(const cue::AssertContext &a_assertContext)
{
    auto first = make_descriptor("12345678-1234-4abc-8def-1234567890ab", a_assertContext);
    auto second = make_descriptor("87654321-4321-4abc-8def-ba0987654321", a_assertContext);
    cue::RecentProjectRegistry registry;
    if (!first || !second || !registry.register_project(*first.try_value(), "C:/Projects/First", 100U, a_assertContext) ||
        !registry.register_project(*second.try_value(), "C:/Projects/Second", 200U, a_assertContext) ||
        registry.entries().size() != 2U || registry.entries()[0].project_id() != second.try_value()->project_id())
    {
        return false;
    }

    if (!registry.set_project_pinned(first.try_value()->project_id(), true, a_assertContext) ||
        !registry.set_project_pinned(second.try_value()->project_id(), true, a_assertContext) ||
        registry.entries()[0].project_id() != first.try_value()->project_id() ||
        !registry.move_pinned_project(second.try_value()->project_id(), 0U, a_assertContext) ||
        registry.entries()[0].project_id() != second.try_value()->project_id() ||
        !registry.move_pinned_project(first.try_value()->project_id(), 0U, a_assertContext) ||
        !registry.mark_project_missing(second.try_value()->project_id(), a_assertContext) ||
        !registry.mark_project_available(second.try_value()->project_id(), a_assertContext) ||
        !registry.register_project(*second.try_value(), "C:/Projects/Second", 250U, a_assertContext) ||
        !registry.mark_project_missing(second.try_value()->project_id(), a_assertContext))
    {
        return false;
    }

    auto duplicate = registry.register_project(*first.try_value(), "D:/Moved/First", 300U, a_assertContext);
    if (!has_project_error(duplicate, cue::ProjectError::DuplicateProjectId) ||
        !registry.reassociate_project(*first.try_value(), "D:/Moved/First", 300U, a_assertContext))
    {
        return false;
    }

    const cue::RecentProject *moved = nullptr;
    for (const cue::RecentProject &entry : registry.entries())
    {
        if (entry.project_id() == first.try_value()->project_id())
        {
            moved = &entry;
        }
    }
    if (moved == nullptr || moved->locator() != "D:/Moved/First" ||
        moved->locator_state() != cue::ProjectLocatorState::Moved)
    {
        return false;
    }

    auto conflict = registry.reassociate_project(*first.try_value(), "C:/Projects/Second", 400U, a_assertContext);
    return has_project_error(conflict, cue::ProjectError::ProjectLocatorConflict) &&
           registry.remove_project(first.try_value()->project_id(), a_assertContext) && registry.entries().size() == 1U &&
           registry.entries()[0].locator_state() == cue::ProjectLocatorState::Missing;
}

/// @brief Registry の Serialize、再 Parse、Atomic Save、再 Load で状態が一致するか検証する
[[nodiscard]] bool test_round_trip_and_storage(const cue::AssertContext &a_assertContext)
{
    auto descriptor = make_descriptor("12345678-1234-4abc-8def-1234567890ab", a_assertContext);
    cue::RecentProjectRegistry registry;
    if (!descriptor || !registry.register_project(*descriptor.try_value(), "C:/Projects/Cue テスト", 12345U,
                                                  a_assertContext) ||
        !registry.set_project_pinned(descriptor.try_value()->project_id(), true, a_assertContext) ||
        !registry.mark_project_missing(descriptor.try_value()->project_id(), a_assertContext))
    {
        return false;
    }

    auto descriptorJson = cue::serialize_project_descriptor(*descriptor.try_value(), a_assertContext);
    if (!descriptorJson || descriptorJson.try_value()->find("lastOpenedMillis") != std::string::npos ||
        descriptorJson.try_value()->find("isPinned") != std::string::npos ||
        descriptorJson.try_value()->find("locatorState") != std::string::npos)
    {
        return false;
    }

    auto serialized = cue::serialize_recent_project_registry(registry, a_assertContext);
    auto reparsed = serialized ? cue::parse_recent_project_registry(*serialized.try_value(), a_assertContext)
                               : cue::Result<cue::RecentProjectRegistry>::failure(
                                     cue::make_project_error(a_assertContext, cue::ProjectError::InvalidWorkspaceFormat,
                                                             "Registry serialization failed"));
    if (!reparsed || reparsed.try_value()->entries().size() != 1U ||
        reparsed.try_value()->entries()[0].locator() != "C:/Projects/Cue テスト" ||
        reparsed.try_value()->entries()[0].locator_state() != cue::ProjectLocatorState::Missing ||
        !reparsed.try_value()->entries()[0].is_pinned())
    {
        return false;
    }

    WorkspaceFilesystem workspace(false, a_assertContext);
    auto empty = cue::load_recent_project_registry(workspace, a_assertContext);
    if (!empty || !empty.try_value()->entries().empty() ||
        !cue::save_recent_project_registry(workspace, registry, a_assertContext))
    {
        return false;
    }
    auto loaded = cue::load_recent_project_registry(workspace, a_assertContext);
    return loaded && loaded.try_value()->entries().size() == 1U &&
           loaded.try_value()->entries()[0].project_id() == descriptor.try_value()->project_id();
}

/// @brief Workspace の未知 Version、重複 Identity、不正 Locator、不変条件違反を拒否するか検証する
[[nodiscard]] bool test_format_rejections(const cue::AssertContext &a_assertContext)
{
    constexpr std::string_view duplicate =
        R"json({"schemaVersion":1,"nextRegistrationOrder":3,"nextPinOrder":1,"entries":[{"projectId":"12345678-1234-4abc-8def-1234567890ab","locator":"C:/A","lastOpenedMillis":1,"isPinned":false,"pinOrder":0,"registrationOrder":1,"locatorState":"available"},{"projectId":"12345678-1234-4abc-8def-1234567890ab","locator":"C:/B","lastOpenedMillis":2,"isPinned":false,"pinOrder":0,"registrationOrder":2,"locatorState":"available"}]})json";
    constexpr std::string_view controlLocator =
        R"json({"schemaVersion":1,"nextRegistrationOrder":2,"nextPinOrder":1,"entries":[{"projectId":"12345678-1234-4abc-8def-1234567890ab","locator":"C:/A\u0001","lastOpenedMillis":1,"isPinned":false,"pinOrder":0,"registrationOrder":1,"locatorState":"available"}]})json";
    constexpr std::string_view invalidOrder =
        R"json({"schemaVersion":1,"nextRegistrationOrder":1,"nextPinOrder":1,"entries":[{"projectId":"12345678-1234-4abc-8def-1234567890ab","locator":"C:/A","lastOpenedMillis":1,"isPinned":false,"pinOrder":0,"registrationOrder":1,"locatorState":"available"}]})json";
    constexpr std::string_view future =
        R"json({"schemaVersion":2,"futureField":true,"nextRegistrationOrder":1,"nextPinOrder":1,"entries":[]})json";
    constexpr std::string_view unknownCurrent =
        R"json({"schemaVersion":1,"unknown":true,"nextRegistrationOrder":1,"nextPinOrder":1,"entries":[]})json";
    auto futureResult = cue::parse_recent_project_registry(future, a_assertContext);
    return !cue::parse_recent_project_registry(duplicate, a_assertContext) &&
           !cue::parse_recent_project_registry(controlLocator, a_assertContext) &&
           !cue::parse_recent_project_registry(invalidOrder, a_assertContext) &&
           has_registry_error(futureResult, cue::ProjectError::UnsupportedWorkspaceVersion) &&
           !cue::parse_recent_project_registry(unknownCurrent, a_assertContext);
}

/// @brief Atomic Workspace 保存失敗を Project Error へ再分類し、成功扱いしないか検証する
[[nodiscard]] bool test_storage_failure(const cue::AssertContext &a_assertContext)
{
    auto descriptor = make_descriptor("12345678-1234-4abc-8def-1234567890ab", a_assertContext);
    cue::RecentProjectRegistry registry;
    WorkspaceFilesystem workspace(true, a_assertContext);
    if (!descriptor || !registry.register_project(*descriptor.try_value(), "C:/Project", 1U, a_assertContext))
    {
        return false;
    }
    auto saved = cue::save_recent_project_registry(workspace, registry, a_assertContext);
    return has_project_error(saved, cue::ProjectError::IoFailure);
}

/// @brief JSON Reader上限まで登録でき、上限を超えるEntryを保存前に拒否するか検証する
[[nodiscard]] bool test_entry_limit(const cue::AssertContext &a_assertContext)
{
    cue::RecentProjectRegistry registry;
    for (std::uint64_t index = 0U; index < 4096U; ++index)
    {
        const std::string id = make_indexed_project_id(index);
        auto descriptor = make_descriptor(id, a_assertContext);
        const std::string locator = "C:/Projects/" + std::to_string(index);
        if (!descriptor || !registry.register_project(*descriptor.try_value(), locator, index, a_assertContext))
        {
            return false;
        }
    }

    auto overflowDescriptor = make_descriptor(make_indexed_project_id(4096U), a_assertContext);
    if (!overflowDescriptor)
    {
        return false;
    }
    auto overflow = registry.register_project(*overflowDescriptor.try_value(), "C:/Projects/4096", 4096U,
                                              a_assertContext);
    return has_project_error(overflow, cue::ProjectError::InvalidWorkspaceFormat);
}
} // namespace

/// @brief Recent Project Registry と User Workspace Storage の全 Acceptance 状態を終了 Code で検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    if (!test_registry_operations(assertContext))
    {
        return 1;
    }
    if (!test_round_trip_and_storage(assertContext))
    {
        return 2;
    }
    if (!test_format_rejections(assertContext))
    {
        return 3;
    }
    if (!test_storage_failure(assertContext))
    {
        return 4;
    }
    return test_entry_limit(assertContext) ? 0 : 5;
}

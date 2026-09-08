#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/Project/Error.h>
#include <Cue/Project/Generator.h>

#include <algorithm>
#include <array>
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

enum class FailurePoint : std::uint8_t
{
    None,
    CreateStaging,
    CreateDirectory,
    WriteDescriptor,
    ReadDescriptor,
    VerifyDescriptor,
    WriteBuildFile,
    ReadBuildFile,
    VerifyBuildFile,
    Publish,
    Durability,
    Rollback,
    WriteDescriptorAndRollback
};

struct TestFile final
{
    std::string path;
    std::vector<std::byte> bytes;
};

class GeneratorFilesystem final : public cue::FilesystemRoot
{
  public:
    /// @brief Atomic Generator の状態遷移を Memory 上で観測できる Root を構築する
    GeneratorFilesystem(FailurePoint a_failure, bool a_hasExistingDestination,
                        const cue::AssertContext &a_assertContext) noexcept
        : m_assertContext(&a_assertContext), m_failure(a_failure), m_hasExistingDestination(a_hasExistingDestination)
    {
    }

    /// @brief Test 状態の複製を禁止する
    GeneratorFilesystem(const GeneratorFilesystem &) = delete;
    /// @brief Test 状態の複製代入を禁止する
    GeneratorFilesystem &operator=(const GeneratorFilesystem &) = delete;
    /// @brief Test 状態の移動を禁止する
    GeneratorFilesystem(GeneratorFilesystem &&) = delete;
    /// @brief Test 状態の移動代入を禁止する
    GeneratorFilesystem &operator=(GeneratorFilesystem &&) = delete;
    /// @brief Memory 上の Test 状態を解放する
    ~GeneratorFilesystem() override = default;

    /// @brief Test Double InstanceをMemory Root Identityとして返す
    [[nodiscard]] cue::Result<cue::FilesystemIdentity> root_identity() const noexcept override
    {
        return cue::Result<cue::FilesystemIdentity>::success(make_filesystem_identity(
            0x544553544D454D33ULL, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this))));
    }

    /// @brief Destination と Operation 所有 Staging の可視状態を返す
    [[nodiscard]] cue::Result<cue::EntryType> query_entry(const cue::RelativePath &a_path) noexcept override
    {
        if (a_path.text() == "SampleProject" && (m_hasExistingDestination || m_isPublished))
        {
            return cue::Result<cue::EntryType>::success(cue::EntryType::Directory);
        }
        if (a_path.text() == "cue-staging-test" && m_hasStaging)
        {
            return cue::Result<cue::EntryType>::success(cue::EntryType::Directory);
        }
        for (const TestFile &file : m_files)
        {
            if (a_path.text() == file.path)
            {
                return cue::Result<cue::EntryType>::success(cue::EntryType::RegularFile);
            }
        }
        return cue::Result<cue::EntryType>::success(cue::EntryType::Missing);
    }

    /// @brief Staged Descriptor を上限内で返し、検証失敗時だけ破損 JSON を注入する
    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file(const cue::RelativePath &a_path,
                                                                std::size_t a_maxBytes) noexcept override
    {
        const auto file = std::find_if(m_files.begin(), m_files.end(), [&a_path](const TestFile &a_candidate)
                                       { return a_candidate.path == a_path.text(); });
        if (file == m_files.end())
        {
            return cue::Result<std::vector<std::byte>>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::NotFound, "Staged project file was not found"));
        }
        if (m_failure == FailurePoint::ReadDescriptor && a_path.text() == "cue-staging-test/CueProject.json")
        {
            return cue::Result<std::vector<std::byte>>::failure(cue::make_io_error(
                *m_assertContext, cue::IoError::IoFailure, "Injected descriptor verification read failure"));
        }
        if (m_failure == FailurePoint::VerifyDescriptor && a_path.text() == "cue-staging-test/CueProject.json")
        {
            const std::array invalid = {std::byte{'{'}};
            return cue::Result<std::vector<std::byte>>::success(std::vector<std::byte>(invalid.begin(), invalid.end()));
        }
        if (m_failure == FailurePoint::ReadBuildFile && a_path.text() == "cue-staging-test/Source/Game/GameModule.cpp")
        {
            return cue::Result<std::vector<std::byte>>::failure(cue::make_io_error(
                *m_assertContext, cue::IoError::IoFailure, "Injected build file verification read failure"));
        }
        if (m_failure == FailurePoint::VerifyBuildFile &&
            a_path.text() == "cue-staging-test/Source/Game/GameModule.cpp")
        {
            const std::array invalid = {std::byte{'?'}};
            return cue::Result<std::vector<std::byte>>::success(std::vector<std::byte>(invalid.begin(), invalid.end()));
        }
        if (file->bytes.size() > a_maxBytes)
        {
            return cue::Result<std::vector<std::byte>>::failure(cue::make_io_error(
                *m_assertContext, cue::IoError::CapacityExceeded, "Staged project file exceeds read limit"));
        }
        return cue::Result<std::vector<std::byte>>::success(std::vector<std::byte>(file->bytes));
    }

    /// @brief Generator が要求した標準 Directory を記録し、指定 Stage の失敗を注入する
    [[nodiscard]] cue::Result<void> create_directories(const cue::RelativePath &a_path) noexcept override
    {
        if (!m_hasStaging || !a_path.text().starts_with("cue-staging-test/"))
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::OutsideRoot, "Directory is outside staging"));
        }
        if (m_failure == FailurePoint::CreateDirectory)
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected directory failure"));
        }
        m_directories.emplace_back(a_path.text().substr(std::string_view("cue-staging-test/").size()));
        return cue::Result<void>::success();
    }

    /// @brief Staged Descriptor を一つの File として保持し、指定 Stage の失敗を注入する
    [[nodiscard]] cue::Result<void> write_file_atomic(const cue::RelativePath &a_path,
                                                      std::span<const std::byte> a_bytes) noexcept override
    {
        if ((m_failure == FailurePoint::WriteDescriptor || m_failure == FailurePoint::WriteDescriptorAndRollback) &&
            a_path.text() == "cue-staging-test/CueProject.json")
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected descriptor write failure"));
        }
        if (m_failure == FailurePoint::WriteBuildFile && a_path.text() == "cue-staging-test/Source/Game/GameModule.cpp")
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected build file write failure"));
        }
        if (!m_hasStaging || !a_path.text().starts_with("cue-staging-test/"))
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::OutsideRoot, "Project file is outside staging"));
        }
        const auto existing = std::find_if(m_files.begin(), m_files.end(), [&a_path](const TestFile &a_candidate)
                                           { return a_candidate.path == a_path.text(); });
        if (existing != m_files.end())
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::AlreadyExists, "Project file already exists"));
        }
        m_files.push_back(TestFile{std::string(a_path.text()), std::vector<std::byte>(a_bytes.begin(), a_bytes.end())});
        return cue::Result<void>::success();
    }

    /// @brief Generator Test対象外のRecovery Backup公開を明示的に拒否する
    [[nodiscard]] cue::Result<void> write_recovery_backup_atomic(const cue::RelativePath &, std::span<const std::byte>,
                                                                 const cue::AssertContext &) noexcept override
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::IoFailure,
                                                             "Recovery backup is not used by generator tests"));
    }

    [[nodiscard]] cue::Result<cue::FileWriteLease> acquire_file_write_lease(const cue::RelativePath &) noexcept override
    {
        return cue::Result<cue::FileWriteLease>::failure(cue::make_io_error(
            *m_assertContext, cue::IoError::IoFailure, "Write lease is not used by generator tests"));
    }

    [[nodiscard]] cue::Result<void> write_file_atomic_if_unchanged(cue::FileWriteLease &, const cue::RelativePath &,
                                                                   cue::FileFingerprint, std::size_t,
                                                                   std::span<const std::byte>) noexcept override
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::IoFailure,
                                                             "Conditional write is not used by generator tests"));
    }

    [[nodiscard]] cue::Result<void> remove_file(const cue::RelativePath &) noexcept override
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Remove is not used by generator tests"));
    }

    /// @brief Destination を上書きせず Operation 所有 Staging Token を発行する
    [[nodiscard]] cue::Result<cue::StagingArea> create_staging_area(
        const cue::RelativePath &a_destination) noexcept override
    {
        if (m_failure == FailurePoint::CreateStaging)
        {
            return cue::Result<cue::StagingArea>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected staging creation failure"));
        }
        if (m_hasExistingDestination || m_isPublished || a_destination.text() != "SampleProject")
        {
            return cue::Result<cue::StagingArea>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::AlreadyExists, "Destination already exists"));
        }
        auto path = cue::RelativePath::parse("cue-staging-test", *m_assertContext);
        m_hasStaging = true;
        return cue::Result<cue::StagingArea>::success(make_staging_area(std::move(*path.try_value()), 77U));
    }

    /// @brief Staging を Destination へ一度だけ公開し、Publish 前後の失敗を区別して注入する
    [[nodiscard]] cue::Result<void> publish_staging_area(cue::StagingArea &&a_staging,
                                                         const cue::RelativePath &a_destination) noexcept override
    {
        if (!m_hasStaging || staging_token(a_staging) != 77U || a_destination.text() != "SampleProject")
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Staging ownership is invalid"));
        }
        if (m_failure == FailurePoint::Publish)
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected publish failure"));
        }
        m_hasStaging = false;
        m_isPublished = true;
        invalidate_staging(a_staging);
        if (m_failure == FailurePoint::Durability)
        {
            return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::DurabilityUnknown,
                                                                 "Injected post-publish durability failure"));
        }
        return cue::Result<void>::success();
    }

    /// @brief Operation 所有 Staging だけを消去し、Rollback 失敗時は状態を保持する
    [[nodiscard]] cue::Result<void> rollback_staging_area(cue::StagingArea &&a_staging) noexcept override
    {
        if (!m_hasStaging || staging_token(a_staging) != 77U)
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Rollback ownership is invalid"));
        }
        if (m_failure == FailurePoint::Rollback || m_failure == FailurePoint::WriteDescriptorAndRollback)
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected rollback failure"));
        }
        m_hasStaging = false;
        m_directories.clear();
        m_files.clear();
        invalidate_staging(a_staging);
        return cue::Result<void>::success();
    }

    /// @brief 最終 Project Directory が公開されたか返す
    [[nodiscard]] bool is_published() const noexcept
    {
        return m_isPublished;
    }

    /// @brief 未公開 Staging が残っているか返す
    [[nodiscard]] bool has_staging() const noexcept
    {
        return m_hasStaging;
    }

    /// @brief Generator が作成した Directory 一覧を返す
    [[nodiscard]] std::span<const std::string> directories() const noexcept
    {
        return m_directories;
    }

    /// @brief Descriptor File が作成されたか返す
    [[nodiscard]] bool has_descriptor() const noexcept
    {
        return has_file("CueProject.json");
    }

    /// @brief Generatorが指定した相対PathのStaged Fileを作成したか返す
    [[nodiscard]] bool has_file(std::string_view a_path) const noexcept
    {
        constexpr std::string_view prefix = "cue-staging-test/";
        return std::ranges::any_of(m_files,
                                   [a_path, prefix](const TestFile &a_file)
                                   {
                                       return std::string_view(a_file.path).starts_with(prefix) &&
                                              std::string_view(a_file.path).substr(prefix.size()) == a_path;
                                   });
    }

    /// @brief Generatorが書いたUTF-8 File内容を検証用Viewとして返す
    [[nodiscard]] std::string_view file_contents(std::string_view a_path) const noexcept
    {
        constexpr std::string_view prefix = "cue-staging-test/";
        const auto file =
            std::ranges::find_if(m_files,
                                 [a_path, prefix](const TestFile &a_value)
                                 {
                                     return std::string_view(a_value.path).starts_with(prefix) &&
                                            std::string_view(a_value.path).substr(prefix.size()) == a_path;
                                 });
        if (file == m_files.end())
        {
            return {};
        }
        return std::string_view(reinterpret_cast<const char *>(file->bytes.data()), file->bytes.size());
    }

  private:
    const cue::AssertContext *m_assertContext;
    FailurePoint m_failure;
    bool m_hasExistingDestination = false;
    bool m_hasStaging = false;
    bool m_isPublished = false;
    std::vector<std::string> m_directories;
    std::vector<TestFile> m_files;
};

/// @brief 各 Test で独立した既知 UUID v4 を構築する
[[nodiscard]] cue::Result<cue::ProjectId> make_project_id(const cue::AssertContext &a_assertContext) noexcept
{
    return cue::ProjectId::parse("12345678-1234-4abc-8def-1234567890ab", a_assertContext);
}

/// @brief Blank Project が要求する最小 Engine Version 範囲を返す
[[nodiscard]] cue::BlankProjectTemplate make_template() noexcept
{
    return cue::BlankProjectTemplate{cue::EngineCompatibility{cue::EngineVersion{0U, 1U, 0U}, std::nullopt}};
}

/// @brief Project Error の最上位分類が期待値と一致するか判定する
[[nodiscard]] bool has_project_error(const cue::Result<cue::ProjectDescriptor> &a_result,
                                     cue::ProjectError a_error) noexcept
{
    return !a_result && a_result.try_error()->code().domain() == "Cue.Project" &&
           a_result.try_error()->code().value() == static_cast<std::int64_t>(a_error);
}

/// @brief Blank Template が必要最小 Directory と Descriptor だけを Atomic 公開するか検証する
[[nodiscard]] bool test_success(const cue::AssertContext &a_assertContext)
{
    GeneratorFilesystem filesystem(FailurePoint::None, false, a_assertContext);
    auto projectId = make_project_id(a_assertContext);
    auto generated = cue::generate_blank_project(filesystem, "SampleProject", "Sample Project", *projectId.try_value(),
                                                 make_template(), a_assertContext);
    constexpr std::array expected = {std::string_view("Assets/Source"), std::string_view("Assets/Runtime"),
                                     std::string_view("Generated"), std::string_view("Saved"),
                                     std::string_view("Source/Game")};
    if (!generated || !filesystem.is_published() || filesystem.has_staging() || !filesystem.has_descriptor() ||
        !filesystem.has_file("CMakeLists.txt") || !filesystem.has_file("CMakePresets.json") ||
        !filesystem.has_file("Source/Game/CMakeLists.txt") || !filesystem.has_file("Source/Game/GameModule.cpp") ||
        filesystem.directories().size() != expected.size())
    {
        return false;
    }
    for (std::size_t index = 0U; index < expected.size(); ++index)
    {
        if (filesystem.directories()[index] != expected[index])
        {
            return false;
        }
    }
    const std::string_view projectCMake = filesystem.file_contents("CMakeLists.txt");
    const std::string_view presets = filesystem.file_contents("CMakePresets.json");
    const std::string_view module = filesystem.file_contents("Source/Game/GameModule.cpp");
    auto serialized = cue::serialize_project_descriptor(*generated.try_value(), a_assertContext);
    return serialized && serialized.try_value()->find("\"defaultScene\":null") != std::string::npos &&
           serialized.try_value()->find("CMakeLists") == std::string::npos &&
           serialized.try_value()->find("Renderer") == std::string::npos &&
           projectCMake.find("CUE_ENGINE_ROOT") != std::string_view::npos &&
           projectCMake.find("CMAKE_VS_PLATFORM_NAME STREQUAL \"x64\"") != std::string_view::npos &&
           projectCMake.find("Sample Project") == std::string_view::npos &&
           presets.find("windows-vs2026-debug") != std::string_view::npos &&
           presets.find("windows-vs2026-development") != std::string_view::npos &&
           presets.find("windows-vs2026-release") != std::string_view::npos &&
           presets.find("$env{CUE_ENGINE_ROOT}") != std::string_view::npos &&
           module.find("0x12U, 0x34U, 0x56U, 0x78U") != std::string_view::npos &&
           module.find("a_sink->reserved[0] != 0U") != std::string_view::npos &&
           module.find("a_sink->reserved[3] != 0U") != std::string_view::npos &&
           module.find("Sample Project") == std::string_view::npos;
}

/// @brief Portable 単一 Segment でない Project 名を Filesystem 変更前に拒否するか検証する
[[nodiscard]] bool test_invalid_names(const cue::AssertContext &a_assertContext)
{
    constexpr std::array names = {std::string_view(""),
                                  std::string_view("Nested/Project"),
                                  std::string_view("CON"),
                                  std::string_view("nul.txt"),
                                  std::string_view("Project."),
                                  std::string_view("Project "),
                                  std::string_view("Project Name")};
    for (const std::string_view name : names)
    {
        GeneratorFilesystem filesystem(FailurePoint::None, false, a_assertContext);
        auto projectId = make_project_id(a_assertContext);
        auto generated = cue::generate_blank_project(filesystem, name, "Sample Project", *projectId.try_value(),
                                                     make_template(), a_assertContext);
        if (!has_project_error(generated, cue::ProjectError::InvalidProjectName) || filesystem.has_staging() ||
            filesystem.is_published())
        {
            return false;
        }
    }
    return true;
}

/// @brief 既存 Destination を空・非空の区別なく上書きしないか検証する
[[nodiscard]] bool test_existing_destination(const cue::AssertContext &a_assertContext)
{
    GeneratorFilesystem filesystem(FailurePoint::None, true, a_assertContext);
    auto projectId = make_project_id(a_assertContext);
    auto generated = cue::generate_blank_project(filesystem, "SampleProject", "Sample Project", *projectId.try_value(),
                                                 make_template(), a_assertContext);
    return has_project_error(generated, cue::ProjectError::IoFailure) && !filesystem.has_staging() &&
           !filesystem.is_published();
}

/// @brief Publish 前の各失敗を Project Error へ分類し、最終 Directory と Staging を残さないか検証する
[[nodiscard]] bool test_failure_rollback(const cue::AssertContext &a_assertContext)
{
    constexpr std::array failures = {
        FailurePoint::CreateStaging,  FailurePoint::CreateDirectory,  FailurePoint::WriteDescriptor,
        FailurePoint::ReadDescriptor, FailurePoint::VerifyDescriptor, FailurePoint::WriteBuildFile,
        FailurePoint::ReadBuildFile,  FailurePoint::VerifyBuildFile,  FailurePoint::Publish};
    for (const FailurePoint failure : failures)
    {
        GeneratorFilesystem filesystem(failure, false, a_assertContext);
        auto projectId = make_project_id(a_assertContext);
        auto generated = cue::generate_blank_project(filesystem, "SampleProject", "Sample Project",
                                                     *projectId.try_value(), make_template(), a_assertContext);
        const cue::ProjectError expected =
            failure == FailurePoint::VerifyDescriptor || failure == FailurePoint::VerifyBuildFile
                ? cue::ProjectError::InvalidFormat
                : cue::ProjectError::IoFailure;
        if (!has_project_error(generated, expected) || filesystem.is_published() || filesystem.has_staging())
        {
            return false;
        }
    }
    return true;
}

/// @brief Rollback 失敗を Primary 原因へ Secondary 診断として保持するか検証する
[[nodiscard]] bool test_rollback_diagnostics(const cue::AssertContext &a_assertContext)
{
    GeneratorFilesystem filesystem(FailurePoint::WriteDescriptorAndRollback, false, a_assertContext);
    auto projectId = make_project_id(a_assertContext);
    auto generated = cue::generate_blank_project(filesystem, "SampleProject", "Sample Project", *projectId.try_value(),
                                                 make_template(), a_assertContext);
    return has_project_error(generated, cue::ProjectError::IoFailure) && filesystem.has_staging() &&
           !filesystem.is_published() && !generated.try_error()->contexts().empty();
}

/// @brief Publish 後 Durability 失敗では完成 Directory を Rollback しないか検証する
[[nodiscard]] bool test_durability_unknown(const cue::AssertContext &a_assertContext)
{
    GeneratorFilesystem filesystem(FailurePoint::Durability, false, a_assertContext);
    auto projectId = make_project_id(a_assertContext);
    auto generated = cue::generate_blank_project(filesystem, "SampleProject", "Sample Project", *projectId.try_value(),
                                                 make_template(), a_assertContext);
    return has_project_error(generated, cue::ProjectError::IoFailure) && filesystem.is_published() &&
           !filesystem.has_staging() && generated.try_error()->root_code().domain() == "Cue.IO" &&
           generated.try_error()->root_code().value() == static_cast<std::int64_t>(cue::IoError::DurabilityUnknown);
}
} // namespace

/// @brief Atomic Blank Project Generator の成功・拒否・Rollback 契約を Process 終了 Code で検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    if (!test_success(assertContext))
    {
        return 1;
    }
    if (!test_invalid_names(assertContext))
    {
        return 2;
    }
    if (!test_existing_destination(assertContext))
    {
        return 3;
    }
    if (!test_failure_rollback(assertContext))
    {
        return 4;
    }
    if (!test_rollback_diagnostics(assertContext))
    {
        return 5;
    }
    return test_durability_unknown(assertContext) ? 0 : 6;
}

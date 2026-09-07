#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/Project/Error.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_validDescriptor = R"json({
    "schemaVersion": 1,
    "projectId": "12345678-1234-4abc-8def-1234567890ab",
    "displayName": "Cue テスト",
    "engineCompatibility": {
        "minimum": "0.1.0",
        "maximumExclusive": "1.0.0"
    },
    "roots": {
        "sourceAssets": "Assets/Source",
        "runtimeAssets": "Assets/Runtime",
        "generated": "Generated",
        "saved": "Saved"
    },
    "defaultScene": null,
    "requiredCapabilities": [],
    "extensions": {"sample.plugin":{"enabled":true,"weights":[1,2.5,null]}}
})json";

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

class MemoryFilesystemRoot final : public cue::FilesystemRoot
{
  public:
    /// @brief 初期 CueProject.json を所有する Memory Filesystem を構築する
    MemoryFilesystemRoot(std::string_view a_initial, const cue::AssertContext &a_assertContext)
        : m_bytes(std::as_bytes(std::span(a_initial.data(), a_initial.size())).begin(),
                  std::as_bytes(std::span(a_initial.data(), a_initial.size())).end()),
          m_assertContext(&a_assertContext)
    {
    }

    /// @brief Memory Filesystem の状態を複製させない
    MemoryFilesystemRoot(const MemoryFilesystemRoot &) = delete;
    /// @brief Memory Filesystem の状態を複製代入させない
    MemoryFilesystemRoot &operator=(const MemoryFilesystemRoot &) = delete;
    /// @brief Memory Filesystem の状態を移動させない
    MemoryFilesystemRoot(MemoryFilesystemRoot &&) = delete;
    /// @brief Memory Filesystem の状態を移動代入させない
    MemoryFilesystemRoot &operator=(MemoryFilesystemRoot &&) = delete;
    /// @brief 所有 Byte 列を解放する
    ~MemoryFilesystemRoot() override = default;

    /// @brief Test Double InstanceをMemory Root Identityとして返す
    [[nodiscard]] cue::Result<cue::FilesystemIdentity> root_identity() const noexcept override
    {
        return cue::Result<cue::FilesystemIdentity>::success(make_filesystem_identity(
            0x544553544D454D34ULL, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this))));
    }

    /// @brief CueProject.json の存在だけを Memory 状態から返す
    [[nodiscard]] cue::Result<cue::EntryType> query_entry(const cue::RelativePath &a_path) noexcept override
    {
        return cue::Result<cue::EntryType>::success(a_path.text() == "CueProject.json" ? cue::EntryType::RegularFile
                                                                                       : cue::EntryType::Missing);
    }

    /// @brief CueProject.json を上限内の場合だけ複製して返す
    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file(const cue::RelativePath &a_path,
                                                                std::size_t a_maxBytes) noexcept override
    {
        if (a_path.text() != "CueProject.json")
        {
            return cue::Result<std::vector<std::byte>>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::NotFound, "Memory file was not found"));
        }
        if (m_bytes.size() > a_maxBytes)
        {
            return cue::Result<std::vector<std::byte>>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::CapacityExceeded, "Memory file exceeds read limit"));
        }
        return cue::Result<std::vector<std::byte>>::success(std::vector<std::byte>(m_bytes));
    }

    /// @brief Memory Test では Directory 作成を副作用なしで成功させる
    [[nodiscard]] cue::Result<void> create_directories(const cue::RelativePath &) noexcept override
    {
        return cue::Result<void>::success();
    }

    /// @brief CueProject.json の Byte 列を一回の置換として保持する
    [[nodiscard]] cue::Result<void> write_file_atomic(const cue::RelativePath &a_path,
                                                      std::span<const std::byte> a_bytes) noexcept override
    {
        if (a_path.text() != "CueProject.json")
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::InvalidPath, "Unexpected memory file path"));
        }
        m_bytes.assign(a_bytes.begin(), a_bytes.end());
        return cue::Result<void>::success();
    }

    /// @brief Project Test対象外のRecovery Backup公開を明示的に拒否する
    [[nodiscard]] cue::Result<void> write_recovery_backup_atomic(
        const cue::RelativePath &, std::span<const std::byte>, const cue::AssertContext &) noexcept override
    {
        return cue::Result<void>::failure(cue::make_io_error(
            *m_assertContext, cue::IoError::InvalidPath, "Recovery backup is not used by project tests"));
    }

    [[nodiscard]] cue::Result<cue::FileWriteLease> acquire_file_write_lease(const cue::RelativePath &) noexcept override
    {
        return cue::Result<cue::FileWriteLease>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Write lease is not used by project tests"));
    }

    [[nodiscard]] cue::Result<void> write_file_atomic_if_unchanged(cue::FileWriteLease &, const cue::RelativePath &,
                                                                   cue::FileFingerprint, std::size_t,
                                                                   std::span<const std::byte>) noexcept override
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::IoFailure,
                                                             "Conditional write is not used by project tests"));
    }

    [[nodiscard]] cue::Result<void> remove_file(const cue::RelativePath &) noexcept override
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Remove is not used by project tests"));
    }

    /// @brief Descriptor Test 対象外の Staging 作成を明示的に拒否する
    [[nodiscard]] cue::Result<cue::StagingArea> create_staging_area(const cue::RelativePath &) noexcept override
    {
        return cue::Result<cue::StagingArea>::failure(cue::make_io_error(
            *m_assertContext, cue::IoError::IoFailure, "Staging is not implemented by memory test root"));
    }

    /// @brief Descriptor Test 対象外の Staging 公開を明示的に拒否する
    [[nodiscard]] cue::Result<void> publish_staging_area(cue::StagingArea &&,
                                                         const cue::RelativePath &) noexcept override
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::IoFailure,
                                                             "Staging is not implemented by memory test root"));
    }

    /// @brief Descriptor Test 対象外の Staging Rollback を明示的に拒否する
    [[nodiscard]] cue::Result<void> rollback_staging_area(cue::StagingArea &&) noexcept override
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::IoFailure,
                                                             "Staging is not implemented by memory test root"));
    }

  private:
    std::vector<std::byte> m_bytes;
    const cue::AssertContext *m_assertContext;
};

/// @brief Result が期待する Project Error Code を保持するか検証する
template <typename Value>
[[nodiscard]] bool has_project_error(const cue::Result<Value> &a_result, cue::ProjectError a_error) noexcept
{
    return !a_result && a_result.try_error()->code().domain() == "Cue.Project" &&
           a_result.try_error()->code().value() == static_cast<std::int64_t>(a_error);
}

/// @brief JSON 内の最初の一致箇所を Test 用の置換文字列へ変更する
[[nodiscard]] std::string replace_once(std::string_view a_text, std::string_view a_before, std::string_view a_after)
{
    std::string result(a_text);
    const std::size_t position = result.find(a_before);
    if (position != std::string::npos)
    {
        result.replace(position, a_before.size(), a_after);
    }
    return result;
}

/// @brief 単一 String 上限を守りながら文書上限直下になる compact Descriptor を構築する
[[nodiscard]] std::string make_limit_descriptor()
{
    constexpr std::size_t maximumDescriptorBytes = 1024U * 1024U;
    constexpr std::size_t maximumStringBytes = 256U * 1024U;
    std::string result = "{\"schemaVersion\":1,\"projectId\":\"12345678-1234-4abc-8def-1234567890ab\","
                         "\"displayName\":\"Limit\",\"engineCompatibility\":{\"minimum\":\"0.1.0\","
                         "\"maximumExclusive\":null},\"roots\":{\"sourceAssets\":\"Assets/Source\","
                         "\"runtimeAssets\":\"Assets/Runtime\",\"generated\":\"Generated\",\"saved\":\"Saved\"},"
                         "\"defaultScene\":null,\"requiredCapabilities\":[],\"extensions\":{\"a\":\"";
    result.append(maximumStringBytes, 'a');
    result.append("\",\"b\":\"");
    result.append(maximumStringBytes, 'b');
    result.append("\",\"c\":\"");
    result.append(maximumStringBytes, 'c');
    result.append("\",\"d\":\"");
    constexpr std::string_view suffix = "\"}}";
    const std::size_t finalStringBytes = maximumDescriptorBytes - result.size() - suffix.size();
    result.append(finalStringBytes, 'd');
    result.append(suffix);
    return result;
}

/// @brief 正常 Descriptor の Model 値、Extension 保持、Serialize Round-trip を検証する
[[nodiscard]] bool test_valid_descriptor(const cue::AssertContext &a_assertContext)
{
    auto parsed = cue::parse_project_descriptor(k_validDescriptor, a_assertContext);
    if (!parsed || parsed.try_value()->schema_version() != 1U ||
        parsed.try_value()->project_id().text() != "12345678-1234-4abc-8def-1234567890ab" ||
        parsed.try_value()->display_name() != "Cue テスト" ||
        parsed.try_value()->roots().source_assets().text() != "Assets/Source" ||
        parsed.try_value()->extensions_json() != "{\"sample.plugin\":{\"enabled\":true,\"weights\":[1,2.5,null]}}")
    {
        return false;
    }
    auto validation = cue::validate_project_descriptor(*parsed.try_value(), a_assertContext);
    auto serialized = cue::serialize_project_descriptor(*parsed.try_value(), a_assertContext);
    if (!validation || !serialized)
    {
        return false;
    }
    auto reparsed = cue::parse_project_descriptor(*serialized.try_value(), a_assertContext);
    return reparsed && parsed.try_value()->equivalent_to(*reparsed.try_value());
}

/// @brief 固定 Schema、Identity、Version、Root の不正入力を安全に拒否することを検証する
[[nodiscard]] bool test_schema_rejections(const cue::AssertContext &a_assertContext)
{
    const std::string duplicateId = replace_once(
        k_validDescriptor, "\"projectId\":", "\"projectId\":\"12345678-1234-4abc-8def-1234567890ab\",\"projectId\":");
    const std::string duplicateExtension =
        replace_once(k_validDescriptor, "\"enabled\":true", "\"enabled\":true,\"enabled\":false");
    const std::string unknown =
        replace_once(k_validDescriptor, "\"schemaVersion\": 1,", "\"schemaVersion\": 1,\"futureRequired\":true,");
    const std::string nestedUnknown =
        replace_once(k_validDescriptor, "\"minimum\": \"0.1.0\",", "\"minimum\": \"0.1.0\",\"futureRequired\":true,");
    const std::string future = replace_once(k_validDescriptor, "\"schemaVersion\": 1", "\"schemaVersion\": 2");
    const std::string futureWithField =
        replace_once(future, "\"projectId\":", "\"futureRequired\":true,\"projectId\":");
    const std::string badId =
        replace_once(k_validDescriptor, "12345678-1234-4abc-8def-1234567890ab", "12345678-1234-3abc-8def-1234567890ab");
    const std::string badRange =
        replace_once(k_validDescriptor, "\"maximumExclusive\": \"1.0.0\"", "\"maximumExclusive\": \"0.1.0\"");
    const std::string outsideRoot = replace_once(k_validDescriptor, "\"Generated\"", "\"../Generated\"");
    const std::string nestedRoot = replace_once(k_validDescriptor, "\"Generated\"", "\"assets/source/Generated\"");
    const std::string reservedRoot = replace_once(k_validDescriptor, "\"Generated\"", "\"NUL.data\"");
    const std::string descriptorCollision =
        replace_once(k_validDescriptor, "\"Generated\"", "\"CueProject.json/Data\"");
    const std::string defaultScene =
        replace_once(k_validDescriptor, "\"defaultScene\": null", "\"defaultScene\": \"scene-id\"");
    const std::string capabilities =
        replace_once(k_validDescriptor, "\"requiredCapabilities\": []", "\"requiredCapabilities\": [\"mesh\"]");

    return has_project_error(cue::parse_project_descriptor(duplicateId, a_assertContext),
                             cue::ProjectError::InvalidFormat) &&
           has_project_error(cue::parse_project_descriptor(duplicateExtension, a_assertContext),
                             cue::ProjectError::InvalidFormat) &&
           has_project_error(cue::parse_project_descriptor(unknown, a_assertContext),
                             cue::ProjectError::InvalidFormat) &&
           has_project_error(cue::parse_project_descriptor(nestedUnknown, a_assertContext),
                             cue::ProjectError::InvalidEngineCompatibility) &&
           has_project_error(cue::parse_project_descriptor(future, a_assertContext),
                             cue::ProjectError::UnsupportedSchemaVersion) &&
           has_project_error(cue::parse_project_descriptor(futureWithField, a_assertContext),
                             cue::ProjectError::UnsupportedSchemaVersion) &&
           has_project_error(cue::parse_project_descriptor(badId, a_assertContext),
                             cue::ProjectError::InvalidProjectId) &&
           has_project_error(cue::parse_project_descriptor(badRange, a_assertContext),
                             cue::ProjectError::InvalidEngineCompatibility) &&
           has_project_error(cue::parse_project_descriptor(outsideRoot, a_assertContext),
                             cue::ProjectError::InvalidRoots) &&
           has_project_error(cue::parse_project_descriptor(nestedRoot, a_assertContext),
                             cue::ProjectError::InvalidRoots) &&
           has_project_error(cue::parse_project_descriptor(reservedRoot, a_assertContext),
                             cue::ProjectError::InvalidRoots) &&
           has_project_error(cue::parse_project_descriptor(descriptorCollision, a_assertContext),
                             cue::ProjectError::InvalidRoots) &&
           has_project_error(cue::parse_project_descriptor(defaultScene, a_assertContext),
                             cue::ProjectError::InvalidFormat) &&
           has_project_error(cue::parse_project_descriptor(capabilities, a_assertContext),
                             cue::ProjectError::InvalidFormat);
}

/// @brief BOM、Control 文字、Resource Limit、UTF-8 不正を Parse 前後で拒否することを検証する
[[nodiscard]] bool test_encoding_and_limits(const cue::AssertContext &a_assertContext)
{
    const std::string bom = std::string("\xEF\xBB\xBF") + std::string(k_validDescriptor);
    const std::string control = replace_once(k_validDescriptor, "Cue テスト", "Cue\\u0001Test");
    const std::string invalidUtf8 = replace_once(k_validDescriptor, "Cue テスト", std::string("Cue\xC0\xAF"));
    const std::string tooLarge(1024U * 1024U + 1U, ' ');
    const std::string atLimit = make_limit_descriptor();
    std::string deep = "{}";
    for (std::size_t index = 0U; index < 32U; ++index)
    {
        deep = "{\"x\":" + deep + "}";
    }
    auto parsedAtLimit = cue::parse_project_descriptor(atLimit, a_assertContext);
    auto serializedAtLimit =
        parsedAtLimit ? cue::serialize_project_descriptor(*parsedAtLimit.try_value(), a_assertContext)
                      : cue::Result<std::string>::failure(cue::make_project_error(
                            a_assertContext, cue::ProjectError::InvalidFormat, "Boundary descriptor did not parse"));
    return atLimit.size() == 1024U * 1024U && parsedAtLimit && serializedAtLimit &&
           serializedAtLimit.try_value()->size() <= 1024U * 1024U &&
           has_project_error(cue::parse_project_descriptor(bom, a_assertContext), cue::ProjectError::InvalidFormat) &&
           has_project_error(cue::parse_project_descriptor(control, a_assertContext),
                             cue::ProjectError::InvalidDisplayName) &&
           has_project_error(cue::parse_project_descriptor(invalidUtf8, a_assertContext),
                             cue::ProjectError::InvalidFormat) &&
           has_project_error(cue::parse_project_descriptor(tooLarge, a_assertContext),
                             cue::ProjectError::InvalidFormat) &&
           has_project_error(cue::parse_project_descriptor(deep, a_assertContext), cue::ProjectError::InvalidFormat);
}

/// @brief 異なる Locator からの Load でも Identity を保持し、Atomic Save 後に再読込できるか検証する
[[nodiscard]] bool test_storage_and_move_identity(const cue::AssertContext &a_assertContext)
{
    MemoryFilesystemRoot original(k_validDescriptor, a_assertContext);
    MemoryFilesystemRoot moved(k_validDescriptor, a_assertContext);
    auto first = cue::load_project_descriptor(original, a_assertContext);
    auto second = cue::load_project_descriptor(moved, a_assertContext);
    if (!first || !second || first.try_value()->project_id() != second.try_value()->project_id())
    {
        return false;
    }
    auto saved = cue::save_project_descriptor(moved, *first.try_value(), a_assertContext);
    auto loaded = cue::load_project_descriptor(moved, a_assertContext);
    return saved && loaded && first.try_value()->equivalent_to(*loaded.try_value());
}
} // namespace

/// @brief Project Descriptor v1 の解析、検証、Round-trip、Storage 契約を統合検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_valid_descriptor(assertContext) && test_schema_rejections(assertContext) &&
                   test_encoding_and_limits(assertContext) && test_storage_and_move_identity(assertContext)
               ? 0
               : 1;
}

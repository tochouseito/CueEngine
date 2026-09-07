#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/Scene/Error.h>
#include <Cue/Scene/Serialization.h>
#include <Cue/Schema/Descriptor.h>

#include <cstdlib>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_sceneJson = R"json({
    "formatVersion": 1,
    "sceneAssetId": "00000000-0000-4000-8000-000000000001",
    "objects": [{
        "objectId": "00000000-0000-4000-8000-000000000002",
        "name": "Root",
        "active": true,
        "parentObjectId": null,
        "transform": {
            "translation": [1,2,3],
            "rotation": [0,0,0,1],
            "scale": [1,1,1]
        },
        "components": [{
            "componentInstanceId": "00000000-0000-4000-8000-000000000003",
            "typeId": "10000000-0000-4000-8000-000000000001",
            "schemaVersion": 1,
            "fields": [
                {"fieldId":99,"value":{"future":[1,2,3]}},
                {"fieldId":2,"value":"Player"},
                {"fieldId":1,"value":-5}
            ]
        },{
            "componentInstanceId": "00000000-0000-4000-8000-000000000004",
            "typeId": "10000000-0000-4000-8000-000000000001",
            "schemaVersion": 2,
            "fields": [{"fieldId":1,"value":{"future":true}}],
            "futureMetadata": {"preserved":true}
        }]
    },{
        "objectId": "00000000-0000-4000-8000-000000000001",
        "name": "SortedFirst",
        "active": false,
        "parentObjectId": null,
        "transform": {
            "translation": [0,0,0],
            "rotation": [0,0,0,1],
            "scale": [1,1,1]
        },
        "components": []
    }],
    "extensions": {"sample":{"enabled":true}}
})json";

constexpr std::string_view k_componentMigrationJson = R"json({
    "formatVersion": 1,
    "sceneAssetId": "00000000-0000-4000-8000-000000000011",
    "objects": [{
        "objectId": "00000000-0000-4000-8000-000000000012",
        "name": "Migrated",
        "active": true,
        "parentObjectId": null,
        "transform": {
            "translation": [0,0,0],
            "rotation": [0,0,0,1],
            "scale": [1,1,1]
        },
        "components": [{
            "componentInstanceId": "00000000-0000-4000-8000-000000000013",
            "typeId": "10000000-0000-4000-8000-000000000001",
            "schemaVersion": 1,
            "fields": [{"fieldId":1,"value":-5},{"fieldId":2,"value":"Migrated"}]
        }]
    }],
    "extensions": {}
})json";

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の通常FatalをProcess失敗へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::abort();
    }

    /// @brief Test中のMessage付きFatalをProcess失敗へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

class MemoryFilesystemRoot final : public cue::FilesystemRoot
{
  public:
    /// @brief 診断Contextを非所有で保持するMemory Filesystemを構築する
    explicit MemoryFilesystemRoot(const cue::AssertContext &a_assertContext) noexcept
        : m_assertContext(&a_assertContext)
    {
    }

    /// @brief Memory FilesystemのCopyを禁止する
    MemoryFilesystemRoot(const MemoryFilesystemRoot &) = delete;
    /// @brief Memory FilesystemのCopy代入を禁止する
    MemoryFilesystemRoot &operator=(const MemoryFilesystemRoot &) = delete;
    /// @brief Memory FilesystemのMoveを禁止する
    MemoryFilesystemRoot(MemoryFilesystemRoot &&) = delete;
    /// @brief Memory FilesystemのMove代入を禁止する
    MemoryFilesystemRoot &operator=(MemoryFilesystemRoot &&) = delete;
    /// @brief Memory File所有値を破棄する
    ~MemoryFilesystemRoot() override = default;

    /// @brief Test Double InstanceをMemory Root Identityとして返す
    [[nodiscard]] cue::Result<cue::FilesystemIdentity> root_identity() const noexcept override
    {
        return cue::Result<cue::FilesystemIdentity>::success(make_filesystem_identity(
            0x544553544D454D36ULL, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this))));
    }

    /// @brief Test Fileを指定Pathへ設定する
    void set(std::string_view a_path, std::string_view a_text)
    {
        const auto bytes = std::as_bytes(std::span(a_text.data(), a_text.size()));
        m_files[std::string(a_path)] = std::vector<std::byte>(bytes.begin(), bytes.end());
    }

    /// @brief Test File内容をText Viewで返す
    [[nodiscard]] std::string_view text(std::string_view a_path) const noexcept
    {
        const auto found = m_files.find(std::string(a_path));
        if (found == m_files.end())
        {
            return {};
        }
        return {reinterpret_cast<const char *>(found->second.data()), found->second.size()};
    }

    /// @brief Test用Recovery Backup本文を内部名前空間へ設定する
    void set_recovery_backup(std::string_view a_destination, std::string_view a_text)
    {
        set(recovery_backup_key(a_destination), a_text);
    }

    /// @brief Test用Recovery Backup本文を内部名前空間から返す
    [[nodiscard]] std::string_view recovery_backup_text(std::string_view a_destination) const noexcept
    {
        return text(recovery_backup_key(a_destination));
    }

    /// @brief Main Sceneへの次回Atomic Writeを失敗させる
    void fail_main_write(bool a_fail) noexcept
    {
        m_failMainWrite = a_fail;
    }

    /// @brief Recovery BackupへのAtomic Writeを失敗させる
    void fail_backup_write(bool a_fail) noexcept
    {
        m_failBackupWrite = a_fail;
    }

    /// @brief Main Sceneへの次回Writeを公開済みDurability不明として報告する
    void make_main_write_durability_unknown(bool a_unknown) noexcept
    {
        m_isMainDurabilityUnknown = a_unknown;
    }

    /// @brief Recovery Backupへの次回Writeを公開済みDurability不明として報告する
    void make_backup_write_durability_unknown(bool a_unknown) noexcept
    {
        m_isBackupDurabilityUnknown = a_unknown;
    }

    /// @brief Main Scene本文の公開後だけ再読込を失敗させる
    void fail_read_after_main_write(bool a_fail) noexcept
    {
        m_failReadAfterMainWrite = a_fail;
        m_hasWrittenMain = false;
    }

    /// @brief Memory内Entry種別を返す
    [[nodiscard]] cue::Result<cue::EntryType> query_entry(const cue::RelativePath &a_path) noexcept override
    {
        return cue::Result<cue::EntryType>::success(
            m_files.contains(std::string(a_path.text())) ? cue::EntryType::RegularFile : cue::EntryType::Missing);
    }

    /// @brief 上限内のMemory Fileを複製して返す
    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file(const cue::RelativePath &a_path,
                                                                std::size_t a_maxBytes) noexcept override
    {
        if (m_failReadAfterMainWrite && m_hasWrittenMain && a_path.text() == "Scenes/Main.cuescene")
        {
            return cue::Result<std::vector<std::byte>>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected post-publish read failure"));
        }
        const auto found = m_files.find(std::string(a_path.text()));
        if (found == m_files.end())
        {
            return cue::Result<std::vector<std::byte>>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::NotFound, "Memory scene file was not found"));
        }
        if (found->second.size() > a_maxBytes)
        {
            return cue::Result<std::vector<std::byte>>::failure(cue::make_io_error(
                *m_assertContext, cue::IoError::CapacityExceeded, "Memory scene file exceeds limit"));
        }
        return cue::Result<std::vector<std::byte>>::success(std::vector<std::byte>(found->second));
    }

    /// @brief Memory TestではDirectory作成を副作用なしで成功させる
    [[nodiscard]] cue::Result<void> create_directories(const cue::RelativePath &) noexcept override
    {
        return cue::Result<void>::success();
    }

    /// @brief Path単位の一回置換または注入失敗を実行する
    [[nodiscard]] cue::Result<void> write_file_atomic(const cue::RelativePath &a_path,
                                                      std::span<const std::byte> a_bytes) noexcept override
    {
        if (m_failMainWrite && a_path.text() == "Scenes/Main.cuescene")
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected scene publish failure"));
        }
        m_files[std::string(a_path.text())] = std::vector<std::byte>(a_bytes.begin(), a_bytes.end());
        if (a_path.text() == "Scenes/Main.cuescene")
        {
            m_hasWrittenMain = true;
        }
        if (m_isMainDurabilityUnknown && a_path.text() == "Scenes/Main.cuescene")
        {
            return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::DurabilityUnknown,
                                                                 "Injected scene durability uncertainty"));
        }
        return cue::Result<void>::success();
    }

    /// @brief 利用者Locator外のMemory KeyへRecovery Backupを公開する
    [[nodiscard]] cue::Result<void> write_recovery_backup_atomic(
        const cue::RelativePath &a_destination, std::span<const std::byte> a_bytes,
        const cue::AssertContext &) noexcept override
    {
        if (m_failBackupWrite)
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected backup publish failure"));
        }
        m_files[recovery_backup_key(a_destination.text())] =
            std::vector<std::byte>(a_bytes.begin(), a_bytes.end());
        if (m_isBackupDurabilityUnknown)
        {
            return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::DurabilityUnknown,
                                                                 "Injected backup durability uncertainty"));
        }
        return cue::Result<void>::success();
    }

    [[nodiscard]] cue::Result<cue::FileWriteLease> acquire_file_write_lease(const cue::RelativePath &) noexcept override
    {
        return cue::Result<cue::FileWriteLease>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Write lease is not used by scene tests"));
    }

    [[nodiscard]] cue::Result<void> write_file_atomic_if_unchanged(cue::FileWriteLease &, const cue::RelativePath &,
                                                                   cue::FileFingerprint, std::size_t,
                                                                   std::span<const std::byte>) noexcept override
    {
        return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::IoFailure,
                                                             "Conditional write is not used by scene tests"));
    }

    [[nodiscard]] cue::Result<void> remove_file(const cue::RelativePath &) noexcept override
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Remove is not used by scene tests"));
    }

    /// @brief Scene Serializer対象外のStaging作成を拒否する
    [[nodiscard]] cue::Result<cue::StagingArea> create_staging_area(const cue::RelativePath &) noexcept override
    {
        return cue::Result<cue::StagingArea>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Staging is not used by scene tests"));
    }

    /// @brief Scene Serializer対象外のStaging公開を拒否する
    [[nodiscard]] cue::Result<void> publish_staging_area(cue::StagingArea &&,
                                                         const cue::RelativePath &) noexcept override
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Staging is not used by scene tests"));
    }

    /// @brief Scene Serializer対象外のStaging破棄を拒否する
    [[nodiscard]] cue::Result<void> rollback_staging_area(cue::StagingArea &&) noexcept override
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Staging is not used by scene tests"));
    }

  private:
    /// @brief 利用者RelativePathでは表現できないTest用Recovery Backup Keyを生成する
    [[nodiscard]] static std::string recovery_backup_key(std::string_view a_destination)
    {
        std::string key(1U, '\x1f');
        key.append("CueBackup:");
        key.append(a_destination);
        return key;
    }

    std::map<std::string, std::vector<std::byte>> m_files;
    const cue::AssertContext *m_assertContext;
    bool m_failMainWrite = false;
    bool m_failBackupWrite = false;
    bool m_isMainDurabilityUnknown = false;
    bool m_isBackupDurabilityUnknown = false;
    bool m_failReadAfterMainWrite = false;
    bool m_hasWrittenMain = false;
};

/// @brief 条件が偽ならTest Processを失敗終了する
void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::abort();
    }
}

/// @brief 成功Resultから所有Valueを取り出す
template <typename Value> Value take_value(cue::Result<Value> &&a_result) noexcept
{
    require(a_result.has_value());
    return std::move(*a_result.try_value());
}

/// @brief Test用TypeIdを返す
[[nodiscard]] cue::schema::TypeId make_type_id(const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::TypeId::parse("10000000-0000-4000-8000-000000000001", a_assertContext));
}

/// @brief Test用FieldIdを返す
[[nodiscard]] cue::schema::FieldId make_field_id(std::uint32_t a_value,
                                                 const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::FieldId::create(a_value, a_assertContext));
}

/// @brief Test用SchemaVersionを返す
[[nodiscard]] cue::schema::SchemaVersion make_version(std::uint32_t a_value,
                                                      const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::SchemaVersion::create(a_value, a_assertContext));
}

/// @brief Test用Schema Registryを構築する
[[nodiscard]] std::unique_ptr<cue::schema::SchemaRegistry> make_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource, std::uint32_t a_version,
    const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::schema::FieldDescriptor> fields;
    fields.push_back(take_value(
        cue::schema::create_field_descriptor(make_field_id(1U, a_assertContext), "health", a_assertContext)));
    fields.push_back(
        take_value(cue::schema::create_field_descriptor(make_field_id(2U, a_assertContext), "name", a_assertContext)));
    std::vector<cue::schema::FieldId> reserved;
    auto descriptor = take_value(cue::schema::create_type_descriptor(
        make_type_id(a_assertContext), "Cue.Test.Component", make_version(a_version, a_assertContext),
        std::move(fields), std::move(reserved), a_assertContext));
    cue::schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
    require(builder.add_type(std::move(descriptor)).has_value());
    return take_value(builder.seal());
}

/// @brief Test用Component Value Schema Registryを構築する
[[nodiscard]] cue::scene::ComponentValueSchemaRegistry make_value_registry(
    const cue::schema::SchemaRegistry &a_registry, std::uint32_t a_version,
    const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::scene::FieldKindBinding> bindings{
        {make_field_id(1U, a_assertContext), cue::scene::FieldValueKind::SignedInteger},
        {make_field_id(2U, a_assertContext), cue::scene::FieldValueKind::String}};
    std::vector<cue::scene::ComponentValueSchema> schemas;
    schemas.push_back(take_value(cue::scene::create_component_value_schema(
        make_type_id(a_assertContext), make_version(a_version, a_assertContext), std::move(bindings), a_registry,
        a_assertContext)));
    return take_value(
        cue::scene::ComponentValueSchemaRegistry::create(std::move(schemas), a_registry, a_assertContext));
}

/// @brief 指定した未知Extension Objectを持つ最小の現行Scene JSONを生成する
[[nodiscard]] std::string make_empty_scene_with_extensions(
    std::string_view a_extensions)
{
    std::string result =
        "{\"formatVersion\":1,\"sceneAssetId\":"
        "\"00000000-0000-4000-8000-000000000041\","
        "\"objects\":[],\"extensions\":";
    result.append(a_extensions);
    result.push_back('}');
    return result;
}

/// @brief 指定Member数の未知Top-level Extension Objectを生成する
[[nodiscard]] std::string make_extension_object(std::size_t a_memberCount)
{
    std::string result("{");
    for (std::size_t index = 0U; index < a_memberCount; ++index)
    {
        if (index > 0U)
        {
            result.push_back(',');
        }
        result.append("\"future");
        result.append(std::to_string(index));
        result.append("\":");
        result.append(std::to_string(index));
    }
    result.push_back('}');
    return result;
}

/// @brief Root Envelope内で指定段数だけ未知Objectを入れ子にしたScene JSONを生成する
[[nodiscard]] std::string make_nested_extension_scene(std::size_t a_depth)
{
    std::string extension;
    for (std::size_t depth = 0U; depth < a_depth; ++depth)
    {
        extension.append("{\"nested\":");
    }
    extension.append("null");
    extension.append(a_depth, '}');
    return make_empty_scene_with_extensions(extension);
}

/// @brief 指定Object Entry数のScene JSONを生成してContainer上限を検証可能にする
[[nodiscard]] std::string make_object_count_scene(std::size_t a_objectCount)
{
    std::string result =
        "{\"formatVersion\":1,\"sceneAssetId\":"
        "\"00000000-0000-4000-8000-000000000042\",\"objects\":[";
    for (std::size_t index = 0U; index < a_objectCount; ++index)
    {
        if (index > 0U)
        {
            result.push_back(',');
        }
        result.append("{}");
    }
    result.append("],\"extensions\":{}}");
    return result;
}

/// @brief Scene Parse失敗が期待Codeと開発者向け診断を持つことを検証する
void require_scene_failure(
    cue::Result<cue::scene::SceneLoadResult> &&a_result,
    cue::scene::SceneError a_expectedCode) noexcept
{
    require(!a_result.has_value());
    require(a_result.try_error() != nullptr);
    require(a_result.try_error()->code().value() ==
            static_cast<std::int64_t>(a_expectedCode));
    require(!a_result.try_error()->summary().empty());
}

/// @brief 各Container上限内の小さい値を多数持つJSON Array Fixtureを生成する
[[nodiscard]] std::string make_dense_json_array(
    std::size_t a_outerCount,
    std::size_t a_lastInnerCount =
        cue::scene::k_maximumSceneContainerElements)
{
    std::string result("[");
    for (std::size_t outer = 0U; outer < a_outerCount; ++outer)
    {
        if (outer > 0U)
        {
            result.push_back(',');
        }
        result.push_back('[');
        const std::size_t innerCount =
            outer + 1U == a_outerCount
                ? a_lastInnerCount
                : cue::scene::k_maximumSceneContainerElements;
        for (std::size_t inner = 0U; inner < innerCount; ++inner)
        {
            if (inner > 0U)
            {
                result.push_back(',');
            }
            result.push_back('0');
        }
        result.push_back(']');
    }
    result.push_back(']');
    return result;
}

/// @brief JSON内Format Versionを一回だけ次Versionへ置換する
[[nodiscard]] cue::Result<std::string> migrate_version(std::string_view a_json, std::string_view a_before,
                                                       std::string_view a_after,
                                                       const cue::AssertContext &a_assertContext) noexcept
{
    std::string result(a_json);
    const std::size_t position = result.find(a_before);
    if (position == std::string::npos)
    {
        return cue::Result<std::string>::failure(
            cue::scene::make_scene_error(a_assertContext, cue::scene::SceneError::MigrationFailed,
                                         "Migration fixture did not contain source version"));
    }
    result.replace(position, a_before.size(), a_after);
    return cue::Result<std::string>::success(std::move(result));
}

/// @brief Version 1を2へ変換するTest Migration
[[nodiscard]] cue::Result<std::string> migrate_one_to_two(std::string_view a_json,
                                                          const cue::AssertContext &a_assertContext) noexcept
{
    return migrate_version(a_json, "\"formatVersion\":1", "\"formatVersion\":2", a_assertContext);
}

/// @brief Version 2を3へ変換するTest Migration
[[nodiscard]] cue::Result<std::string> migrate_two_to_three(std::string_view a_json,
                                                            const cue::AssertContext &a_assertContext) noexcept
{
    return migrate_version(a_json, "\"formatVersion\":2", "\"formatVersion\":3", a_assertContext);
}

/// @brief Scene Migration Stepの出力上限超過を再現する
[[nodiscard]] cue::Result<std::string> migrate_to_oversized_scene(std::string_view, const cue::AssertContext &) noexcept
{
    std::string oversized = "{\"formatVersion\":2,\"padding\":\"";
    oversized.resize(16U * 1024U * 1024U + 1U, 'x');
    return cue::Result<std::string>::success(std::move(oversized));
}

/// @brief Component Field Array内のTest整数値を一回だけ置換する
[[nodiscard]] cue::Result<std::string> migrate_component_value(std::string_view a_json, std::string_view a_before,
                                                               std::string_view a_after,
                                                               const cue::AssertContext &a_assertContext) noexcept
{
    std::string result(a_json);
    const std::size_t position = result.find(a_before);
    if (position == std::string::npos)
    {
        return cue::Result<std::string>::failure(
            cue::scene::make_scene_error(a_assertContext, cue::scene::SceneError::MigrationFailed,
                                         "Component migration fixture did not contain source value"));
    }
    result.replace(position, a_before.size(), a_after);
    return cue::Result<std::string>::success(std::move(result));
}

/// @brief Component Schema Version 1のFieldをVersion 2相当へ変換する
[[nodiscard]] cue::Result<std::string> migrate_component_one_to_two(std::string_view a_json,
                                                                    const cue::AssertContext &a_assertContext) noexcept
{
    return migrate_component_value(a_json, "\"value\":-5", "\"value\":-4", a_assertContext);
}

/// @brief Component Schema Version 2のFieldをVersion 3相当へ変換する
[[nodiscard]] cue::Result<std::string> migrate_component_two_to_three(
    std::string_view a_json, const cue::AssertContext &a_assertContext) noexcept
{
    return migrate_component_value(a_json, "\"value\":-4", "\"value\":-3", a_assertContext);
}

/// @brief 最大Scene Format Version直前から最大値へ一段進める
[[nodiscard]] cue::Result<std::string> migrate_scene_to_maximum(
    std::string_view a_json, const cue::AssertContext &a_assertContext) noexcept
{
    return migrate_version(a_json, "\"formatVersion\":4294967294",
                           "\"formatVersion\":4294967295",
                           a_assertContext);
}

/// @brief 最大Component Schema Versionへの最終StepでField列を維持する
[[nodiscard]] cue::Result<std::string> migrate_component_to_maximum(
    std::string_view a_json, const cue::AssertContext &) noexcept
{
    return cue::Result<std::string>::success(std::string(a_json));
}

/// @brief Scene Round-trip、Migration、Atomic Save契約を検証する
void test_serialization() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource identitySource;
    auto registry = make_registry(identitySource, 1U, assertContext);
    auto valueRegistry = make_value_registry(*registry, 1U, assertContext);
    cue::scene::SceneMigrationRegistry migrations;
    cue::scene::ComponentMigrationRegistry componentMigrations;

    auto parsed = cue::scene::parse_scene_document(k_sceneJson, *registry, valueRegistry, migrations,
                                                   componentMigrations, assertContext);
    require(parsed.has_value());
    constexpr std::string_view k_parentMarker = "\"parentObjectId\": null";
    std::string forwardParentScene(k_sceneJson);
    const std::size_t firstParent = forwardParentScene.find(k_parentMarker);
    require(firstParent != std::string::npos);
    forwardParentScene.replace(
        firstParent, k_parentMarker.size(),
        "\"parentObjectId\": \"00000000-0000-4000-8000-000000000001\"");
    require(cue::scene::parse_scene_document(
                forwardParentScene, *registry, valueRegistry, migrations,
                componentMigrations, assertContext)
                .has_value());
    std::string cycleScene(forwardParentScene);
    const std::size_t secondParent = cycleScene.find(k_parentMarker);
    require(secondParent != std::string::npos);
    cycleScene.replace(
        secondParent, k_parentMarker.size(),
        "\"parentObjectId\": \"00000000-0000-4000-8000-000000000002\"");
    require(!cue::scene::parse_scene_document(
                 cycleScene, *registry, valueRegistry, migrations,
                 componentMigrations, assertContext)
                 .has_value());
    std::string danglingParentScene(k_sceneJson);
    danglingParentScene.replace(
        danglingParentScene.find(k_parentMarker), k_parentMarker.size(),
        "\"parentObjectId\": \"00000000-0000-4000-8000-000000000099\"");
    require(!cue::scene::parse_scene_document(
                 danglingParentScene, *registry, valueRegistry, migrations,
                 componentMigrations, assertContext)
                 .has_value());
    std::string duplicateComponentScene(k_sceneJson);
    const std::size_t duplicateComponentPosition = duplicateComponentScene.find(
        "00000000-0000-4000-8000-000000000004");
    require(duplicateComponentPosition != std::string::npos);
    duplicateComponentScene.replace(
        duplicateComponentPosition, 36U,
        "00000000-0000-4000-8000-000000000003");
    require(!cue::scene::parse_scene_document(
                 duplicateComponentScene, *registry, valueRegistry,
                 migrations, componentMigrations, assertContext)
                 .has_value());
    const std::string excessiveNodeJson = make_dense_json_array(64U);
    require(!cue::scene::parse_scene_document(
                 excessiveNodeJson, *registry, valueRegistry, migrations,
                 componentMigrations, assertContext)
                 .has_value());
    require(!cue::scene::OpaqueFieldData::create(
                 make_field_id(99U, assertContext), excessiveNodeJson,
                 assertContext)
                 .has_value());
    const std::string exactNodeBudgetJson =
        make_dense_json_array(64U, 4031U);
    require(!cue::scene::OpaqueFieldData::create(
                 make_field_id(99U, assertContext), exactNodeBudgetJson,
                 assertContext)
                 .has_value());

    const std::string densePayload =
        std::string("{\"dense\":") + make_dense_json_array(32U) + "}";
    auto aggregateDocument = cue::scene::SceneDocument::create(
        take_value(cue::scene::SceneAssetId::parse(
            "00000000-0000-4000-8000-000000000031", assertContext)),
        assertContext);
    const auto aggregateObjectId = take_value(cue::scene::ObjectId::parse(
        "00000000-0000-4000-8000-000000000032", assertContext));
    require(aggregateDocument
                .add_object(aggregateObjectId, "Dense", true, std::nullopt,
                            cue::math::Transform{})
                .has_value());
    const auto opaqueTypeId = take_value(cue::schema::TypeId::parse(
        "20000000-0000-4000-8000-000000000001", assertContext));
    const auto opaqueVersion = make_version(1U, assertContext);
    for (std::uint32_t index = 1U; index <= 2U; ++index)
    {
        const auto componentId =
            take_value(cue::scene::ComponentInstanceId::parse(
                index == 1U
                    ? "00000000-0000-4000-8000-000000000033"
                    : "00000000-0000-4000-8000-000000000034",
                assertContext));
        auto opaque = take_value(cue::scene::OpaqueComponentData::create(
            componentId, opaqueTypeId, opaqueVersion, densePayload, *registry,
            assertContext));
        require(aggregateDocument
                    .add_component(
                        aggregateObjectId,
                        cue::scene::SceneComponent::opaque(std::move(opaque)))
                    .has_value());
    }
    require(!cue::scene::serialize_scene_document(aggregateDocument,
                                                   assertContext)
                 .has_value());
    cue::schema::SchemaRegistryIdentitySource mismatchedIdentitySource;
    auto mismatchedRegistry = make_registry(mismatchedIdentitySource, 1U, assertContext);
    auto mismatched = cue::scene::parse_scene_document(k_sceneJson, *mismatchedRegistry, valueRegistry, migrations,
                                                       componentMigrations, assertContext);
    require(!mismatched.has_value());
    require(mismatched.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::InvalidComponentData));
    auto first = cue::scene::serialize_scene_document(parsed.try_value()->document(), assertContext);
    auto second = cue::scene::serialize_scene_document(parsed.try_value()->document(), assertContext);
    require(first && second && *first.try_value() == *second.try_value());
    auto reparsed = cue::scene::parse_scene_document(*first.try_value(), *registry, valueRegistry, migrations,
                                                     componentMigrations, assertContext);
    require(reparsed.has_value());
    auto roundTrip = cue::scene::serialize_scene_document(reparsed.try_value()->document(), assertContext);
    require(roundTrip && *roundTrip.try_value() == *first.try_value());
    require(roundTrip.try_value()->find("\"fieldId\":1") < roundTrip.try_value()->find("\"fieldId\":2"));
    require(roundTrip.try_value()->find("\"fieldId\":2") < roundTrip.try_value()->find("\"fieldId\":99"));
    require(roundTrip.try_value()->find("\"objectId\":\"00000000-0000-4000-8000-000000000001\"") <
            roundTrip.try_value()->find("\"objectId\":\"00000000-0000-4000-8000-000000000002\""));
    require(roundTrip.try_value()->find("\"sample\"") != std::string::npos);
    require(roundTrip.try_value()->find("\"futureMetadata\"") != std::string::npos);

    auto invalidNameDocument = cue::scene::SceneDocument::create(
        take_value(cue::scene::SceneAssetId::parse(
            "00000000-0000-4000-8000-000000000021", assertContext)),
        assertContext);
    auto invalidNameId = take_value(cue::scene::ObjectId::parse(
        "00000000-0000-4000-8000-000000000022", assertContext));
    const std::string invalidUtf8(1U, static_cast<char>(0xC3U));
    const auto invalidName = invalidNameDocument.add_object(
        invalidNameId, invalidUtf8, true, std::nullopt,
        cue::math::Transform{});
    require(!invalidName.has_value());
    require(invalidName.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::InvalidName));
    require(invalidNameDocument.object_count() == 0U);
    std::string oversizedName(cue::scene::k_maximumSceneStringBytes + 1U,
                              'n');
    const auto oversizedAdd = invalidNameDocument.add_object(
        invalidNameId, oversizedName, true, std::nullopt,
        cue::math::Transform{});
    require(!oversizedAdd.has_value());
    require(oversizedAdd.try_error()->code().value() ==
            static_cast<std::int64_t>(
                cue::scene::SceneError::ResourceLimitExceeded));
    require(invalidNameDocument.object_count() == 0U);
    require(invalidNameDocument
                .add_object(invalidNameId, "Valid", true, std::nullopt,
                            cue::math::Transform{})
                .has_value());
    const auto invalidRename =
        invalidNameDocument.rename_object(invalidNameId, invalidUtf8);
    require(!invalidRename.has_value());
    require(invalidRename.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::InvalidName));
    require(invalidNameDocument.find_object(invalidNameId)->name() == "Valid");
    const auto oversizedRename =
        invalidNameDocument.rename_object(invalidNameId, oversizedName);
    require(!oversizedRename.has_value());
    require(oversizedRename.try_error()->code().value() ==
            static_cast<std::int64_t>(
                cue::scene::SceneError::ResourceLimitExceeded));
    require(invalidNameDocument.find_object(invalidNameId)->name() == "Valid");
    require(cue::scene::serialize_scene_document(invalidNameDocument,
                                                 assertContext)
                .has_value());

    const std::string future = std::string("{\"formatVersion\":2,\"sceneAssetId\":") +
                               "\"00000000-0000-4000-8000-000000000001\",\"objects\":[],"
                               "\"extensions\":{}}";
    const auto futureResult = cue::scene::parse_scene_document(future, *registry, valueRegistry, migrations,
                                                               componentMigrations, assertContext);
    require(!futureResult.has_value());
    require(futureResult.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::UnsupportedFormatVersion));

    cue::scene::SceneMigrationRegistry chain;
    require(chain.add_step(1U, &migrate_one_to_two, assertContext).has_value());
    require(chain.add_step(2U, &migrate_two_to_three, assertContext).has_value());
    const std::string migrationSource = "{\"formatVersion\":1}";
    const auto migrated = chain.migrate(migrationSource, 1U, 3U, assertContext);
    require(migrated && migrated.try_value()->find("\"formatVersion\":3") != std::string::npos);
    require(migrationSource == "{\"formatVersion\":1}");
    cue::scene::SceneMigrationRegistry missing;
    require(missing.add_step(1U, &migrate_one_to_two, assertContext).has_value());
    require(!missing.migrate("{\"formatVersion\":1}", 1U, 3U, assertContext).has_value());
    cue::scene::SceneMigrationRegistry oversizedMigration;
    require(oversizedMigration.add_step(1U, &migrate_to_oversized_scene, assertContext).has_value());
    require(!oversizedMigration.migrate("{\"formatVersion\":1}", 1U, 2U, assertContext).has_value());
    constexpr std::uint32_t k_beforeMaximumVersion =
        std::numeric_limits<std::uint32_t>::max() - 1U;
    cue::scene::SceneMigrationRegistry maximumSceneMigration;
    require(maximumSceneMigration
                .add_step(k_beforeMaximumVersion, &migrate_scene_to_maximum,
                          assertContext)
                .has_value());
    require(maximumSceneMigration
                .migrate("{\"formatVersion\":4294967294}",
                         k_beforeMaximumVersion,
                         std::numeric_limits<std::uint32_t>::max(),
                         assertContext)
                .has_value());

    cue::schema::SchemaRegistryIdentitySource migratedIdentitySource;
    auto migratedRegistry = make_registry(migratedIdentitySource, 3U, assertContext);
    auto migratedValueRegistry = make_value_registry(*migratedRegistry, 3U, assertContext);
    cue::scene::ComponentMigrationRegistry componentChain;
    require(componentChain.add_step(make_type_id(assertContext), 1U, &migrate_component_one_to_two, assertContext)
                .has_value());
    require(componentChain.add_step(make_type_id(assertContext), 2U, &migrate_component_two_to_three, assertContext)
                .has_value());
    auto migratedComponent = cue::scene::parse_scene_document(
        k_componentMigrationJson, *migratedRegistry, migratedValueRegistry, migrations, componentChain, assertContext);
    require(migratedComponent.has_value());
    auto migratedComponentText =
        cue::scene::serialize_scene_document(migratedComponent.try_value()->document(), assertContext);
    require(migratedComponentText.has_value());
    require(migratedComponentText.try_value()->find("\"schemaVersion\":3") != std::string::npos);
    require(migratedComponentText.try_value()->find("\"fieldId\":1,\"value\":-3") != std::string::npos);
    cue::scene::ComponentMigrationRegistry missingComponentStep;
    require(missingComponentStep.add_step(make_type_id(assertContext), 1U, &migrate_component_one_to_two, assertContext)
                .has_value());
    auto rejectedComponent =
        cue::scene::parse_scene_document(k_componentMigrationJson, *migratedRegistry, migratedValueRegistry, migrations,
                                         missingComponentStep, assertContext);
    require(!rejectedComponent.has_value());
    require(rejectedComponent.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::MissingMigrationStep));
    cue::scene::ComponentMigrationRegistry maximumComponentMigration;
    require(maximumComponentMigration
                .add_step(make_type_id(assertContext), k_beforeMaximumVersion,
                          &migrate_component_to_maximum, assertContext)
                .has_value());
    require(maximumComponentMigration
                .migrate(make_type_id(assertContext), "[]", k_beforeMaximumVersion,
                         std::numeric_limits<std::uint32_t>::max(), assertContext)
                .has_value());

    MemoryFilesystemRoot filesystem(assertContext);
    filesystem.set("Scenes/Main.cuescene", "original");
    filesystem.set_recovery_backup("Scenes/Main.cuescene", "prior-backup");
    auto path = take_value(cue::RelativePath::parse("Scenes/Main.cuescene", assertContext));
    filesystem.fail_main_write(true);
    auto failed = cue::scene::save_scene_document(filesystem, path, parsed.try_value()->document(), *registry,
                                                  valueRegistry, migrations, componentMigrations, assertContext);
    require(failed.status() == cue::scene::SceneSaveStatus::NotPublished);
    require(filesystem.text("Scenes/Main.cuescene") == "original");
    require(filesystem.recovery_backup_text("Scenes/Main.cuescene") == "prior-backup");
    filesystem.fail_main_write(false);
    filesystem.make_main_write_durability_unknown(true);
    auto uncertain = cue::scene::save_scene_document(filesystem, path, parsed.try_value()->document(), *registry,
                                                     valueRegistry, migrations, componentMigrations, assertContext);
    require(uncertain.status() == cue::scene::SceneSaveStatus::PublishedButDurabilityUnknown);
    auto uncertainBackupBytes = uncertain.take_recovery_backup_bytes();
    require(uncertainBackupBytes.has_value());
    require(std::string_view(reinterpret_cast<const char *>(uncertainBackupBytes->data()),
                             uncertainBackupBytes->size()) == "original");
    require(filesystem.text("Scenes/Main.cuescene") != "original");
    require(filesystem.recovery_backup_text("Scenes/Main.cuescene") == "prior-backup");
    filesystem.make_main_write_durability_unknown(false);
    auto saved = cue::scene::save_scene_document(filesystem, path, parsed.try_value()->document(), *registry,
                                                 valueRegistry, migrations, componentMigrations, assertContext);
    require(saved.status() == cue::scene::SceneSaveStatus::Committed);
    auto loaded = cue::scene::load_scene_document(filesystem, path, *registry, valueRegistry, migrations,
                                                  componentMigrations, assertContext);
    require(loaded.has_value());

    MemoryFilesystemRoot namespaceFilesystem(assertContext);
    namespaceFilesystem.set("Scenes/Main.cuescene", "namespace-original");
    namespaceFilesystem.set("Scenes/Main.cuescene.backup", "user-scene");
    auto namespaceSaved =
        cue::scene::save_scene_document(namespaceFilesystem, path, parsed.try_value()->document(), *registry,
                                        valueRegistry, migrations, componentMigrations, assertContext);
    require(namespaceSaved.status() == cue::scene::SceneSaveStatus::Committed);
    require(namespaceFilesystem.text("Scenes/Main.cuescene.backup") == "user-scene");
    require(namespaceFilesystem.recovery_backup_text("Scenes/Main.cuescene") == "namespace-original");

    MemoryFilesystemRoot backupFailureFilesystem(assertContext);
    backupFailureFilesystem.set("Scenes/Main.cuescene", "backup-failure-original");
    backupFailureFilesystem.set_recovery_backup("Scenes/Main.cuescene", "preserved-backup");
    backupFailureFilesystem.fail_backup_write(true);
    auto backupFailed =
        cue::scene::save_scene_document(backupFailureFilesystem, path, parsed.try_value()->document(), *registry,
                                        valueRegistry, migrations, componentMigrations, assertContext);
    require(backupFailed.status() == cue::scene::SceneSaveStatus::PublishedButVerificationFailed);
    require(backupFailureFilesystem.text("Scenes/Main.cuescene") != "backup-failure-original");
    require(backupFailureFilesystem.recovery_backup_text("Scenes/Main.cuescene") == "preserved-backup");

    MemoryFilesystemRoot backupUncertainFilesystem(assertContext);
    backupUncertainFilesystem.set("Scenes/Main.cuescene", "backup-uncertain-original");
    backupUncertainFilesystem.make_backup_write_durability_unknown(true);
    auto backupUncertain =
        cue::scene::save_scene_document(backupUncertainFilesystem, path, parsed.try_value()->document(), *registry,
                                        valueRegistry, migrations, componentMigrations, assertContext);
    require(backupUncertain.status() == cue::scene::SceneSaveStatus::PublishedButBackupDurabilityUnknown);
    auto retainedBackupBytes = backupUncertain.take_recovery_backup_bytes();
    require(retainedBackupBytes.has_value());
    require(std::string_view(reinterpret_cast<const char *>(retainedBackupBytes->data()), retainedBackupBytes->size()) ==
            "backup-uncertain-original");
    require(backupUncertainFilesystem.text("Scenes/Main.cuescene") != "backup-uncertain-original");
    require(backupUncertainFilesystem.recovery_backup_text("Scenes/Main.cuescene") == "backup-uncertain-original");

    MemoryFilesystemRoot verificationFilesystem(assertContext);
    verificationFilesystem.set("Scenes/Main.cuescene", "verification-original");
    verificationFilesystem.fail_read_after_main_write(true);
    auto verificationFailed =
        cue::scene::save_scene_document(verificationFilesystem, path, parsed.try_value()->document(), *registry,
                                        valueRegistry, migrations, componentMigrations, assertContext);
    require(verificationFailed.status() == cue::scene::SceneSaveStatus::PublishedButVerificationFailed);
    require(verificationFailed.try_error() != nullptr);
    require(verificationFailed.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::PublishedVerificationFailed));
    require(verificationFilesystem.text("Scenes/Main.cuescene") != "verification-original");
    require(verificationFilesystem.recovery_backup_text("Scenes/Main.cuescene") == "verification-original");
}

/// @brief 破損Scene、入力上限、未知Data保持、既存Document不変契約をまとめて検証する
void test_malformed_scene_inputs() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource identitySource;
    auto registry = make_registry(identitySource, 1U, assertContext);
    auto valueRegistry = make_value_registry(*registry, 1U, assertContext);
    cue::scene::SceneMigrationRegistry migrations;
    cue::scene::ComponentMigrationRegistry componentMigrations;

    /// @brief 同じRegistry集合で一つのMalformed Fixtureを解析する
    auto parse = [&](std::string_view a_json) noexcept
    {
        return cue::scene::parse_scene_document(
            a_json, *registry, valueRegistry, migrations,
            componentMigrations, assertContext);
    };

    require_scene_failure(parse(""), cue::scene::SceneError::InvalidFormat);
    std::string bomScene("\xEF\xBB\xBF");
    bomScene.append("{}");
    require_scene_failure(parse(bomScene),
                          cue::scene::SceneError::InvalidFormat);
    require_scene_failure(parse("{"), cue::scene::SceneError::InvalidFormat);
    require_scene_failure(
        parse("{\"formatVersion\":1,\"formatVersion\":1,"
              "\"sceneAssetId\":\"00000000-0000-4000-8000-000000000041\","
              "\"objects\":[],\"extensions\":{}}"),
        cue::scene::SceneError::InvalidFormat);
    auto trailingScene = make_empty_scene_with_extensions("{}");
    trailingScene.push_back('x');
    require_scene_failure(parse(trailingScene),
                          cue::scene::SceneError::InvalidFormat);
    auto invalidUtf8Scene = make_empty_scene_with_extensions(
        "{\"future\":\"valid\"}");
    const std::size_t validText = invalidUtf8Scene.find("valid");
    require(validText != std::string::npos);
    invalidUtf8Scene[validText] = static_cast<char>(0xC3U);
    invalidUtf8Scene.erase(validText + 1U, 4U);
    require_scene_failure(parse(invalidUtf8Scene),
                          cue::scene::SceneError::InvalidFormat);

    const auto overNested = make_nested_extension_scene(
        cue::scene::k_maximumSceneNestingDepth + 1U);
    require_scene_failure(parse(overNested),
                          cue::scene::SceneError::InvalidFormat);
    const auto excessiveObjects = make_object_count_scene(
        cue::scene::k_maximumSceneObjectCount + 1U);
    require_scene_failure(parse(excessiveObjects),
                          cue::scene::SceneError::InvalidFormat);
    std::string oversizedScene(cue::scene::k_maximumSceneBytes + 1U, 'x');
    require_scene_failure(parse(oversizedScene),
                          cue::scene::SceneError::ResourceLimitExceeded);

    const auto maximumExtensions = make_extension_object(
        cue::scene::k_maximumSceneContainerElements);
    const auto maximumUnknownScene =
        make_empty_scene_with_extensions(maximumExtensions);
    auto preservedUnknown = parse(maximumUnknownScene);
    require(preservedUnknown.has_value());
    auto preservedText = cue::scene::serialize_scene_document(
        preservedUnknown.try_value()->document(), assertContext);
    require(preservedText.has_value());
    const auto lastExtensionIndex =
        cue::scene::k_maximumSceneContainerElements - 1U;
    const auto lastExtension = std::string("\"future") +
                               std::to_string(lastExtensionIndex) + "\":" +
                               std::to_string(lastExtensionIndex);
    require(preservedText.try_value()->find(lastExtension) !=
            std::string::npos);
    const auto excessiveExtensions = make_extension_object(
        cue::scene::k_maximumSceneContainerElements + 1U);
    require_scene_failure(
        parse(make_empty_scene_with_extensions(excessiveExtensions)),
        cue::scene::SceneError::InvalidFormat);

    auto existing = parse(k_sceneJson);
    require(existing.has_value());
    auto before = cue::scene::serialize_scene_document(
        existing.try_value()->document(), assertContext);
    require(before.has_value());
    MemoryFilesystemRoot filesystem(assertContext);
    filesystem.set("Scenes/Malformed.cuescene", overNested);
    const auto path = take_value(cue::RelativePath::parse(
        "Scenes/Malformed.cuescene", assertContext));
    auto failedLoad = cue::scene::load_scene_document(
        filesystem, path, *registry, valueRegistry, migrations,
        componentMigrations, assertContext);
    require_scene_failure(std::move(failedLoad),
                          cue::scene::SceneError::InvalidFormat);
    auto after = cue::scene::serialize_scene_document(
        existing.try_value()->document(), assertContext);
    require(after.has_value());
    require(*before.try_value() == *after.try_value());
    require(filesystem.text("Scenes/Malformed.cuescene") == overNested);
}
} // namespace

/// @brief Cue.Scene SerializerのRound-tripとStorage契約Testを実行する
int main()
{
    test_serialization();
    test_malformed_scene_inputs();
    return 0;
}

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Math/Transform.h>
#include <Cue/Runtime/Error.h>
#include <Cue/Runtime/RuntimeSceneSession.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Schema/Descriptor.h>
#include <Cue/Schema/Error.h>
#include <Cue/Schema/Registry.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_transformTypeId = "50000000-0000-4000-8000-000000000005";
constexpr std::string_view k_sceneObjectStateTypeId = "10000000-0000-4000-8000-000000000001";

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Unit Test中の通常FatalをProcess失敗へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(77);
    }

    /// @brief Unit Test中のEmergency FatalをProcess失敗へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(78);
    }
};

/// @brief 条件が偽ならRuntime Unit Testを失敗終了する
void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::_Exit(2);
    }
}

/// @brief Resultが失敗ならRuntime Unit Testを失敗終了する
template <typename T> void require(const cue::Result<T> &a_result) noexcept
{
    require(a_result.has_value());
}

/// @brief 成功Resultから所有Valueを取り出す
template <typename T> [[nodiscard]] T take_value(cue::Result<T> &&a_result) noexcept
{
    require(a_result.has_value());
    return std::move(*a_result.try_value());
}

/// @brief Canonical UUIDからTest用TypeIdを生成する
[[nodiscard]] cue::schema::TypeId make_type_id(std::string_view a_text,
                                               const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::TypeId::parse(a_text, a_assertContext));
}

/// @brief Fieldを持たないTest用Type Descriptorを生成する
[[nodiscard]] cue::schema::TypeDescriptor make_type_descriptor(std::string_view a_typeId, std::string_view a_name,
                                                               const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::schema::FieldDescriptor> fields;
    std::vector<cue::schema::FieldId> reserved;
    return take_value(
        cue::schema::create_type_descriptor(make_type_id(a_typeId, a_assertContext), a_name,
                                            take_value(cue::schema::SchemaVersion::create(1U, a_assertContext)),
                                            std::move(fields), std::move(reserved), a_assertContext));
}

/// @brief TransformとSceneObjectStateを登録したSeal済みRegistryを生成する
[[nodiscard]] std::unique_ptr<cue::schema::SchemaRegistry> make_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource, const cue::AssertContext &a_assertContext) noexcept
{
    cue::schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
    require(builder.add_type(make_type_descriptor(k_transformTypeId, "Cue.Core.Transform", a_assertContext)));
    require(builder.add_type(
        make_type_descriptor(k_sceneObjectStateTypeId, "Cue.Scene.SceneObjectState", a_assertContext)));
    return take_value(builder.seal());
}

/// @brief 二Objectを持つMutable DocumentをTest用に生成する
[[nodiscard]] cue::scene::SceneDocument make_document(const cue::AssertContext &a_assertContext) noexcept
{
    cue::scene::SceneDocument document = cue::scene::SceneDocument::create(
        take_value(cue::scene::SceneAssetId::parse("70000000-0000-4000-8000-000000000001", a_assertContext)),
        a_assertContext);
    require(document.add_object(
        take_value(cue::scene::ObjectId::parse("70000000-0000-4000-8000-000000000002", a_assertContext)), "Root", true,
        std::nullopt, cue::math::Transform{}));
    require(document.add_object(
        take_value(cue::scene::ObjectId::parse("70000000-0000-4000-8000-000000000003", a_assertContext)), "Child", true,
        take_value(cue::scene::ObjectId::parse("70000000-0000-4000-8000-000000000002", a_assertContext)),
        cue::math::Transform{}));
    return document;
}

/// @brief Snapshot非保持、終了冪等性、World Identity分離、開始失敗後の再開始を検証する
void test_scene_session_lifecycle(cue::AssertContext &a_assertContext) noexcept
{
    cue::schema::SchemaRegistryIdentitySource schemaIdentitySource;
    std::unique_ptr<cue::schema::SchemaRegistry> registry = make_registry(schemaIdentitySource, a_assertContext);
    cue::game_core::WorldIdentitySource worldIdentitySource;
    cue::scene::SceneDocument document = make_document(a_assertContext);

    std::unique_ptr<cue::runtime::RuntimeSceneSession> firstSession;
    {
        cue::scene::SceneSnapshot snapshot = take_value(cue::scene::create_scene_snapshot(document, a_assertContext));
        auto started = cue::runtime::RuntimeSceneSession::start(
            snapshot, worldIdentitySource, *registry, make_type_id(k_transformTypeId, a_assertContext),
            make_type_id(k_sceneObjectStateTypeId, a_assertContext), a_assertContext);
        require(started.has_value());
        firstSession = std::move(*started.try_value());
    }

    require(firstSession->state() == cue::runtime::RuntimeSceneSessionState::Running);
    require(firstSession->entity_count() == 2U);
    const std::uint64_t firstWorldId = firstSession->world_id();
    require(firstWorldId != 0U);

    require(document.add_object(
        take_value(cue::scene::ObjectId::parse("70000000-0000-4000-8000-000000000004", a_assertContext)), "Later", true,
        std::nullopt, cue::math::Transform{}));
    require(firstSession->entity_count() == 2U);
    require(firstSession->end());
    require(firstSession->end());
    require(firstSession->state() == cue::runtime::RuntimeSceneSessionState::Stopped);
    require(firstSession->entity_count() == 0U);

    cue::scene::SceneSnapshot restartedSnapshot =
        take_value(cue::scene::create_scene_snapshot(document, a_assertContext));
    auto restarted = cue::runtime::RuntimeSceneSession::start(
        restartedSnapshot, worldIdentitySource, *registry, make_type_id(k_transformTypeId, a_assertContext),
        make_type_id(k_sceneObjectStateTypeId, a_assertContext), a_assertContext);
    require(restarted.has_value());
    require((*restarted.try_value())->world_id() != firstWorldId);
    require((*restarted.try_value())->entity_count() == 3U);
    require((*restarted.try_value())->end());

    auto invalidType = cue::runtime::RuntimeSceneSession::start(
        restartedSnapshot, worldIdentitySource, *registry, make_type_id(k_transformTypeId, a_assertContext),
        make_type_id("90000000-0000-4000-8000-000000000009", a_assertContext), a_assertContext);
    require(!invalidType);
    require(invalidType.try_error()->code().domain() == "Cue.Runtime");
    require(invalidType.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::runtime::RuntimeError::SceneSessionStartFailed));
    require(invalidType.try_error()->root_code().domain() == "Cue.Schema");
    require(invalidType.try_error()->root_code().value() ==
            static_cast<std::int64_t>(cue::schema::SchemaError::NotFound));

    auto afterFailure = cue::runtime::RuntimeSceneSession::start(
        restartedSnapshot, worldIdentitySource, *registry, make_type_id(k_transformTypeId, a_assertContext),
        make_type_id(k_sceneObjectStateTypeId, a_assertContext), a_assertContext);
    require(afterFailure.has_value());
    require((*afterFailure.try_value())->world_id() != firstWorldId);
    require((*afterFailure.try_value())->end());
}
} // namespace

/// @brief Runtime Scene SessionのHeadless所有境界と失敗後回復を検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    test_scene_session_lifecycle(assertContext);
    return 0;
}

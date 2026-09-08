#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/GameCore/RuntimeSystemRegistry.h>
#include <Cue/GameCore/RuntimeWorld.h>
#include <Cue/GameCore/World.h>
#include <Cue/Math/Transform.h>
#include <Cue/Schema/Descriptor.h>
#include <Cue/Schema/Registry.h>
#include <Cue/Schema/Types.h>

#include <cstdlib>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
class ProcessFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief 期待した再入拒否を Process の非zero終了へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(42);
    }

    /// @brief 診断Message付きの再入拒否を Process の非zero終了へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(42);
    }
};

class ReentrantDestructorComponent final
{
  public:
    /// @brief Component 破棄中に同じ World の Structural API を呼ぶ検証対象を構築する
    ReentrantDestructorComponent(cue::game_core::World &a_world, bool a_reenterOnDestruction) noexcept
        : m_world(&a_world), m_isOwner(true), m_reenterOnDestruction(a_reenterOnDestruction)
    {
    }

    /// @brief 再入検証責任の複製を禁止する
    ReentrantDestructorComponent(const ReentrantDestructorComponent &) = delete;
    /// @brief 再入検証責任の複製代入を禁止する
    ReentrantDestructorComponent &operator=(const ReentrantDestructorComponent &) = delete;

    /// @brief Storageへの移動時に再入検証責任を一度だけ移す
    ReentrantDestructorComponent(ReentrantDestructorComponent &&a_other) noexcept
        : m_world(a_other.m_world), m_isOwner(a_other.m_isOwner), m_reenterOnDestruction(a_other.m_reenterOnDestruction)
    {
        a_other.m_isOwner = false;
    }

    /// @brief Component が Move 代入を要求されないことを固定する
    ReentrantDestructorComponent &operator=(ReentrantDestructorComponent &&) = delete;

    /// @brief 破棄中の再入が成功する誤実装を Process 成功として露出させる
    ~ReentrantDestructorComponent() noexcept
    {
        if (m_isOwner && m_reenterOnDestruction)
        {
            auto result = m_world->create_entity();
            static_cast<void>(result);
        }
    }

  private:
    cue::game_core::World *m_world;
    bool m_isOwner;
    bool m_reenterOnDestruction;
};

struct EmptyComponent final
{
};

/// @brief Test用の検証済みTypeIdを生成する
[[nodiscard]] cue::schema::TypeId make_type_id(std::string_view a_text, const cue::AssertContext &a_assertContext)
{
    auto result = cue::schema::TypeId::parse(a_text, a_assertContext);

    if (!result)
    {
        std::_Exit(2);
    }

    return std::move(*result.try_value());
}

/// @brief 指定ScenarioのQuery、Mutation、Thread違反をProcess単位で検証する
[[nodiscard]] int run_process_test(std::string_view a_mode) noexcept
{
    ProcessFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    if (a_mode == "RuntimeSystemRegistryWrongThread")
    {
        cue::game_core::RuntimeSystemRegistry systemRegistry(assertContext);
        /// @brief Registry Owner以外のThreadから状態参照を試行する
        std::thread worker([&systemRegistry]() noexcept { static_cast<void>(systemRegistry.state()); });
        worker.join();
        return 0;
    }

    cue::schema::SchemaRegistryIdentitySource schemaIdentitySource;
    cue::schema::SchemaRegistryBuilder builder(schemaIdentitySource, assertContext);
    auto version = cue::schema::SchemaVersion::create(1U, assertContext);

    if (!version)
    {
        return 3;
    }

    std::vector<cue::schema::FieldDescriptor> fields;
    std::vector<cue::schema::FieldId> reservedFieldIds;
    const auto typeId = make_type_id("30000000-0000-4000-8000-000000000003", assertContext);
    const auto emptyTypeId = make_type_id("40000000-0000-4000-8000-000000000004", assertContext);
    auto descriptor =
        cue::schema::create_type_descriptor(typeId, "Cue.Test.ReentrantDestructor", std::move(*version.try_value()),
                                            std::move(fields), std::move(reservedFieldIds), assertContext);

    if (!descriptor || !builder.add_type(std::move(*descriptor.try_value())))
    {
        return 4;
    }

    auto transformVersion = cue::schema::SchemaVersion::create(1U, assertContext);
    const auto transformTypeId = make_type_id("50000000-0000-4000-8000-000000000005", assertContext);
    std::vector<cue::schema::FieldDescriptor> transformFields;
    std::vector<cue::schema::FieldId> transformReservedFieldIds;

    if (!transformVersion)
    {
        return 4;
    }

    auto transformDescriptor = cue::schema::create_type_descriptor(
        transformTypeId, "Cue.Core.Transform", std::move(*transformVersion.try_value()), std::move(transformFields),
        std::move(transformReservedFieldIds), assertContext);

    if (!transformDescriptor || !builder.add_type(std::move(*transformDescriptor.try_value())))
    {
        return 4;
    }

    auto emptyVersion = cue::schema::SchemaVersion::create(1U, assertContext);
    std::vector<cue::schema::FieldDescriptor> emptyFields;
    std::vector<cue::schema::FieldId> emptyReservedFieldIds;

    if (!emptyVersion)
    {
        return 4;
    }

    auto emptyDescriptor =
        cue::schema::create_type_descriptor(emptyTypeId, "Cue.Test.Empty", std::move(*emptyVersion.try_value()),
                                            std::move(emptyFields), std::move(emptyReservedFieldIds), assertContext);

    if (!emptyDescriptor || !builder.add_type(std::move(*emptyDescriptor.try_value())))
    {
        return 4;
    }

    auto registry = builder.seal();

    if (!registry)
    {
        return 5;
    }

    cue::game_core::WorldIdentitySource worldIdentitySource;
    auto world = cue::game_core::World::create(worldIdentitySource, **registry.try_value(), assertContext);

    if (!world)
    {
        return 6;
    }

    auto componentType = (*world.try_value())->register_component<ReentrantDestructorComponent>(typeId);
    auto emptyComponentType = (*world.try_value())->register_component<EmptyComponent>(emptyTypeId);
    auto entity = (*world.try_value())->create_entity();

    if (!componentType || !emptyComponentType || !entity)
    {
        return 7;
    }

    auto component = (*world.try_value())
                         ->add_component(*componentType.try_value(), *entity.try_value(), **world.try_value(),
                                         a_mode == "StructuralReentry");

    if (!component)
    {
        return 8;
    }

    if (a_mode == "StructuralReentry")
    {
        auto destroy = (*world.try_value())->destroy_entity(*entity.try_value());
        static_cast<void>(destroy);
        return 0;
    }

    if (a_mode == "QueryMutation")
    {
        /// @brief Query中に直接Structural Mutationを試行する
        auto callback = [&world](cue::game_core::EntityHandle, const ReentrantDestructorComponent &) noexcept
        {
            auto created = (*world.try_value())->create_entity();
            static_cast<void>(created);
        };
        auto query = (*world.try_value())->query_read(*componentType.try_value(), callback);
        static_cast<void>(query);
        return 0;
    }

    if (a_mode == "NestedQuery")
    {
        /// @brief Nested Query内側でComponentを観測する
        auto innerCallback = [](cue::game_core::EntityHandle, const ReentrantDestructorComponent &) noexcept {};
        /// @brief Query Callbackから同じWorldのQueryを再入する
        auto outerCallback = [&world, &componentType, &innerCallback](cue::game_core::EntityHandle,
                                                                      const ReentrantDestructorComponent &) noexcept
        {
            auto nested = (*world.try_value())->query_read(*componentType.try_value(), innerCallback);
            static_cast<void>(nested);
        };
        auto query = (*world.try_value())->query_read(*componentType.try_value(), outerCallback);
        static_cast<void>(query);
        return 0;
    }

    if (a_mode == "NestedEmptyQuery")
    {
        /// @brief Storage 未生成の Nested Query が早期 return せず拒否されることを検証する
        auto innerCallback = [](cue::game_core::EntityHandle, const EmptyComponent &) noexcept {};
        /// @brief Query Callback から同じ World の空 Storage Query へ再入する
        auto outerCallback = [&world, &emptyComponentType, &innerCallback](
                                 cue::game_core::EntityHandle, const ReentrantDestructorComponent &) noexcept
        {
            auto nested = (*world.try_value())->query_read(*emptyComponentType.try_value(), innerCallback);
            static_cast<void>(nested);
        };
        auto query = (*world.try_value())->query_read(*componentType.try_value(), outerCallback);
        static_cast<void>(query);
        return 0;
    }

    if (a_mode == "QueryDestruction")
    {
        /// @brief Query Callback 中の World 破棄が Storage 解放前に拒否されることを検証する
        auto callback = [&world](cue::game_core::EntityHandle, const ReentrantDestructorComponent &) noexcept
        { (*world.try_value()).reset(); };
        auto query = (*world.try_value())->query_read(*componentType.try_value(), callback);
        static_cast<void>(query);
        return 0;
    }

    if (a_mode == "QueryException")
    {
        /// @brief Query Callback 例外が Guard の Stack Unwind 後に Fatal へ移ることを検証する
        auto callback = [](cue::game_core::EntityHandle, const ReentrantDestructorComponent &) { throw 1; };
        auto query = (*world.try_value())->query_read(*componentType.try_value(), callback);
        static_cast<void>(query);
        return 0;
    }

    if (a_mode == "WrongThread")
    {
        /// @brief World Owner以外のThreadからStructural APIを呼び出す
        std::thread worker(
            [&world]() noexcept
            {
                auto created = (*world.try_value())->create_entity();
                static_cast<void>(created);
            });
        worker.join();
        return 0;
    }

    if (a_mode == "HeadlessRuntimeWorld")
    {
        auto runtime = cue::game_core::RuntimeWorld::create(worldIdentitySource, **registry.try_value(),
                                                            transformTypeId, assertContext);
        auto initialized = runtime->initialize();
        auto *commands = runtime->try_command_buffer();
        const auto *transformType = runtime->try_transform_type();

        if (!initialized || commands == nullptr || transformType == nullptr)
        {
            return 11;
        }

        auto pending = commands->create_entity();

        if (!pending || !commands->add_component(*transformType, *pending.try_value()))
        {
            return 12;
        }

        auto tick = runtime->tick();
        auto stop = runtime->request_stop();
        auto stopTick = runtime->tick();
        return tick && stop && stopTick && runtime->state() == cue::game_core::RuntimeWorldState::Shutdown ? 0 : 13;
    }

    if (a_mode == "RuntimeStopWrongThread")
    {
        auto runtime = cue::game_core::RuntimeWorld::create(worldIdentitySource, **registry.try_value(),
                                                            transformTypeId, assertContext);

        if (!runtime->initialize())
        {
            return 14;
        }

        /// @brief Runtime World の停止要求を Owner 以外の Thread から試行する
        std::thread worker(
            [&runtime]() noexcept
            {
                auto stop = runtime->request_stop();
                static_cast<void>(stop);
            });
        worker.join();
        return 0;
    }

    return 9;
}
} // namespace

/// @brief GameCoreのProcess終了を伴う失敗契約を検証する
int main(int a_argumentCount, char **a_arguments)
{
    if (a_argumentCount != 2)
    {
        return 10;
    }

    return run_process_test(a_arguments[1]);
}

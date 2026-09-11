#pragma once

#include <Cue/Runtime/RuntimeSystemFactory.h>
#include <Cue/RuntimeHost/GameModuleQueryProvider.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Schema/Registry.h>

#include <memory>
#include <utility>
#include <vector>

namespace cue::runtime_host
{
class GameModuleConnection;

/// @brief Loader方式に依存しないRuntimeHost一回分の所有Startup入力
///
/// RuntimeHostApplicationへProject Scope Owner Thread内で移動し、最終破棄まで同じThreadに留める。
/// System群はModule Connectionより先に破棄される。
class RuntimeHostStartup final
{
  public:
    /// @brief 接続済みModule所有権を分離せずScene Snapshotと束ねる
    RuntimeHostStartup(PreparedGameModule &&a_gameModule, scene::SceneSnapshot a_startupScene) noexcept
        : m_gameModule(std::move(a_gameModule.m_connection)),
          m_schemaIdentitySource(std::move(a_gameModule.m_schemaIdentitySource)),
          m_schemaRegistry(std::move(a_gameModule.m_schemaRegistry)),
          m_startupScene(std::move(a_startupScene)), m_systems(std::move(a_gameModule.m_systems))
    {
    }

    /// @brief Startup Ownerの複製を禁止する
    RuntimeHostStartup(const RuntimeHostStartup &) = delete;
    /// @brief Startup Ownerの複製代入を禁止する
    RuntimeHostStartup &operator=(const RuntimeHostStartup &) = delete;
    /// @brief 所有値を同一Thread内で移動する
    RuntimeHostStartup(RuntimeHostStartup &&) noexcept = default;
    /// @brief 古いSystemから逆順に破棄してから所有値を移動代入する
    RuntimeHostStartup &operator=(RuntimeHostStartup &&a_other) noexcept
    {
        if (this == &a_other)
        {
            return *this;
        }
        while (!m_systems.empty())
        {
            m_systems.pop_back();
        }
        m_startupScene = std::move(a_other.m_startupScene);
        m_schemaRegistry.reset();
        m_schemaIdentitySource.reset();
        m_gameModule.reset();
        m_gameModule = std::move(a_other.m_gameModule);
        m_schemaIdentitySource = std::move(a_other.m_schemaIdentitySource);
        m_schemaRegistry = std::move(a_other.m_schemaRegistry);
        m_systems = std::move(a_other.m_systems);
        return *this;
    }
    /// @brief System、Scene、Registry、Identity、Moduleの寿命順で破棄する
    ~RuntimeHostStartup() noexcept
    {
        while (!m_systems.empty())
        {
            m_systems.pop_back();
        }
    }

  private:
    friend class RuntimeHostApplication;

    /// @brief Moduleなしの固定Smoke入力をSchema所有権と寿命順に束ねる
    RuntimeHostStartup(std::shared_ptr<GameModuleConnection> a_gameModule,
                       std::unique_ptr<schema::SchemaRegistryIdentitySource> a_schemaIdentitySource,
                       std::unique_ptr<schema::SchemaRegistry> a_schemaRegistry,
                       scene::SceneSnapshot a_startupScene,
                       std::vector<runtime::RuntimeSystemRegistration> a_systems) noexcept
        : m_gameModule(std::move(a_gameModule)), m_schemaIdentitySource(std::move(a_schemaIdentitySource)),
          m_schemaRegistry(std::move(a_schemaRegistry)), m_startupScene(std::move(a_startupScene)),
          m_systems(std::move(a_systems))
    {
    }

    /// @brief Runtime Systemと共有するModule Connectionを一度だけ移す
    [[nodiscard]] std::shared_ptr<GameModuleConnection> take_game_module() noexcept
    {
        return std::move(m_gameModule);
    }

    /// @brief Schema RegistryとDense Indexより長く保持するIdentity Sourceを移す
    [[nodiscard]] std::unique_ptr<schema::SchemaRegistryIdentitySource> take_schema_identity_source() noexcept
    {
        return std::move(m_schemaIdentitySource);
    }

    /// @brief Runtime Sessionより長く保持するSchema Registryを一度だけ移す
    [[nodiscard]] std::unique_ptr<schema::SchemaRegistry> take_schema_registry() noexcept
    {
        return std::move(m_schemaRegistry);
    }

    /// @brief Runtime Session開始に使うScene Snapshotを一度だけ移す
    [[nodiscard]] scene::SceneSnapshot take_startup_scene() noexcept
    {
        return std::move(m_startupScene);
    }

    /// @brief Module Callbackを保持するSystem登録を一度だけ移す
    [[nodiscard]] std::vector<runtime::RuntimeSystemRegistration> take_systems() noexcept
    {
        return std::move(m_systems);
    }

    std::shared_ptr<GameModuleConnection> m_gameModule;
    std::unique_ptr<schema::SchemaRegistryIdentitySource> m_schemaIdentitySource;
    std::unique_ptr<schema::SchemaRegistry> m_schemaRegistry;
    scene::SceneSnapshot m_startupScene;
    std::vector<runtime::RuntimeSystemRegistration> m_systems;
};
} // namespace cue::runtime_host

#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Runtime/RuntimeSystemFactory.h>
#include <Cue/Scene/Instantiation.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::schema
{
class SchemaRegistry;
class SchemaRegistryIdentitySource;
}

namespace cue::runtime_host
{
/// @brief Game Module DLL、Project Scope Handle、検索Directory GuardをRuntime Sessionより長く所有する境界
class RuntimePackageModule
{
  public:
    /// @brief Platform固有Ownerを派生型として安全に破棄する
    virtual ~RuntimePackageModule() noexcept = default;

  protected:
    /// @brief 派生Module Ownerだけが基底部分を構築する
    RuntimePackageModule() noexcept = default;
    /// @brief DLL Ownerの複製を禁止する
    RuntimePackageModule(const RuntimePackageModule &) = delete;
    /// @brief DLL Ownerの複製代入を禁止する
    RuntimePackageModule &operator=(const RuntimePackageModule &) = delete;
    /// @brief Callbackが参照するOwner Addressを固定する
    RuntimePackageModule(RuntimePackageModule &&) = delete;
    /// @brief Callbackが参照するOwner Addressを固定する
    RuntimePackageModule &operator=(RuntimePackageModule &&) = delete;
};

/// @brief 検証済みPackageから構築したModule、Schema、Startup Scene、Systemを一括所有する起動入力
class LoadedRuntimePackage final
{
  public:
    /// @brief Package Ownerの暗黙複製を禁止する
    LoadedRuntimePackage(const LoadedRuntimePackage &) = delete;
    /// @brief Package Ownerの暗黙複製代入を禁止する
    LoadedRuntimePackage &operator=(const LoadedRuntimePackage &) = delete;
    /// @brief Package Ownerを同一Thread内で移動する
    LoadedRuntimePackage(LoadedRuntimePackage &&) noexcept = default;
    /// @brief Package Ownerを同一Thread内で移動代入する
    LoadedRuntimePackage &operator=(LoadedRuntimePackage &&) noexcept = default;
    /// @brief System定義、Scene、Registry、Moduleの逆順で所有値を破棄する
    ~LoadedRuntimePackage() noexcept;

    /// @brief Package RootのAbsolute Native Pathを診断用に返す
    [[nodiscard]] std::string_view package_root() const noexcept;
    /// @brief Manifestで検証したProject Identityを返す
    [[nodiscard]] std::string_view project_id() const noexcept;
    /// @brief Game Module登録を含む不変Schema Registryを返す
    [[nodiscard]] const schema::SchemaRegistry &schema_registry() const noexcept;
    /// @brief Startup Scene Runtime Dataから構築した不変Snapshotを返す
    [[nodiscard]] const scene::SceneSnapshot &startup_scene() const noexcept;
    /// @brief Module Callbackを保持するSystem登録を呼出側へ一度だけ移す
    [[nodiscard]] std::vector<runtime::RuntimeSystemRegistration> take_systems() noexcept;
    /// @brief Runtime Sessionより長く保持するGame Module Ownerを呼出側へ移す
    [[nodiscard]] std::unique_ptr<RuntimePackageModule> take_module() noexcept;
    /// @brief Runtime Sessionより長く保持するSchema Registryを呼出側へ移す
    [[nodiscard]] std::unique_ptr<schema::SchemaRegistry> take_schema_registry() noexcept;
    /// @brief Runtime Session開始まで保持するStartup Scene Snapshotを呼出側へ移す
    [[nodiscard]] scene::SceneSnapshot take_startup_scene() noexcept;

  private:
    friend Result<LoadedRuntimePackage> load_runtime_package(
        schema::SchemaRegistryIdentitySource &, const AssertContext &) noexcept;

    /// @brief 全起動入力の検証成功後だけPackage Ownerを構築する
    LoadedRuntimePackage(std::string a_packageRoot, std::string a_projectId,
                         std::unique_ptr<RuntimePackageModule> a_module,
                         std::unique_ptr<schema::SchemaRegistry> a_schemaRegistry,
                         scene::SceneSnapshot a_startupScene,
                         std::vector<runtime::RuntimeSystemRegistration> a_systems) noexcept;

    std::string m_packageRoot;
    std::string m_projectId;
    std::unique_ptr<RuntimePackageModule> m_module;
    std::unique_ptr<schema::SchemaRegistry> m_schemaRegistry;
    scene::SceneSnapshot m_startupScene;
    std::vector<runtime::RuntimeSystemRegistration> m_systems;
};

/// @brief Executable親DirectoryだけからPackageを検証しRuntime起動入力を構築する
[[nodiscard]] Result<LoadedRuntimePackage> load_runtime_package(
    schema::SchemaRegistryIdentitySource &a_identitySource,
    const AssertContext &a_assertContext) noexcept;
} // namespace cue::runtime_host

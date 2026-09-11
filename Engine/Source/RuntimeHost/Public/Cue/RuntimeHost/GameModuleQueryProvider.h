#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/GameModule/GameModuleAbi.h>
#include <Cue/Runtime/RuntimeSystemFactory.h>
#include <Cue/Schema/Registry.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace cue
{
class AssertContext;
}

namespace cue::schema
{
class SchemaRegistryIdentitySource;
}

namespace cue::runtime_host
{
class RuntimeHostStartup;

/// @brief Game Module ABI v1 Query Entryの共通Function Pointer型
using GameModuleQueryFunction = CueGameModuleResult(CUE_GAME_MODULE_CALL *)(
    std::uint32_t, CueGameModuleQueryOutputV1 *, CueGameModuleDiagnosticV1 *) noexcept;

/// @brief Query、API Table、Module/System Callbackが参照するCodeを最後まで生存させるLease
///
/// Dynamic実装はDLLと依存DLLを所有し、Static実装はProcess Imageの生存期間を表す。生成Threadで取得し、
/// Runtime System停止とState破棄後に同じThreadで解放する。派生型のDestructorはModule破棄後に呼ばれる。
class GameModuleCodeLifetime
{
  public:
    virtual ~GameModuleCodeLifetime() noexcept = default;

  protected:
    GameModuleCodeLifetime() noexcept = default;
    GameModuleCodeLifetime(const GameModuleCodeLifetime &) = delete;
    GameModuleCodeLifetime &operator=(const GameModuleCodeLifetime &) = delete;
    GameModuleCodeLifetime(GameModuleCodeLifetime &&) = delete;
    GameModuleCodeLifetime &operator=(GameModuleCodeLifetime &&) = delete;
};

/// @brief Query Entryとその実行Code Leaseを不可分に渡す解決結果
struct ResolvedGameModuleQuery final
{
    GameModuleQueryFunction query = nullptr;
    std::shared_ptr<GameModuleCodeLifetime> codeLifetime;
};

/// @brief Dynamic／Static取得方式を共通ABI接続処理から分離するProvider境界
///
/// resolveはProject Scope Owner Threadから一度呼び、返却Leaseが破棄されるまでProvider由来のQuery Entryを
/// 有効に保つ。Query出力またはAPI identityが不適合な場合のError Domainは取得方式ごとの既存契約を保つ。
class GameModuleQueryProvider
{
  public:
    virtual ~GameModuleQueryProvider() noexcept = default;

    [[nodiscard]] virtual Result<ResolvedGameModuleQuery> resolve(
        const AssertContext &a_assertContext) noexcept = 0;

    /// @brief Query出力またはAPI identityの不適合を取得方式固有のErrorへ分類する
    [[nodiscard]] virtual Error make_query_contract_error(
        const AssertContext &a_assertContext, std::string_view a_summary) const noexcept = 0;

  protected:
    GameModuleQueryProvider() noexcept = default;
    GameModuleQueryProvider(const GameModuleQueryProvider &) = delete;
    GameModuleQueryProvider &operator=(const GameModuleQueryProvider &) = delete;
    GameModuleQueryProvider(GameModuleQueryProvider &&) = delete;
    GameModuleQueryProvider &operator=(GameModuleQueryProvider &&) = delete;
};

/// @brief Project Scope Module HandleをCode Leaseより先に破棄する共有Owner
///
/// connect_game_moduleを呼んだProject Scope Owner Threadで共有し、最後の参照も同じThreadで解放する。
/// DestructorはGame ModuleのdestroyModule Callbackを呼ぶため、別Threadへ最終所有権を渡してはならない。
class GameModuleConnection final
{
  public:
    GameModuleConnection(const GameModuleConnection &) = delete;
    GameModuleConnection &operator=(const GameModuleConnection &) = delete;
    GameModuleConnection(GameModuleConnection &&) = delete;
    GameModuleConnection &operator=(GameModuleConnection &&) = delete;
    ~GameModuleConnection() noexcept;

  private:
    friend class GameModuleConnectionFactory;

    GameModuleConnection(std::shared_ptr<GameModuleCodeLifetime> a_codeLifetime,
                         const CueGameModuleApiV1 &a_api, CueGameModuleHandle a_module) noexcept;

    std::shared_ptr<GameModuleCodeLifetime> m_codeLifetime;
    const CueGameModuleApiV1 *m_api;
    CueGameModuleHandle m_module;
};

/// @brief 共通ABI接続後のModule Owner、Schema Registry、Runtime System登録を一括所有する
///
/// connect_game_moduleを呼んだProject Scope Owner Threadだけで移動、参照、破棄する。Runtime System登録の
/// DestructorはGame Module Callbackを呼ぶ。所有値は個別に分離せず、RuntimeHostStartupへ一括移送する。
class PreparedGameModule final
{
  public:
    /// @brief 接続済みStartup所有権の複製を禁止する
    PreparedGameModule(const PreparedGameModule &) = delete;
    /// @brief 接続済みStartup所有権の複製代入を禁止する
    PreparedGameModule &operator=(const PreparedGameModule &) = delete;
    /// @brief 所有値を同一Owner Thread内で移動する
    PreparedGameModule(PreparedGameModule &&) noexcept = default;
    /// @brief 古いSystemから逆順破棄して所有値を移動代入する
    PreparedGameModule &operator=(PreparedGameModule &&a_other) noexcept;
    /// @brief System、Registry、Identity Source、Module、Code Leaseの順で破棄する
    ~PreparedGameModule() noexcept;

    /// @brief 接続時にSealしたSchema Registryを所有権を移さず参照する
    [[nodiscard]] const schema::SchemaRegistry &schema_registry() const noexcept;
    /// @brief Module Callbackを保持するSystem登録を所有権を移さず参照する
    [[nodiscard]] std::vector<runtime::RuntimeSystemRegistration> &systems() noexcept;

  private:
    friend Result<PreparedGameModule> connect_game_module(
        GameModuleQueryProvider &, std::string_view, std::unique_ptr<schema::SchemaRegistryIdentitySource>,
        const AssertContext &) noexcept;
    friend class RuntimeHostStartup;

    PreparedGameModule(std::shared_ptr<GameModuleConnection> a_connection,
                       std::unique_ptr<schema::SchemaRegistryIdentitySource> a_schemaIdentitySource,
                       std::unique_ptr<schema::SchemaRegistry> a_schemaRegistry,
                       std::vector<runtime::RuntimeSystemRegistration> a_systems) noexcept;

    std::shared_ptr<GameModuleConnection> m_connection;
    std::unique_ptr<schema::SchemaRegistryIdentitySource> m_schemaIdentitySource;
    std::unique_ptr<schema::SchemaRegistry> m_schemaRegistry;
    std::vector<runtime::RuntimeSystemRegistration> m_systems;
};

/// @brief Provider差を越えて同一ABI検証、登録順、Rollback、System Lifetimeを構築する
///
/// Provider、Project ID、AssertContextは呼出中だけ借用し、Identity Sourceの所有権を常に受け取る。成功時は
/// PreparedGameModuleがModule、Code Lease、Identity Source、Registryを所有する。失敗時は生成済みSystem Stateを
/// 逆順破棄し、Registry、Identity Source、Module、Leaseの順で解放する。呼出ThreadがProject Scope Owner Threadとなり、
/// 返却値と返却値から構築するRuntimeHostStartupは、移動、Callback呼出、最終破棄を同じThreadで行う。
[[nodiscard]] Result<PreparedGameModule> connect_game_module(
    GameModuleQueryProvider &a_provider, std::string_view a_expectedProjectId,
    std::unique_ptr<schema::SchemaRegistryIdentitySource> a_identitySource,
    const AssertContext &a_assertContext) noexcept;

/// @brief Process Image内の`cue_game_module_query`をUnloadなしで提供するStatic Providerを作る
[[nodiscard]] Result<std::unique_ptr<GameModuleQueryProvider>> create_static_game_module_query_provider(
    GameModuleQueryFunction a_query, const AssertContext &a_assertContext) noexcept;
} // namespace cue::runtime_host

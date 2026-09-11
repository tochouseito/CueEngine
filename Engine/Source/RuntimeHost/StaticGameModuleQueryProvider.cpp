#include <Cue/RuntimeHost/GameModuleQueryProvider.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Runtime/Error.h>

#include <cstdlib>
#include <memory>
#include <utility>

namespace
{
/// @brief Process Imageに組み込まれたCodeの非Unload Lifetimeを表す
class StaticGameModuleCodeLifetime final : public cue::runtime_host::GameModuleCodeLifetime
{
};

/// @brief 直接LinkされたQuery Entryを返すProvider
class StaticGameModuleQueryProvider final : public cue::runtime_host::GameModuleQueryProvider
{
  public:
    explicit StaticGameModuleQueryProvider(cue::runtime_host::GameModuleQueryFunction a_query) noexcept
        : m_query(a_query)
    {
    }

    [[nodiscard]] cue::Result<cue::runtime_host::ResolvedGameModuleQuery> resolve(
        const cue::AssertContext &a_assertContext) noexcept override
    {
        try
        {
            return cue::Result<cue::runtime_host::ResolvedGameModuleQuery>::success(
                {m_query, std::make_shared<StaticGameModuleCodeLifetime>()});
        }
        catch (...)
        {
            a_assertContext.fatal_handler().terminate("Static Game Module code lease allocation failed");
            std::abort();
        }
    }

    [[nodiscard]] cue::Error make_query_contract_error(
        const cue::AssertContext &a_assertContext, std::string_view a_summary) const noexcept override
    {
        return cue::runtime::make_runtime_error(
            a_assertContext, cue::runtime::RuntimeError::InvalidApplicationConfiguration, a_summary);
    }

  private:
    cue::runtime_host::GameModuleQueryFunction m_query;
};
} // namespace

namespace cue::runtime_host
{
Result<std::unique_ptr<GameModuleQueryProvider>> create_static_game_module_query_provider(
    GameModuleQueryFunction a_query, const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_query == nullptr)
        {
            return Result<std::unique_ptr<GameModuleQueryProvider>>::failure(runtime::make_runtime_error(
                a_assertContext, runtime::RuntimeError::InvalidApplicationConfiguration,
                "Static Game Module Query entry is null"));
        }
        std::unique_ptr<GameModuleQueryProvider> provider =
            std::make_unique<StaticGameModuleQueryProvider>(a_query);
        return Result<std::unique_ptr<GameModuleQueryProvider>>::success(std::move(provider));
    }
    catch (...)
    {
        a_assertContext.fatal_handler().terminate("Static Game Module Query Provider allocation failed");
        std::abort();
    }
}
} // namespace cue::runtime_host

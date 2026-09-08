#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/GameCore/RuntimeSystem.h>

#include <memory>

namespace cue
{
class AssertContext;
}

namespace cue::runtime
{
/// @brief Project Scopeの不変定義からSession-local Runtime Systemを一つ生成した結果
struct RuntimeSystemRegistration final
{
    game_core::RuntimeSystemDescriptor descriptor;
    std::unique_ptr<game_core::RuntimeSystem> system;
};

/// @brief PlayまたはRuntimeHostのSessionごとに独立Runtime Systemを生成するProject Scope境界
///
/// FactoryはProject Scope Ownerが所有してPlay Controllerより長く生存し、返却SystemへMutable状態を共有しない
class RuntimeSystemFactory
{
  public:
    /// @brief 派生FactoryをProject Scope Pointerから安全に破棄する
    virtual ~RuntimeSystemFactory() noexcept = default;

    /// @brief 新しいSessionだけが所有するDescriptorとSystemを生成する
    [[nodiscard]] virtual Result<RuntimeSystemRegistration> create_system(
        const AssertContext &a_assertContext) const noexcept = 0;

  protected:
    /// @brief 派生Factoryだけが基底部分を構築できるようにする
    RuntimeSystemFactory() noexcept = default;
    /// @brief Factory Identityと依存Bindingの複製を禁止する
    RuntimeSystemFactory(const RuntimeSystemFactory &) = delete;
    /// @brief Factory Identityと依存Bindingの複製代入を禁止する
    RuntimeSystemFactory &operator=(const RuntimeSystemFactory &) = delete;
    /// @brief Project Scope FactoryのAddressを固定するためMove構築を禁止する
    RuntimeSystemFactory(RuntimeSystemFactory &&) = delete;
    /// @brief Project Scope FactoryのAddressを固定するためMove代入を禁止する
    RuntimeSystemFactory &operator=(RuntimeSystemFactory &&) = delete;
};
} // namespace cue::runtime

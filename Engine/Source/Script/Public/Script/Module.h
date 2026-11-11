#pragma once

#include <concepts>
#include <cstdint>
#include <memory>
#include <typeinfo>
#include <utility>
#include <vector>

#include <Script/Behaviour.h>

namespace cue
{
/// @brief 型情報と生成関数を同じ Module で所有する。Factory の例外は ABI Adapter が捕捉する
struct ScriptRegistration final
{
    ScriptTypeDescriptor descriptor;
    std::unique_ptr<Behaviour> (*create)() = nullptr;
    const std::type_info *type = nullptr;
};

/// @brief 派生型の Factory を登録する。C++ の所有権を Module の外へ移さない
template <std::derived_from<Behaviour> T>
[[nodiscard]] ScriptRegistration make_script_registration(ScriptTypeDescriptor a_descriptor)
{
    return {std::move(a_descriptor), []() -> std::unique_ptr<Behaviour> { return std::make_unique<T>(); }, &typeid(T)};
}

/// @brief Game Module 内の登録・Instance を所有し、Version 付き C ABI で操作する
/// 全操作は Session の World Owner Thread 上で直列化する。Unload 前に全 Instance を破棄する
/// Engine 側ではこの C++ 型を受け取らず、get_api が返す Table と Token のみを使う
class ScriptModule final
{
    struct State;
    struct CreateToken final
    {
    };

  public:
    /// @brief 検証済み登録から構築する。利用側は create を使う
    ScriptModule(CreateToken, std::unique_ptr<State> a_state) noexcept;
    /// @brief まだ所有する Instance も生成した Module 内で破棄する
    ~ScriptModule();
    /// @brief 型と Property の ID・既定値を検証し、不変な登録を所有する
    [[nodiscard]] static Result<std::unique_ptr<ScriptModule>> create(std::vector<ScriptRegistration> a_registrations);
    /// @brief Table の context の住所を固定する
    ScriptModule(const ScriptModule &) = delete;
    /// @brief Table の context の住所を固定する
    ScriptModule &operator=(const ScriptModule &) = delete;
    /// @brief ABI 不一致を拒否し、借用 Table を公開する。呼出先は破棄・Unload まで有効
    [[nodiscard]] CueScriptStatus get_api(std::uint32_t a_version, CueScriptModuleApi &a_api) noexcept;

  private:
    std::unique_ptr<State> m_state;
};
} // namespace cue

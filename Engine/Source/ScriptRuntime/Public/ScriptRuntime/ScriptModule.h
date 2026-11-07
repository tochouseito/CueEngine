#pragma once

#include <memory>
#include <thread>
#include <vector>

#include <Foundation/Path.h>
#include <Script/Abi.h>
#include <ScriptData/ScriptData.h>

namespace cue
{
/// @brief Runtime が Game Module の借用 Table と複写 Metadata を所有する
/// Owner Thread 上で全操作を直列化する。生成 Instance の破棄完了まで DLL を生存させる
/// Editor プロセスは利用せず、Metadata 取得も Play / 検査プロセスで行う
class RuntimeGameModule final
{
    struct State;
    struct CreateToken final
    {
    };

  public:
    /// @brief Factory が検証済みの Module を一意所有へ移す
    RuntimeGameModule(CreateToken, std::unique_ptr<State> a_state) noexcept;
    /// @brief Table が不要になった後に DLL を解放する。Instance の停止は ScriptSystem が先に行う
    ~RuntimeGameModule();
    /// @brief 絶対 Path の Game DLL を安全な検索規則で読み、ABI / 構成 / Metadata を検証する
    [[nodiscard]] static Result<std::unique_ptr<RuntimeGameModule>> load(const Path &a_file);
    /// @brief 静的 Link した登録入口を同じ ABI 検証へ通す。関数の寿命は Module より長くする
    [[nodiscard]] static Result<std::unique_ptr<RuntimeGameModule>> from_static(CueScriptGetModuleFunction a_entry);
    RuntimeGameModule(const RuntimeGameModule &) = delete;
    RuntimeGameModule &operator=(const RuntimeGameModule &) = delete;
    /// @brief DLL から独立した所有 Metadata を次の破棄まで借用する
    [[nodiscard]] const std::vector<ScriptTypeDescriptor> &types() const noexcept;
    /// @brief ABI 操作の前に生成 Thread と同じか検証する
    [[nodiscard]] Result<void> check_owner() const;
    /// @brief ScriptSystem だけが呼ぶ Table を借用する。Unload 後の保持は禁止する
    [[nodiscard]] const CueScriptModuleApi &api() const noexcept;
    /// @brief Instance 回収失敗時に DLL の解放を避ける。失敗した Play プロセスの終了で OS が回収する
    void retain_until_process_exit() noexcept;

  private:
    std::unique_ptr<State> m_state;
};
} // namespace cue

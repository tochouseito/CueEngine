#pragma once

#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <Foundation/Result.h>
#include <Script/Abi.h>
#include <ScriptData/ScriptData.h>

namespace cue
{
/// @brief 動的な関数 Table の寿命を所有する。ロードした Platform 側が実装する
class IGameModuleLibrary
{
  public:
    /// @brief Table の借用終了後に Platform のロード参照を解放する
    virtual ~IGameModuleLibrary() = default;
    /// @brief Instance の回収に失敗したときは、ロード参照を Process 終了まで維持する
    virtual void retain_until_process_exit() noexcept = 0;
};

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
    /// @brief ロード済みの入口を ABI 検証に通し、成功時は Table とライブラリを所有する
    /// 構成未指定なら Host と同じ構成を要求する。失敗時はライブラリを呼出 Thread で解放する
    [[nodiscard]] static Result<std::unique_ptr<RuntimeGameModule>> from_dynamic(
        CueScriptGetModuleFunction a_entry, std::unique_ptr<IGameModuleLibrary> a_library,
        std::optional<ScriptConfiguration> a_configuration = std::nullopt);
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
    /// @brief C ABI の Table を期待構成で検証する。Static は native 構成、DLL は検証済み選択値を渡す
    [[nodiscard]] static Result<std::unique_ptr<RuntimeGameModule>> from_entry(CueScriptGetModuleFunction a_entry,
                                                                               ScriptConfiguration a_configuration);
    std::unique_ptr<State> m_state;
};
} // namespace cue

#include <Script/ModuleEntry.h>

namespace
{
/// @brief 空の Scene からゲーム処理を追加できる最小の登録型
class EmptyBehaviour final : public cue::Behaviour
{
  public:
    /// @brief 登録例では World を変更せず、時間入力を利用する処理は派生実装で追加する
    [[nodiscard]] cue::Result<void> update(cue::GameObject &, const cue::ScriptUpdateContext &) override
    {
        return cue::Result<void>::success();
    }
};

} // namespace

// 共通 Module 入口から呼ぶ、この Script の型登録を定義する
CUE_DEFINE_SCRIPT(EmptyBehaviour,
                  cue::make_script_registration<EmptyBehaviour>({"game.Empty", "Empty Behaviour", 1, {}}))

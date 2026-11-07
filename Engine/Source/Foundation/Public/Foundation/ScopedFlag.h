#pragma once

namespace cue
{
/// @brief Scope 内の再入を示す Flag を立て、例外時も元の値へ戻す
///
/// Owner Thread が直列に操作する Flag を借用し、Scope より長く生存させる
class ScopedFlag final
{
  public:
    /// @brief 現在値を保存し、Scope の実行中だけ Flag を立てる
    explicit ScopedFlag(bool &a_flag) noexcept : m_flag(a_flag), m_previous(a_flag)
    {
        m_flag = true;
    }

    /// @brief 元の値を復元し、借用を終了する
    ~ScopedFlag()
    {
        m_flag = m_previous;
    }

    ScopedFlag(const ScopedFlag &) = delete;
    ScopedFlag &operator=(const ScopedFlag &) = delete;

  private:
    bool &m_flag;
    bool m_previous;
};
} // namespace cue

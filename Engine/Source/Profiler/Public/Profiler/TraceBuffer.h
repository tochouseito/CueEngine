#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace cue
{
/// @brief 起動前に確保した Trace の保存領域と、欠落・上書き件数を所有する
///
/// Single Producer または外部排他で使う。resize / reset / set_circular は記録停止中だけ呼ぶ
/// 参照・Pointer は resize / 破棄で失効する。Consumer は記録停止・Worker Join 後だけ読む
/// File / ログ出力・時計取得・実行中の Allocation は行わない
template <typename T> class TraceBuffer final
{
  public:
    /// @brief 容量を初期確保する。非可動な GPU Slot の Atomic Member も保持できる
    explicit TraceBuffer(std::size_t a_capacity = 0, bool a_isCircular = false)
        : m_storage(a_capacity ? std::make_unique<T[]>(a_capacity) : nullptr), m_capacity(a_capacity),
          m_isCircular(a_isCircular)
    {
    }

    /// @brief 保存領域を複製しない
    TraceBuffer(const TraceBuffer &) = delete;
    /// @brief 保存領域を複製しない
    TraceBuffer &operator=(const TraceBuffer &) = delete;
    /// @brief 記録停止中の所有権を移す。元の参照は移動先の再確保・破棄まで有効
    TraceBuffer(TraceBuffer &&a_other) noexcept
        : m_storage(std::move(a_other.m_storage)), m_capacity(std::exchange(a_other.m_capacity, 0)),
          m_accepted(std::exchange(a_other.m_accepted, 0)), m_dropped(std::exchange(a_other.m_dropped, 0)),
          m_isCircular(a_other.m_isCircular)
    {
    }
    /// @brief 記録停止中の所有権を移す
    TraceBuffer &operator=(TraceBuffer &&a_other) noexcept
    {
        // 自己 Move 以外では配列と記録件数の所有権を移す
        if (this != &a_other)
        {
            m_storage = std::move(a_other.m_storage);
            m_capacity = std::exchange(a_other.m_capacity, 0);
            m_accepted = std::exchange(a_other.m_accepted, 0);
            m_dropped = std::exchange(a_other.m_dropped, 0);
            m_isCircular = a_other.m_isCircular;
        }
        return *this;
    }
    /// @brief 全 Consumer と Producer の停止後に領域を解放する
    ~TraceBuffer() = default;

    /// @brief 容量を変更し、記録を初期化する。Allocation 失敗時は旧領域を維持する
    void resize(std::size_t a_capacity)
    {
        // 先に新しい配列を確保し、失敗時は旧配列を残す
        auto storage = a_capacity ? std::make_unique<T[]>(a_capacity) : nullptr;
        m_storage = std::move(storage);
        m_capacity = a_capacity;
        reset();
    }

    /// @brief 容量と循環設定を維持し、件数だけを初期化する
    void reset() noexcept
    {
        // 配列を再確保せず、記録範囲と欠落件数だけを初期化する
        m_accepted = 0;
        m_dropped = 0;
    }

    /// @brief 停止中に循環設定を固定する。設定変更時は保存履歴を初期化する
    void set_circular(bool a_isCircular) noexcept
    {
        m_isCircular = a_isCircular;
        reset();
    }

    /// @brief CPU Trace を保存し、非循環満杯時は破棄数だけを増やす
    [[nodiscard]] bool append(const T &a_value) noexcept
    {
        static_assert(std::is_nothrow_copy_assignable_v<T>);
        // 通常記録の満杯と容量 0 は欠落件数へ計上する
        if (!m_capacity || (!m_isCircular && m_accepted == m_capacity))
        {
            ++m_dropped;
            return false;
        }
        // 循環記録では格納位置だけを折り返し、総採用件数は増やし続ける
        m_storage[m_accepted % m_capacity] = a_value;
        ++m_accepted;
        return true;
    }

    /// @brief 固定領域の容量を返す。保持件数は retained_count で確認する
    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_capacity;
    }
    /// @brief 記録中に変化しない容量を返す
    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return m_capacity;
    }
    /// @brief 循環上書きも含む採用件数を返す
    [[nodiscard]] std::size_t accepted_count() const noexcept
    {
        return m_accepted;
    }
    /// @brief 容量不足で採用しなかった件数を返す
    [[nodiscard]] std::size_t dropped_count() const noexcept
    {
        return m_dropped;
    }
    /// @brief 現在参照可能な保持件数を返す
    [[nodiscard]] std::size_t retained_count() const noexcept
    {
        // 採用済み総数から、今も配列内に残る件数を求める
        return (std::min)(m_accepted, m_capacity);
    }
    /// @brief 循環保存で上書きした古い記録数を返す
    [[nodiscard]] std::size_t overwritten_count() const noexcept
    {
        return m_accepted - retained_count();
    }
    /// @brief 保持している最古の記録の連番を返す
    [[nodiscard]] std::size_t first_sequence() const noexcept
    {
        return overwritten_count();
    }
    /// @brief 外部で Slot ごとの排他を行う GPU Adapter へ保存領域を借用する
    [[nodiscard]] T *data() noexcept
    {
        return m_storage.get();
    }
    /// @brief 記録停止後に固定領域を借用する
    [[nodiscard]] const T *data() const noexcept
    {
        return m_storage.get();
    }
    /// @brief 外部排他された物理 Slot を借用する。index は容量未満であること
    [[nodiscard]] T &operator[](std::size_t a_index) noexcept
    {
        return m_storage[a_index];
    }
    /// @brief 停止済みの物理 Slot を借用する。index は容量未満であること
    [[nodiscard]] const T &operator[](std::size_t a_index) const noexcept
    {
        return m_storage[a_index];
    }
    /// @brief 保持記録を古い順で返す。index は retained_count 未満であること
    [[nodiscard]] const T &at_chronological(std::size_t a_index) const noexcept
    {
        // 物理配列の折返しを隠し、最も古い標本から参照する
        return m_storage[(first_sequence() + a_index) % m_capacity];
    }

  private:
    std::unique_ptr<T[]> m_storage;
    std::size_t m_capacity = 0;
    std::size_t m_accepted = 0;
    std::size_t m_dropped = 0;
    bool m_isCircular = false;
};
} // namespace cue

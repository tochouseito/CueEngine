#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <Foundation/Result.h>
#include <Profiler/TimingSamples.h>

namespace cue
{
namespace profiler_detail
{
struct MetricState;
}

/// @brief 登録元 Profiler と独立して記録領域の寿命を共有する固定容量の計測 Handle
///
/// 全操作は Thread-safe。add / statistics / reset は動的確保・ログ・File 出力を行わない
/// コピーは同じ系列を共有する。空 Handle は記録を行わず、空の統計を返す
/// 同じ Handle Object の再代入・移動・破棄と、操作を並行させない
class ProfileMetric final
{
  public:
    /// @brief 登録前の空 Handle を作る
    ProfileMetric() noexcept = default;

    /// @brief 記録領域を共有し、別の系列を暗黙に作らない
    ProfileMetric(const ProfileMetric &) noexcept = default;
    /// @brief 記録領域の所有を移す
    ProfileMetric(ProfileMetric &&) noexcept = default;
    /// @brief 記録領域を共有する Handle へ置換する
    ProfileMetric &operator=(const ProfileMetric &) noexcept = default;
    /// @brief 記録領域の所有を移す
    ProfileMetric &operator=(ProfileMetric &&) noexcept = default;
    /// @brief 共有参照を解放する。Registry または他 Handle の所有中は記録領域を維持する
    ~ProfileMetric() = default;

    /// @brief 非負の経過時間を直近 120 標本へ追加する。系列ごとの短い排他だけを使う
    void add(std::chrono::nanoseconds a_duration) const noexcept;
    /// @brief 全有効標本の正確な平均・nearest-rank p95・最大を所有値で返す
    [[nodiscard]] TimingStatistics statistics() const noexcept;
    /// @brief 現在保持している標本数を返す
    [[nodiscard]] std::size_t sample_count() const noexcept;
    /// @brief Handle と登録 ID を維持して全標本だけを消去する
    void reset() const noexcept;
    /// @brief 登録済みの記録領域を保持するか返す
    [[nodiscard]] bool is_valid() const noexcept;

  private:
    friend class Profiler;
    /// @brief Profiler が登録した記録領域を借用者と共有する
    explicit ProfileMetric(std::shared_ptr<profiler_detail::MetricState> a_state) noexcept;
    std::shared_ptr<profiler_detail::MetricState> m_state;
};

/// @brief 表示・保存側へ渡す系列名と同時点の統計を所有する
struct ProfilerMetricSnapshot final
{
    std::uint64_t id = 0;
    std::string name;
    TimingStatistics statistics;
};

/// @brief 登録系列の最新統計を所有する。各系列間の厳密な原子的同時性は保証しない
struct ProfilerSnapshot final
{
    std::vector<ProfilerMetricSnapshot> metrics;
};

/// @brief 記録領域の登録と集計公開を所有し、外部出力を行わない Profiler
///
/// 登録・Snapshot は Thread-safe。記録は Handle の系列 Mutex だけを使い Registry を取得しない
/// Snapshot の取得と系列登録は確保を伴うため、Frame 内の記録経路では呼ばない
/// Handle は Profiler の破棄後も利用可能。Registry は Producer 破棄後の最終結果も保持する
class Profiler final
{
  public:
    /// @brief 空の Registry を作る。構築時の Allocation 失敗は呼出側の生成境界へ伝播する
    Profiler();
    /// @brief Registry だけを解放する。取得済み Handle の記録領域は最後の所有者まで残る
    ~Profiler();
    Profiler(const Profiler &) = delete;
    Profiler &operator=(const Profiler &) = delete;
    Profiler(Profiler &&) = delete;
    Profiler &operator=(Profiler &&) = delete;

    /// @brief 系列を登録する。同名の使用中系列は別 ID、未使用系列は領域を再利用して初期化する
    /// @throws std::bad_alloc 系列生成時の Allocation 失敗。Worker 開始前の生成境界で処理する
    /// @throws std::overflow_error ID の枯渇。登録失敗時は既存系列と次の ID を変更しない
    [[nodiscard]] ProfileMetric create_metric(std::string_view a_name);
    /// @brief 停止済み Producer の最終値を含めて複製する。返却後は Producer の Lock を保持しない
    [[nodiscard]] Result<ProfilerSnapshot> snapshot() const;

  private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace cue

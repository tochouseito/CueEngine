# 手動性能調査 Tool

通常の Runtime / Host は Profiler の統計と汎用 Trace だけを提供する。特定の仮説を試す条件分岐を本体へ常設せず、この Directory の Tool と調査専用 Branch で比較する。

## Build

`CUE_BUILD_PERFORMANCE_TOOLS` は既定で `OFF`。`BUILD_TESTING=ON` の構成で明示的に有効にすると手動 Tool の Target を追加する。通常の単体 Test と Profiler Test はこの Option に依存しない。

```powershell
. scripts/vcpkg_environment.ps1
$env:VCPKG_ROOT = get_vcpkg_root
cmake --preset windows-vs2026 -DCUE_BUILD_PERFORMANCE_TOOLS=ON -DCUE_ENABLE_TIMING_TRACE=ON
cmake --build --preset windows-vs2026-development --target CueSpikeTimingProbe CueSpikeEtwExport CueM09FramePacingProbe CueFrameControllerTimingProbe CueImGuiDescriptorLookupProbe
```

出力先は従来と同じ `out/build/windows-vs2026/Engine/Tests/<Configuration>/`。CMake Cache の変更だけでは過去の実行 File や配置済み Shader は削除されず、Binary も再 Build されない。残っている File の存在だけで現在の構成を判定せず、Build の Option と Source / Binary Hash を確認する。

| Target | 用途 |
|---|---|
| `CueSpikeTimingProbe` | 通常 UI / GPU / Present の詳細記録。詳細 Trace ON が必要 |
| `CueSpikeEtwExport` | 保存済み ETL の検証・抽出 |
| `CueM09FramePacingProbe` | Frame Pacing の比較 |
| `CueFrameControllerTimingProbe` | CPU FrameController の比較 |
| `CueImGuiDescriptorLookupProbe` | Descriptor 検索の単独マイクロベンチマーク |

長時間の性能比較は CTest に登録しない。必要な実機操作は実行前に確認する。`scripts/capture_spike_etw.ps1` などの既存 Script は同じ実行 File 配置を参照できる。

## 通常構成へ戻す

```powershell
cmake --preset windows-vs2026 -DCUE_BUILD_PERFORMANCE_TOOLS=OFF -DCUE_ENABLE_TIMING_TRACE=OFF
pwsh -NoProfile -File scripts/codex_build.ps1
```

基本の 120 標本統計・GPU Query・入力遅延の集計は維持する。`CUE_ENABLE_TIMING_TRACE` は詳細 Trace の記録だけを切り替える。

## 再調査の進め方

1. 現在の通常コードを基準に調査 Branch / Worktree を用意する
2. Tool から変えられる条件は Tool 側で指定する。本体内部を変える比較は調査 Branch に限定する
3. 基準 Commit、実験 Commit、Build Option、実行 File Hash、実行引数、Size / 前面状態、全標本・欠落件数を保存する
4. 同じ条件の対照試行で比較し、通常機能として採用する変更だけを統合する
5. 試行終了後も調査結果と復元元の Commit を残す

旧実験は [Legacy](Legacy/README.md)、過去の調査結果は `Docs/Research/` を参照する。

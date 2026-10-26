# 実験コードと通常の計測基盤の分離

- 日付: 2026-10-26
- Issue / Branch: #144 / `m06/issue144/gpu-waits-and-telemetry`
- 整理前 Commit: `64ae3d2bde45f2eebbe1016387300d5522165420`
- ローカル保全タグ: `research/m06-issue144-before-cleanup-2026-10-26`
- 今回 Commit / Push / Merge は行わない

## 目的

通常の Runtime / Host / Renderer へ特定の仮説用の分岐を常設せず、将来の再調査に必要な Tool・履歴・結果を維持する。直近 120 標本の全件統計、GPU Query、入力遅延、汎用詳細 Trace は恒久的な計測基盤として残す。

## 本体から外したもの

- `CUE_DESKTOP_INVESTIGATION` による ImGui 入力転送時の Context Lock 回避
- 同 Macro による Timestamp Frequency Cache と Queue の実験用 Member
- Foundation の公開 Header にあった `PerformanceStudy.h` / `DesktopInvestigation.h`
- EditorHost の Private にあった独自 ImGui Recorder / Descriptor 検索実験 Header
- 通常 Shader 配布に含まれていた `StudyImGui.hlsl`

現在の本体側の実験分岐は EditorHost / DX12 に存在していた。Runtime / WindowsHost には通常 Profiler と汎用 Trace の記録点だけを残し、今回これらの Source には変更を加えていない。

Source 内の `CUE_DESKTOP_INVESTIGATION` / `CUE_PERFORMANCE_STUDY` と実験 Header 参照は 0。汎用 `TimingTraceScope` の記録点は従来と同じ 62 箇所。Profiler と Runtime の Source は変更していない。

## 配置と保存

`Engine/Tests/Performance/Legacy/` に、旧比較 Probe・実験 Header・Shader・対応する実行 / 集計 Script を移した。当時の本体変更を前提とするため、現行 CMake の Tool Target へは登録しない。復元元・制約は [Legacy の説明](../../Engine/Tests/Performance/Legacy/README.md) を参照する。

Descriptor 検索は単独で使えるため、Header を `Engine/Tests/Performance/ImGuiDescriptorLookupStudy.h` へ移し、`CueImGuiDescriptorLookupProbe` として Build できるようにした。

移設時は既存の実験部品の内容・License・整形を保全し、旧 Probe の Include をテスト側の相対参照へ変更した。過去のコード規約差異は新たな通常コードへ持ち込まず、旧部品を一括整形して再現元との差分を増やさない。

コード内部の仮説を再検証する場合は、保存タグから一括復帰する運用にせず、現行の基準 Branch から新しい調査 Branch / Worktree を作り、必要な実験だけを移す。実験と基準の Commit / 実行 File Hash / Build Option / 実行条件を結果へ残す。

## Build の分離

新しい `CUE_BUILD_PERFORMANCE_TOOLS` は既定 `OFF`。OFF の Solution には手動 Probe 5 Target が含まれず、通常の単体 Test は維持する。`BUILD_TESTING=OFF` と Tool ON の組み合わせは Configure Error とし、黙って Tool を作らない状態を避ける。

ON の場合のみ次の Target を追加する。

- `CueSpikeTimingProbe`
- `CueSpikeEtwExport`
- `CueM09FramePacingProbe`
- `CueFrameControllerTimingProbe`
- `CueImGuiDescriptorLookupProbe`

出力先は従来の `out/build/windows-vs2026/Engine/Tests/<Configuration>/` を維持する。Shader / DXC Runtime の配置は既存 Test と共有し、同じ Directory への Copy の競合を増やさない。

Build 手順と計測の境界は [性能調査 Tool](../../Engine/Tests/Performance/README.md) にまとめた。File 出力 / ETW / 前面切替 / 枠なし表示等は手動 Probe 側に保持する。通常 Profiler 統計と `CUE_ENABLE_TIMING_TRACE` の有効化は別の設定である。

## 既存の未 Commit 変更

作業開始時の `EditorHost/Main.cpp` と `WindowsHost.cpp` のユーザーコメント追加を保全した。両 File がバックアップと Byte 単位で一致することを検査した。

保存先: `out/validation/Performance20261026/InvestigationCleanup/`。

- `BeforeUserChanges.patch`
- `EditorMain.before.cpp` / `WindowsHost.before.cpp`
- `BaseCommit.txt` / `BeforeStatus.txt`

ローカルタグは Commit 済みの実験コードだけを固定し、未 Commit コメントを含む新しい Commit は作っていない。

## 検証

- 通常構成: `pwsh -NoProfile -File scripts/codex_build.ps1` 成功
- 通常 Debug CTest: **56 / 56 成功**（46.51 秒）
- OFF の Solution に手動 Tool 5 Target が存在しないことを確認
- ON の Solution に手動 Tool 5 Target が存在することを確認
- ON / Trace ON の Development で手動 Tool 5 Target の Build 成功。Probe の Usage / Trace 有効化も確認
- 元の実験 Header / Shader 5 File と移設後の内容が一致することを確認
- ユーザーコメント、Runtime / Profiler、62 の汎用 Trace 記録点を保全
- 検証後は手動 Tool / 詳細 Trace の Cache を OFF へ戻す

Build / Test Log は上記保存先に保持する。性能比較・ETW 採取は今回実行していない。配置や Compile Option を変えた手動 Tool で以前の性能値がそのまま再現するという主張はしない。

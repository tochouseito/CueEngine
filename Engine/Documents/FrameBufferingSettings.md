# Frame バッファリング設定

Issue: [#142](https://github.com/tochouseito/CueEngine/issues/142)

## 共通の枠数

性能設定 UI の `Buffering (frames)` で CPU / GPU の保持枠数を一括設定する。従来の `CPU snapshots`、`Update lead frames`、`Worker threads` と `GPU resource frames` の独立した編集 Control を置き換える

| Buffering | CPU Snapshot | GPU 資源枠 / ImGui Upload Ring | Update 先行数 | Worker |
| --- | --- | --- | --- | --- |
| Single: 1 | 1 | 1 | 0 | OFF |
| Double: 2 | 2 | 2 | 1 | ON |
| Triple: 3 | 3 | 3 | 2 | ON |
| N: 4〜8 | N | N | N − 1 | ON |

UI の上限は開発者 Policy の CPU / GPU 容量と先行数制限にも従う。現在の導出結果と実効値は読み取り専用で表示する。`Unfinished GPU frames` は 1〜N の独立した上限で、N を縮小した場合だけ必要に応じて補正する。`SwapChain buffers` と `Queued presents` は独立した表示設定

`gpu.resourceFrameCount` をバッファリング数の正本とする。性能設定要求、保存、Editor 起動時に `frame_config_for_buffering` で CPU 容量、Worker と先行数を導出する。`set_frame_buffering` は UI 編集時の一括変更と GPU 未完了上限の補正を担う。Runtime の `FrameControllerDesc` と下位 Host の独立した実行構成は、低水準の実行・検証契約として維持する

GPU 使用中の領域は Fence で再利用を制限する。CPU Snapshot から ImGui Upload への転送方式は継続し、任意に生成した Buffer は所有側の契約で管理する

## Preset

| Preset | Buffering | GPU 未完了上限 | SwapChain Buffer | 表示 Queue 上限 |
| --- | --- | --- | --- | --- |
| Low latency | 1 | 1 | 2 | 1 |
| Balanced | 2 | 2 | 2 | 1 |
| Throughput | 3 | 3 | 3 | 2 |

## 保存形式

新規保存は `CuePerformance 2`。枠数を一つだけ保存し、CPU Snapshot 数、Worker、Update 先行数を個別保存しない。FPS / 待機 / 同期、GPU 未完了上限、表示設定、Clear Color は引き続き保存する

`CuePerformance 1` は読込可能。旧入力の妥当性を確認した後、旧 `GPU resource frames` を新しい共通枠数として採用する。旧 Worker OFF や独立した先行数は新しい自動規則で置き換える。このため旧 CPU 数と GPU 数が異なる保存設定では、CPU 数と Worker の実効構成が変わる場合がある。読込だけでは File を書き換えず、次の保存で新形式へ更新する

不正な枠数、容量を超える GPU 未完了上限、未知の Schema や余分な入力は拒否する。保存失敗時は従来どおり旧 File を保持する

## 検証

編集 Model で 1 / 2 / 3 / 8 枠の往復、Worker と先行数の導出、GPU 上限の補正、他の設定の保持を確認する。保存形式の往復、旧形式移行、不正入力と保存失敗も確認する。実 Editor では起動時の導出、Preset 切替、Waitable ON/OFF、保存からの再起動、Resize / 最小化 / Close を確認する

2026-10-16、Windows / x64。`pwsh -NoProfile -File scripts/codex_build.ps1` の Debug Build と `ctest --preset windows-vs2026-debug --output-on-failure` の全 51 件が成功した。CTest は 41.74 秒。ログは `out/validation/M10/142-Debug-build.log` と `142-Debug-tests.log`。手動のマウス操作と Development / Release 構成は今回未確認

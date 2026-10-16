# M10 Performance Settings Corrections

Milestone: [M10](https://github.com/tochouseito/CueEngine/milestone/11)

| Issue | 修正対象 |
| --- | --- |
| [#132](https://github.com/tochouseito/CueEngine/issues/132) | Worker OFF の FPS 待機方式と Spin |
| [#133](https://github.com/tochouseito/CueEngine/issues/133) | 単一 Thread の GPU 先行上限と物理枠再利用 |
| [#134](https://github.com/tochouseito/CueEngine/issues/134) | Waitable OFF の表示 Queue 未適用 |
| [#135](https://github.com/tochouseito/CueEngine/issues/135) | Worker 切替での Update 先行数保持 |
| [#136](https://github.com/tochouseito/CueEngine/issues/136) | 条件付き設定と要求 / 実効値の UI |

## #132 FPS 待機

周期の期限と CPU 完了 / 周期完了の分離は M09 を維持する。Sleep / Hybrid / Spin の実効計算を共通化し、単一 Thread でも Waiter の高精度 Timer を使う。Main は一回最大 1 ms の Sleep または Spin 区間で Message Pump へ戻る。表示準備の再確認にも粗い Condition Variable の Timeout を使わない。Worker は同じ方針で期限まで停止可能に待つ

実効 Spin は FPS 上限 0 または Sleep の場合は 0、それ以外は要求値と周期の 1/8 の小さい方。実待機時間は OS Scheduler と Timer の対応状況に依存する

RecordingWaiter による決定的な Test で、単一 Thread の Sleep / Hybrid 選択、期限前の投入拒否、1 ms 以下の待機要求、Spin 上限を検証する。FrameControllerTimingProbe の `--fps-only` は GPU / Window を含めず Worker ON/OFF を同じ 60 FPS 設定で比較する。実入力から画面までの遅延や全環境の 60 FPS 達成を保証する測定ではない

2026-10-16、Debug / x64、空 Callback / 60 FPS / 120 Frame を 2 回測定した。単一 Thread は変更前 32.54〜32.55 FPS、変更後 60.44〜60.46 FPS。変更後の Worker は 60.40〜60.48 FPS。最初の Frame が即時なので壁時計平均は 60 を少し超える。単一 Thread の直近間隔は 16.669 / 16.699 ms。Debug Build と Runtime / WindowsHost / EditorHost の関連 CTest 12 件が成功した

## #133 GPU の先行と枠再利用

単一 Thread の投入前確認から全 GPU 完了待機を取り除く。GPU 未完了上限に空きがあり、次に使う FrameGraph の物理枠、共有 Pool 資源、ImGui の GPU Ring が安全な場合に次の Frame を作る。未準備なら UI / Frame ID を進めず、短い高精度待機後に Message Pump へ戻る。CPU Snapshot 数、GPU 物理枠数、GPU 未完了上限、SwapChain Buffer 数は独立して扱う

Pool の非待機照会は Lease の予約ではない。確認から取得まで所有側で直列化する。独自 Pool は `can_acquire` を実装する。未対応なら明示的な失敗を返し、Main に隠れた GPU 待機を持ち込まない。Worker 経路の安全な完了待機は維持する

制御可能な Completion で物理枠別の照会、エラー伝播、照会時の待機ゼロを検証する。Pool の Read / Write 競合と待機ゼロを検証する。実 Host Test では Queue を Fence で止め、単一 Thread で二 Frame を未完了のまま提出できること、三 Frame 目が UI を作らず速やかに戻ること、解放後に再開することを確認する

Debug Build と DX12 / WindowsHost / Editor の関連 CTest 28 件が成功した

## #134 表示 Queue の実効値

保存用の `PerformanceSettings` と観測用の `EffectivePerformanceSettings` を分ける。`effective.presentation.maxQueuedFrames` は optional とし、Waitable OFF 時は未適用の `nullopt` を返す。要求値の保存形式と数値は保持する。SwapChain は `SetMaximumFrameLatency` の成功後、Waitable Handle を所有する間だけ適用済み上限を公開する

UI は Queued presents を Waitable OFF 中に編集不可とし、要求値と未適用の理由を表示する。これは OS / DXGI 自身の Queue 既定値がゼロになる意味ではない。設定保存の往復、状態解決、Native SwapChain の ON/OFF、実 Editor の再設定後の状態を Test する

Debug Build と関連 CTest 4 件が成功した

## #135 Update 先行数の編集

UI は `requested_update_lead_frames` で Worker 設定と独立した要求値を表示する。OFF 中は先行数 Control を無効化し、実効値が 0 で要求は保持されることを表示する。Widget が編集された場合だけ値を書き戻し、未指定の自動値を UI の表示だけで明示値へ変換しない

CPU 容量の縮小時は明示した先行数を容量 - 1 へ収める。自動指定の `nullopt` は維持する。純粋な編集 Model の Test で ON → OFF → ON、OFF 中の保存・読込、明示 / 自動指定、容量の縮小・拡張、不正容量の拒否を確認する

Debug Build と関連 CTest 4 件が成功した

## #136 条件付き Control と実効表示

UI は編集 Model の判定で次の Control を無効化し、理由を表示する。無効化だけでは保存要求を書き換えない

| Control | 編集条件 |
| --- | --- |
| CPU snapshots / Update lead | Worker ON |
| Wait method | FPS 上限あり |
| Spin | FPS 上限あり、Hybrid |
| Queued presents | Waitable ON |
| Prediction slack | Display prediction、Waitable、VSync |

CPU snapshots は OFF 中も容量自体は確保されるが、CPU 処理は直列で Update 先行は 0。GPU の物理枠・未完了上限は Worker OFF でも独立して働く。GPU 容量縮小時は未完了上限も収める。容量を再拡張しても明示上限を自動増加しない

現在の要求と実効 Update 先行数、Spin、Prediction slack、表示 Queue 上限を別々に表示する。実効 Spin は周期の 1/8 まで。予測が Fallback している間の実効 slack は 0。編集中の予測設定に必要な機能が揃っていれば slack を編集できるが、実行時の統計が古ければ Fallback 理由を表示する

編集 Model Test は Worker ON/OFF、FPS 無制限、Sleep/Hybrid、各同期方式、VSync / Waitable の欠落、高 FPS の Spin 制限を代表例で検証する。保存往復、Native API、実 Host の枠切替・Resize・最小化・Close と合わせて確認する。手動のマウス操作や入力から表示までの遅延の実測は今回の自動検証に含めない

## 統合時の検証

2026-10-16、Windows / x64。各構成は `scripts/codex_build.ps1` と対応する CTest Preset で確認した

| 構成 | Build | 全 CTest |
| --- | --- | --- |
| Debug | 成功 | 51 / 51 成功 |
| Development | 成功 | 51 / 51 成功 |
| Release | 成功 | 51 / 51 成功 |

実行ログはローカルの `out/validation/M10/` に保存する。Issue ごとに Branch / PR を分け、統合先は `develop`

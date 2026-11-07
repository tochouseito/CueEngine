# GPU 表示資源の Resize（M04-06）

Issue: [#59](https://github.com/tochouseito/CueEngine/issues/59)

## 所有と同期

WindowsHost は Main Thread で Window の最新 ClientSize を取得する。サイズ変更と最小化では新規 CPU Frame の投入を停止する。既に投入した Frame は Render の記録 Scope を通して ImGui Snapshot を回収し、必要な Present を終える。待機中も `step()` は Window Message を処理する

`Runtime::is_idle()` は投入済み Update / Render Callback の完了を非 blocking で確認し、Worker の最初の失敗を返す。完了数が揃ってから Main Thread で `DX12MainFrameGraph::resize()` を呼ぶ。GPU 完了は CPU の静止とは別に Graph と Queue の Fence で待つ

Resize の順序は次のとおり

1. 全 CPU Callback と Snapshot 借用を終える
2. Graph の全枠で GPU Completion を待つ
3. 旧 FinalColor、Placed Resource、RTV / SRV、外部 BackBuffer Binding と Pass を解放する
4. SwapChain の Graphics Queue を idle にし、RTV Slot と BackBuffer の直接参照を解除する
5. `ResizeBuffers` で非零の最新寸法へ変更し、名前付き BackBuffer と RTV を再取得する
6. 新しい Graph ID、FinalColor と枠別 View、追加 Pass、表示 Pass を生成し、同じ Runtime の Frame 投入を再開する

Window、Runtime Worker、Frame 番号、Graphics Queue Lease、ImGuiManager と UI 専用 Descriptor Heap は維持する。零寸法の間は Resource を作らず Present も行わない。最小化中は新しい UI / Update Frame の投入も止め、Message 配送と Close は処理する。複数の Resize 通知は最新寸法へまとめ、同じ正常寸法は再生成しない

DXGI 呼出を Render の ImGui Context 排他中に行わない。Window 所有 Thread が Snapshot Texture の Pin 待機中に Render の ResizeBuffers と循環待ちを作らないよう、CPU Frame を排出してから Owner Thread で再生成する

## Pass の再生成

`MainFrameGraphConfig::configure` は Graph ごとに新しい FinalColor Handle を受け取り、追加 Pass を再生成する。表示 Pass は `displayPassFactory` が新しい一意所有 Instance を返す。EditorHost の既定 ImGuiPass もこの Factory から作り、同じ ImGuiManager を借用する

互換用の `displayPass` は初回だけ所有権を移す。一回限りの独自 Pass に Factory がない場合は、サイズ変更を旧 Graph の破棄前に Error とする。標準表示 Pass は Factory の指定なしで再生成できる。Factory / configure / setup / 旧 Pass の破棄は Resize の呼出 Thread 上で直列に行う。旧 Graph の Resource Handle は次の Graph へ持ち越さない

## 失敗時

GPU 待機が失敗したら利用中の資源を解放せず、主原因を Result で返す。SwapChain の再取得が途中で失敗した場合は部分資源を Owner に残し、Present を拒否する。再試行または shutdown で回収する。Graph の再生成失敗後も記録を再開せず、停止状態から再試行できる。Host は失敗を呼出元へ返し、既存の異常終了処理で shutdown する

## 検証

2026-10-07、Windows x64 / Visual Studio 2026 / MSBuild / Debug で確認した

| 項目 | 結果 |
| --- | --- |
| 規定の Debug Build | 成功 |
| 全 CTest | 38 / 38 成功 |
| Host のタイミング再検証 | Lifecycle / Resize 統合 Test を追加で 3 回連続実行して成功 |
| WARP Resize 画素 | 7 回の連続寸法変更と標準 Pass の Resize、Factory 失敗後の再試行で四隅 RGBA = 51, 102, 153, 255 |
| CPU / GPU 寿命 | CPU 待機前に提出した Frame を Resize が回収、Descriptor 全 Slot の返却、Error / Corruption と Device 自身以外の Live GPU Object がないことを確認 |
| Host / Editor 統合 | 実 Window の状態変更、新寸法での Pass 記録、復帰後の ImGui GPU 記録、同じ Worker Thread の継続と終了を確認 |
| 失敗経路 | 再構築 Factory の null / 例外、停止状態での記録拒否と同寸法再試行、CPU Worker 失敗の静止判定への伝播 |
| Hardware 実画面 | CueWindowsHost のクリアカラー、最大化、最小化、復帰後の描画と Close を Computer Use で確認 |
| Editor 実画面 | Test / TEST の表示を確認。操作前にツール経由の対象 Window が消え、手動 Resize は未完了 |

初回テストの2件の失敗を調査し修正した。Leak Probe が保持する正常な Live Device Warning で Debug Layer が停止していたため、Test の最終診断時だけ Warning の Break を解除し、重大診断と実 Object の残存検査は維持した。Host Test は記録回数だけで先に進まず、Window 実寸法と Cache、Graph 寸法、CPU Callback 完了が揃うまで待つようにした。最終全テストは成功

ログは `out/validation/Issue59/debug-build.log` / `debug-tests.log` / `host-repeat-tests.log` に保存する（Git 管理対象外）。Development / Release と任意の DeviceLost 注入は未実施。Editor のツール経由起動の終了原因は未確定で、実 Process の自動 Resize / 復帰 / 6 秒継続テストでは再現しない。Resize 中の GPU 待機や再コンパイルの時間を測定していないため、性能改善は主張しない

- DX12MainFrameGraph Test: WARP で複数寸法へ連続 Resize し、FinalColor Clear → 巨大三角形表示後の BackBuffer の四隅を Readback して RGBA を検証する
- SwapChain Test: VSync の有無、Tearing 許可、Graphics Queue の継続、RTV 再取得、同サイズと零寸法、停止後の操作拒否
- WindowsHost Test: 実 Window の最大化、最小化、復帰、連続 Resize と Pass の新寸法、Worker Thread 継続
- EditorHost Test: 既定 Test / TEST UI、Main の UI 構築と Worker Snapshot 転送が復帰後も進むこと
- Runtime Test: 未完了と完了の静止判定、投入停止中の Worker 失敗伝播

Native Command List の Resource 操作は COM 参照を保持しないため、GPU 完了後の Resize に CommandPool の停止は不要。Pool を継続使用し、再貸出前の Reset で旧記録を消す。[Microsoft の Command List 寿命の説明](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles)

BackBuffer の直接参照と間接参照を解除してから ResizeBuffers を呼ぶ。[Microsoft の ResizeBuffers 契約](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers)

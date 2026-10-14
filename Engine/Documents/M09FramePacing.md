# M09 Frame Pacing and Latency Control

Milestone: https://github.com/tochouseito/CueEngine/milestone/10

## 設定契約

- CPU の `maxFramesInFlight` は CPU Snapshot の容量。Update 先行数は active Render より先に準備できる件数で、容量より小さくする
- 未指定の Update 先行数は既存の容量から導出する。単一 Thread は常に先行 0
- GPU の `resourceFrameCount` と `maxFramesInFlight`、SwapChain の `bufferCount` と `maxQueuedFrames` は独立した値
- 先行数は上限であり、指定数だけ入力を遅延させる処理ではない。入力から物理表示までの遅延を保証しない
- 既定は CPU 2 枠、Update 1F 先行、GPU 2 枠、表示 Queue 上限 1、60 FPS、VSync / Tearing 無効
- Native の対応機能は Host が確認し、要求値と適用値を分離する。設定値の型と範囲は変更前に Result で検証する

## 実装順

#115 設定、#116 Present 後待機、#117 枠分離、#118 GPU 上限、#119 Waitable SwapChain、#120 設定変更と保存、#121 計測と表示同期、#122 固定更新と補間

FPS 時間待機は Render Callback / Present 後に集約し、Main は Message Pump を継続する。Update は次周期通知で休む。単一 Thread は Present 後に期限を登録して戻り、Main の Message Pump を長い Sleep で停止させない

この Milestone は完了 Issue を閉じても open のまま維持する

## #116 Present 後待機

Worker は CPU Render 完了を即時公開し、周期待機が終わってから次の投入許可を増やす。起動時だけ先行数 + 1 件を許可し、Render F0 と Update F1 の重なりを作る。未来の Update 完了を Render の必須条件にしないため、Resize の排出でデッドロックしない

FPS 待機は共通の停止 Token で解除する。Main は最大 1 ms の通知待ちで Pump へ戻り、FPS Timer / Spin を行わない。単一 Thread は Present 後に期限だけ保持し、次の step で判定する。CPU 完了間隔と周期完了間隔を別統計にする

## #117 CPU / GPU 枠

Host は CPU 容量を ImGui Snapshot に、GPU 容量を Graph と公式 ImGui Backend の Ring に渡す。論理 Frame ID と GPU Context の物理枠番号は同一と仮定しない。CPU Snapshot の Pin は記録後に回収し、GPU VB / IB Ring と Texture Descriptor はそれぞれの最終利用 Fence で保護する

容量は 1〜8 枠を検証し、GPU 未完了上限は GPU 枠容量以下にする。SwapChain は別に 2〜16 枚を検証する

## #118 GPU 未完了上限

Graph はすべての Queue を合流した正常提出 Completion を返す。Host は Present の有無に関係なく固定容量 Queue へ登録する。上限では停止可能な短い通知待機を挟み、非待機 status で Device Lost を確認する。CPU / GPU 枠の再利用待機は引き続き各 Owner が行う

## #119 表示 Queue

Waitable SwapChain は最初の Frame を含め記録前に表示枠を取得する。最大表示 Queue 数を DXGI に設定し、GPU 未完了上限や FPS 上限とは分離する。取得した通知は Present まで保持し、最小化などの記録取消で失わない。Resize は生成時の Flags を維持し、停止は Worker join 後に待機 Handle を閉じる

単一 Thread は UI 構築前に表示 / GPU 枠を非待機で確認する。未準備なら Frame ID を増やさず Pump に戻る。Worker 構成では 1 ms ごとに停止 / Resize 要求を確認する

## #120 設定変更

要求は Main / UI から値で受け取り、開発者の公開範囲と再生成 Factory を変更前に検証する。次の Main 境界で新規投入を止め、周期待機だけ取消して CPU Snapshot を排出する。Worker join、GPU 完了、Graph / SwapChain / ImGui 枠変更の順に進み、同じ論理 Frame ID から再開する

ImGui Context と入力 Handler は維持する。GPU Backend の再生成は MainViewport の Platform データも破棄するため、Win32 Backend も再接続する。SwapChain の Graphics Queue は Backend の QueuePool が継続所有する

不正な要求と読込失敗は既存設定を維持する。Native 再構築の途中失敗 / 例外は Result と停止状態へ確定し、shutdown のみ許可する。Factory のない一回限りの表示 Pass は再構築が必要な変更を事前に拒否する

Editor の Test 内「Performance settings」で編集 / Preset / Apply / Apply and save / Load saved を提供する。保存は Schema 付き File を FileSystem.replace_file で置換し、次回起動時にもゲームの Policy を検証する。既定保存先は Product dataRoot/editor/Performance.cfg。Policy 自体は保存値から変更しない

## #121 同期と計測

- FixedPeriod: Render / Present 後に Controller が FPS 上限の残り時間だけ待つ
- PresentationReady: 同じ FPS 上限に加え、次周期の通知前に DXGI の表示準備を待つ。次の記録では取得済み Permit を再利用する
- DisplayPrediction: VSync と有効な DXGI Refresh 統計がある場合、次 Refresh 位相 - 指定 Slack を次の開始候補にする。FPS 上限の期限より早くは開始しない。表示に間に合う GPU 処理時間の予測器ではなく、Slack は必要な処理余裕を開発者が設定する
- 表示待機未対応、統計未対応 / 250 ms より古い / 不連続 / Monitor 変更は FixedPeriod に戻す。要求 / 実効 Mode と理由は UI / API で取得する

Main が Frame を採用した時刻を入力採取の代理時刻とし、Render 開始 / CPU 完了までの経過時間を保持する。物理入力イベントの発生からの遅延ではない。CPU Render 完了、周期完了、FPS 時間待機、表示準備待機、GPU 上限待機、Graph 記録、Present API 戻り間隔、完了 GPU Pass 時間を別に表示する。GPU 未完了数は Render の提出時点の Snapshot

DXGI SyncQPCTime は SyncRefreshCount の Refresh 標本であり、Present Frame の直接の表示時刻と同一視しない。PresentRefreshCount との差を Refresh 周期で補正した場合は推定値と公開する。統計が取れない場合は未計測と表示し、Present の時刻から表示時刻を捏造しない。取得仕様は [DXGI_FRAME_STATISTICS](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics) に従う

CueM09FramePacingProbe は同じ容量 / Test UI / Clear の条件で、VSync ごとに三つの Mode を比較する。180 Frame のうち先頭 60 を Warmup とし、最新 120 標本を CSV へ出す。物理的な入力遅延、VRR / 複数 Monitor、表示の見た目はこの Probe だけでは検証できない

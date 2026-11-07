# CPU 時計の取得と所有

経過時間の取得には `Platform/Clock.h` の `cue::Clock` を使う。時刻と期間の表現には引き続き `std::chrono::steady_clock::time_point`、`std::chrono::nanoseconds` 等を使う

## Platform 実装

- `create_clock()` は単調時計の一意所有権を返す
- `get_default_clock()` は既定時計を借用する。Process 内の差し替え登録は提供しない
- 両者の標準実装は同じ時間軸を使う。`steady_clock::now()` の直接呼出しは `Platform/Private/Clock.cpp` に閉じる
- `Clock::now()` は全 Thread から呼出可能で、再入可能、後戻りせず、例外を送出しない

## Host と利用側

WindowsHost は Window の生成前に時計を確定する。未指定なら ThreadServices 内の時計を所有し、`WindowsHostConfig::clock` 指定時は外部時計を借用する。EditorHost は `EditorHostConfig::clock` を下位 Host へ渡す

同じ時計を次の利用先へ接続する

1. Window 入力イベントの到達時刻
2. Runtime / FrameController の周期制御と CPU 計測
3. Host の Frame 採取・入力遅延・GPU 待機計測
4. ImGui の UI 構築・Snapshot Copy・Context Lock・GPU 待機計測
5. DX12 FrameGraph の記録・Fence 待機計測と SwapChain の Present 時刻

Host の時計は Window、UI、Backend、Runtime より長く生存させる。外部時計の Owner は Host の停止完了まで生存させる。単体利用する ImGuiManager は Manager の破棄まで時計を借用する

Backend Factory Callback は `IFileSystem *` と `Clock &` を受け取り、生成先 Backend に同じ時計を接続する。DX12Backend は `DX12ResourceContext` で時計を貸し、SwapChain と FrameGraph が借用する。Resize 後も同じ時計を維持する

単体利用では WindowSystem、DX12Backend、ImGuiManager の生成時に時計を指定できる。未指定なら Platform の既定時計を使う。入力から表示までの経過時間を測る利用者は、全区間で同じ時計を渡す

## 用途の異なる時刻

ログの日時とログ File 名は暦時刻であり、`system_clock` を使う。単調時計の経過時間と相互に減算しない

DXGI 表示統計、GPU Clock Calibration、ETW Trace の Native Tick は外部 API の時間軸を持つため、その取得 API を維持する。DXGI の時刻は QPC との差分を求め、Host と共通の Clock の現在値に対応付ける

## 検証

InputLatencyTests では実時計から原点を一日ずらした Clock を EditorHost へ注入する。Single / Double / Triple buffering と Waitable の各経路で、入力到達から Present / CPU 完了までの標本が有効で、時刻順序が保たれることを確認する

# ADR-0005: 最小 Renderer と Windows Presentation の境界

- Status: Accepted
- Date: 2026-09-25
- Issue: [M04-01](https://github.com/tochouseito/CueEngine/issues/19)
- 関連: [ADR-0001](0001-architecture-boundaries.md)、[ADR-0003](0003-frame-thread-contract.md)、[ADR-0004](0004-runtime-host-lifecycle.md)

## 背景

M03 の `WindowsHost` は Window と共通 `Runtime` を起動するが、Render Callback は空処理である。M04 は単色 Clear と Present を実 Window へ接続する。D3D12 の所有権と GPU 完了条件を `FrameController` の CPU 側の進行数だけで代用できない。

## 決定

Windows 固有の `D3D12Renderer` が Device、Direct Queue、Swap Chain、Back Buffer、RTV、Command Allocator/List、Fence を一意所有する。`WindowsHost` は Renderer を一意所有し、共通 `Runtime` の Render Callback に Frame 番号を渡す。共通 `Runtime`、`Window`、`FrameController` の公開契約は D3D12 型を知らない。Scene、Material、Texture、ImGui 用の抽象 API は M04 に含めない。

Windows 固有の Window API は `Window&` から有効期間が Window より短い Native Handle を借用する。Handle は Renderer の初期化時に Swap Chain 作成へ渡す。Win32 型は Windows 実装の `.cpp` 内に閉じ、共通 `Platform` と Renderer の公開 Header は Windows SDK を含まない。Handle は Window の破棄後に使用しない。

Renderer の Frame 入力は Frame 番号と固定の Clear Color とする。`render_frame` は直列化された Render Callback 上で、現在の Surface 状態を適用し、Back Buffer を `PRESENT` から `RENDER_TARGET` へ遷移させて Clear し、`PRESENT` へ戻して Submit／Present する。`FrameController` が報告する Render 完了は CPU Callback の完了であり、GPU 完了ではない。Allocator と Back Buffer は対応する Fence 値の完了後だけ再利用する。

Window Message は MainThread で処理する。Resize／Minimize／Restore の最新要求を Renderer に所有値で渡し、Swap Chain 操作は Render Callback 側で直列に実行する。最小化中または Client Size が 0 の間は描画と `ResizeBuffers` を保留する。Size が変わる場合は GPU 完了を待ち、旧 Back Buffer 参照を解放してから `ResizeBuffers` と RTV 再生成を行う。初期 Size と同じ Event は再生成しない。

Close と失敗時は新規 Frame 投入を止める。`WindowsHost::shutdown()` は `Runtime` の Worker を停止・join してから Renderer の GPU 完了を待ち、Renderer を解放した後に Window を破棄する。途中失敗でも残る資源を回収し、最初の `Error` を主原因として返す。Device Lost と GPU 待機失敗は操作名と HRESULT を `Result` に保持する。

M04 の製品用 Host では `FrameController::maxFps` を Frame 間隔の上限とし、`Present(0, 0)` を用いる。VSync を選ぶ別の構成では `maxFps` を 0 にし、二重の待機を避ける。Buffer 数は FrameController の先行数と独立し、GPU Fence で再利用を制御する。

## 検証

公開 Header を Windows SDK なしで Compile し、依存方向を確認する。Hardware Adapter を優先し WARP 経路を用意する。Debug／Development／Release の Build と CTest、実 Window の連続 Frame、Resize、最小化／復帰、Close を確認する。D3D12 Debug Layer が利用可能な環境では Resource Barrier、RTV、Allocator 再利用、Fence の警告を確認する。

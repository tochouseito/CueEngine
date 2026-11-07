# Legacy `develop` RHI / DX12 移植対応表

対象: `CueEngineLegacy/Engine/Source/Runtime/RHI` の `develop`。2026-09-30 時点の現行 CueEngine と照合。描画システム、Assets、Debug 表示機能は対象外。現行の `Result<T>`、`static create`、世代付き Handle と `cue` 名前空間を優先したため、API の署名は Legacy と同一ではない。

| Legacy のクラス／機能 | 現行 CueEngine | 状態 |
| --- | --- | --- |
| `IBackend`、`BackendFactory`、`D3D12Backend` | `IBackend`、`BackendFactory`、`DX12Backend` | 固定 Graph と外部 Graph の実行、Present、Manager／Pool 借用、停止、Factory を実装。外部 Graph の Render／Present 分離と Backend の Graph Factory は未実装 |
| `IRenderDevice`、`DX12RenderDevice` | `IRenderDevice`、`DX12RenderDevice` | Device 境界を実装。Device 生成経路は現行実装を維持 |
| `IQueueContext`、`IQueuePool`、`DX12GpuCommandQueue`、`DX12QueuePool` | 同名クラス | Graphics 2、Compute 4、Copy 4 の Queue Pool 貸出、Fence、GPU 間待機、timestamp frequency を実装。Present は現在の描画と同じ主 Graphics Queue。独立 Present Queue は未採用 |
| `ICommandContext`、`ICommandPool`、`DX12GpuCommandContext`、`DX12CommandPool` | 同名クラス、`IGpuCommandRecorder` | 複数 Context 貸出、Submit／Fence 再利用、timestamp query、event marker、Barrier／Copy／Root Binding／UAV Clear／間接描画を実装。描画命令は `IGpuCommandRecorder` に分離し、Legacy の `ICommandContext` とは API 配置が異なる |
| `IBufferManager`、`DX12BufferManager` | 同名クラス | Default／Upload／Readback の複数 Heap Slice、永続 Map、`SlotUploader`、名前検索を実装 |
| `ITextureManager`、`DX12TextureManager` | 同名クラス | 2D／3D／Cube／Mip／Array／MSAA、初期 Subresource 転送、Descriptor Index、名前検索を実装。DDS File 読込は未実装 |
| `IViewManager`、`DX12ViewManager`、`DescriptorAllocator` | 同名クラス | RTV／DSV／SRV／CBV／UAV、Buffer 要素範囲、Texture Mip／Slice、CPU Shadow と Shader-visible Heap、Texture Table、独立 ImGui Heap、名前検索を実装。ImGui の Font Descriptor 登録は Editor 実装時に接続する |
| `IPipelineManager`、`DX12PipelineManager`、`HLSLCompiler` | 同名クラス | Source／File の DXC Compile、Root Descriptor／Table／定数、Graphics／Compute PSO、InputLayout／Blend／Rasterizer／Depth／RTV／DSV Format、名前検索を実装 |
| `FrameGraphBuilder`、`FrameGraphContext`、`FrameGraphPass`、`FrameGraph` | 同名クラス、`DX12GraphExecutor`、`DX12GraphResourceBindings` | 依存・Barrier・Pass 所有、複数 Queue 実行、外部 Graph Binding、Resize 後の Surface 再解決、CPU 実行統計を実装。Legacy の Manager 経由 Resource 生成／検索・再構築・GPU 実行統計は未実装 |
| `SwapChain`、`ResourceLeakChecker` | `DX12SwapChain`、`ResourceLeakChecker` | 現行の表示・Resize と Debug 生存 Object 診断を維持。Monitor Refresh Rate 取得は移植対象から除外 |
| `DX12GpuResource`、`SlotUploader`、`PresentToSwapChainPass` | `DX12ResourcePool`、`SlotUploader`、`DX12CopyToBackBufferPass` | Resource／View の世代付き Pool、`SlotUploader`、Back Buffer Copy を現行方式で維持。GPU 使用中の破棄は Manager が Queue Idle を確認する。Legacy の Resource 個別 Fence は未採用 |

## 完了条件

- [x] Legacy の主要 RHI クラス責務を現行 `Result<T>` と Handle 規則へ対応付ける。
- [x] Queue／Command の複数貸出、Fence 返却、Renderer 基盤で使う命令を移植する。
- [x] Buffer／Texture／View／Pipeline の主要 Descriptor と生成機能を移植する。
- [x] 外部 FrameGraph の実行、Resize 後の再利用、CPU 実行統計を実装する。
- [x] Debug|x64 のビルド、CTest、実 Window、Readback Pixel、DX12 Debug Layer を確認する。
- [ ] 未採用の Legacy API と GPU 実行の細部を Review して確定する。

## 現時点の検証

- Debug|x64 の MSBuild は成功し、CTest は 20 件すべて成功
- 実 Window の起動・Resize・終了と固定 Mesh の Readback Pixel を CTest で確認
- 外部 FrameGraph を実 Window で実行して Resize 後も再利用し、Pass の CPU 統計を確認
- 外部 Graph の Surface 初期・最終状態、物理 Resource の重複 Binding を GPU Submit 前に検証し、固定 Graph への復帰を確認
- UAV Clear を CPU Shadow Descriptor と GPU-visible Descriptor から行い、Readback で値を確認
- 非有界 Descriptor Table を拒否し、Texture Table が専用領域を超えないことを確認
- 間接描画 Signature は実 Device で生成確認。間接描画の Pixel 結果は未検証
- Graphics から COPY Queue へ渡す Texture は Graphics 側で `COMMON` に戻し、COPY Queue では暗黙的な状態昇格と減衰を用いる。Readback と D3D12 InfoQueue の Error／Corruption 件数を確認
- 複数 Queue の全組合せ、複数 GPU、全 Legacy API、Release 構成の検証は未完了

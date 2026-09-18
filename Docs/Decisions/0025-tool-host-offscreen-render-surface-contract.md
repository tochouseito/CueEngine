# ADR-0025: Tool Host Offscreen Render Surface Contract

- Status: Accepted
- Date: 2026-09-18
- Decision Owners: CueEngine Project
- Amends: ADR-0019（Tool HostのD3D12 Resource Ownership）

## Context

M22では、Shader、Geometry、Asset Pipelineへ進む前に、固定色でClearしたOffscreen Render Targetを
EditorのGame Viewへ表示する最小Vertical Sliceが必要となる。

ADR-0019で`Cue.ToolHost.WindowsD3D12`はTool用Device、Direct Queue、Command List、Swap Chain、
Frame Resource、ImGui用Shader-visible Descriptor Heapを所有する。一方、`ToolHostClient`へ公開される
Frame境界はImGui Widget構築だけであり、Offscreen Textureの要求、表示、Resize、GPU寿命を表す契約はない。
Runtime側の`PresentationContext`はBack BufferのClear／Presentに限定され、汎用TextureまたはRenderer契約を
まだ持たない。

本ADRはM22最初のGame Viewだけに必要なOffscreen Surface契約を決定する。Shader、PSO、Depth、Camera、
Scene Render Data、Runtime Renderer、FrameGraph、Debug View、複数Surface APIは決定しない。

## Current Boundary

```text
Cue.Editor.Tool
    |
    | ToolHostRenderSurfaceRequest
    | ToolHostRenderSurfaceView (opaque ImGui Texture ID + size)
    v
Cue.ToolHost.WindowsD3D12
    |-- owns Tool D3D12 Device / Direct Queue / Fence
    |-- owns Frame Allocators / Command List / Swap Chain
    |-- owns Offscreen Color Resource / RTV Heap / SRV Slot
    `-- records Clear and Resource Barriers before ImGui Draw

Cue.Editor.ImGui
    |-- receives only opaque Texture ID and dimensions
    `-- emits requested Game View content size

Cue.RHI / Cue.RHI.D3D12 / RuntimeHost
    `-- unchanged by this decision
```

`Cue.Editor.Tool`は最終Composition RootとしてTool Host callbackとEditor ImGui Adapterを接続する。
`Cue.Editor.ImGui`、`Cue.EditorCore`、Runtime ModuleへD3D12、DXGI、COM、Native Descriptor Handleを公開しない。

## Options

### Option A: Runtime RHIへ汎用Texture／Render Pass APIを追加する

将来のGame Rendererと同じResource抽象を利用できるが、Format、Usage、View、Barrier、Pass、Queue、
Descriptor、Thread契約を固定する必要がある。固定色Clearだけに対してM22後半のRenderer Architectureを
先取りするため採用しない。

### Option B: Cue.Editor.ToolまたはCue.Editor.ImGuiがD3D12 Resourceを所有する

Game View実装は局所化できるが、Tool Hostが所有するDevice、Queue、Command List、Fence、Descriptor Heapを
上位へ公開する必要があり、ADR-0019のNative Object非公開規則とGPU寿命の一意所有を破るため採用しない。

### Option C: Tool Hostが一つの汎用Offscreen Surfaceを所有する

Tool Hostの既存Direct Queue、Command List、Fence Domain内でResource Barrier、Clear、ImGui Sampleを順序付ける。
Clientは前Frameで観測したPanel寸法を要求し、HostからD3D12型を含まない短命Viewを受け取る。
最初のGame Viewへ必要な最小境界であり、Runtime RHIを変更しないため採用する。

### Option D: 独立Renderer Moduleを先に追加する

Game View、Debug View、RuntimeHostで共有可能な長期構造を設計できるが、Shader／PSO／Render Data契約が未確定な
段階でRenderer責務を固定する。M22最初のClear Gateには過剰なため、固定Geometry描画へ進むResearchまで延期する。

## Decision

Option Cを採用する。

### Public Boundary

`Cue.ToolHost.WindowsD3D12`は次のPlatform非依存値だけを`ToolHostClient`へ追加する。

- `ToolHostRenderSurfaceRequest`: 可視状態と要求Pixel幅／高さ
- `ToolHostRenderSurfaceView`: ImGui user textureとしてだけ使える非所有の64-bit IDと実Resource寸法
- `render_surface_request()`: Frame開始時にHostがOwner Threadから呼ぶ要求取得callback
- `render_surface_ready()`: `draw_frame()`直前にHostがOwner Threadから呼ぶ短命View通知callback

Texture IDはD3D12 GPU Descriptor Handleを値として保持できるが、公開契約上はImGui user texture以外へ
変換、演算、保存しない。Viewは次回`render_surface_ready()`またはHost終了までだけ有効とし、Clientは所有しない。
RequestとViewはC++ static library境界だけで使用し、安定ABIまたはPlugin境界としない。

`Cue.Editor.ImGui`は同じ形のEditor固有View／Requestを使用し、`Cue.Editor.Tool`がTool Host型との変換を担当する。
これによりEditor ImGui TargetはTool HostまたはD3D12へ依存しない。

### Ownership and Lifetime

| Object | Owner | Lifetime Rule |
| --- | --- | --- |
| Offscreen Color Resource | Tool Host | Deviceより短く、GPUが参照する最後のFence完了より長く保持する |
| Surface RTV Heap | Surface Generation | Color Resourceと同じGenerationで生成・退役する |
| Surface SRV Slot | Tool Host Descriptor Pool | Generationごとに一SlotをLeaseし、最後のSample Fence完了後だけPoolへ返す |
| Retired Generation | Tool Host | 退役時点の`lastSignaledFence`と共に保持し、Completed Value到達後に解放する |
| Surface View | ToolHostClient | 非所有。次回通知またはHost終了で失効する |
| Requested Size | Editor Presentation State | Game Viewを描画したOwner Thread上で次Frame向けに更新する |

Surface Generationの作成は新Resource、RTV Heap、SRV Slot、Descriptor書込みが全て成功してからActiveへ公開する。
途中失敗では新Generationだけを破棄し、既存Active GenerationとClient Viewを変更しない。

### Frame Sequence and Resource State

一つのOwner Threadで次を順序付ける。

```text
wait selected Frame Slot reuse fence
    -> collect retired generations whose fence completed
    -> read previous Game View size request
    -> atomically create/replace/retire surface generation
    -> notify current non-owning view
    -> reset allocator and command list
    -> begin ImGui frame and build Game View widgets
    -> transition surface PIXEL_SHADER_RESOURCE -> RENDER_TARGET
    -> clear surface
    -> transition surface RENDER_TARGET -> PIXEL_SHADER_RESOURCE
    -> render ImGui draw data sampling the surface
    -> execute / present / signal
```

Surface Resourceは`PIXEL_SHADER_RESOURCE`で生成し、各Clearで上記2 Barrierを記録する。ClearとImGui Sampleを
同じDirect Command Listへ記録するため、Cross-Queue Fenceまたは外部State Ownershipを追加しない。

### Resize, Hidden, and Zero Size

- Content幅または高さが1 Pixel未満、WindowがCollapse／非表示の場合はInactive Requestとする
- Inactive Requestでは新ResourceまたはClear Workを作らず、Active Generationを最後のSignal値で退役させる
- 要求寸法がActive Generationと同じ場合は再生成しない
- 寸法が変わる場合は新Generationを先に生成し、既存Generationを退役させる
- Panel寸法はImGui Frame中に確定するため、Surface実寸の更新は次Frameに反映する
- 更新待ちの1 Frameは直前Textureを現在Panel寸法へ表示し、初回だけ準備中表示を許容する
- 一辺の上限は16,384 Pixelとし、Editor Adapterは要求を上限内へClampする

### Fence and Deferred Release

退役Generationへ保存するFence値は、そのGenerationを最後にImGui Sampleへ使用したSubmitを覆う
`lastSignaledFence`とする。0は未提出を表し、直ちに解放できる。1以上は`GetCompletedValue()`が対象以上に
到達した場合だけResource、RTV Heap、SRV Slotを解放する。

Frame Slot再利用Waitは選択Slotだけを覆うため、退役Generation解放判定を代替しない。退役配列は
Frames in Flight数以上を保持し、容量不足を暗黙上書きしない。容量不足は診断可能なErrorとしてFrame受付を停止し、
既存`lastSignaledFence`の有限Drain後だけCleanupする。

Fence Completed Valueが`UINT64_MAX`なら完了値として扱わず、ADR-0019のDevice Removal経路へ移る。
完了もRemovalも証明できない場合はResourceを解放しないFatal規則を維持する。

### Error Contract

- 不正寸法は`ToolHostError::RenderSurfaceInvalidSize`
- ResourceまたはRTV Heap生成失敗はNative HRESULT付き`RenderSurfaceCreationFailed`
- SRV Slot不足は`RenderSurfaceDescriptorExhausted`
- 退役保持容量不足は`RenderSurfaceRetirementCapacityExceeded`
- Device Removalは先行Surface ErrorをCauseとして`ToolHostError::DeviceRemoved`へ再分類する
- Surface準備失敗では現在FrameをExecuteせず、既存`lastSignaledFence`を有限DrainしてからCleanupする
- D3D12 APIが戻り値を持たないBarrier、RTV／SRV書込み、ClearはDebug Layer／InfoQueue検証対象とする

自動再生成、同一Process内Device Recovery、失敗を準備中表示へ変換して継続するFallbackは行わない。

### Adapter Selection for Validation

`ToolHostDescriptor`へ`HardwarePreferred`と`Warp`を持つAdapter Preferenceを追加する。
Production既定は`HardwarePreferred`とし、適合Hardwareがなければ従来どおりWARPへFallbackする。
Testは`Warp`を明示して同じSurface SmokeをSoftware Adapterでも実行する。このPreferenceはTool Host専用であり、
Runtime RHIのAdapter Policyを変更しない。

## Validation

- Public Header Compile TestでD3D12、DXGI、COM型が公開されないことを確認する
- Headless Editor ImGui TestでGame ViewがDock対象になり、可視／Collapse時のRequestが切り替わることを確認する
- Tool Host SmokeでSurface生成、固定色Clear、Resize Generation、非表示退役を最低3 Frame実行する
- 同じSmokeをHardware Preferredと明示WARPの両方で実行する
- Debug Layer／InfoQueueにBarrier、Descriptor、Resource Lifetime Errorがないことを確認する
- Debug、Development、Release Buildと関連CTestを実行する
- 実WindowでGame View表示、Panel Resize、Collapse／Restore、終了を確認する

## Consequences

### Positive

- D3D12 Resource、Descriptor、Command、Fence所有権を既存Tool Host内へ閉じ込められる
- Editor／Runtimeの上位ModuleへNative Graphics型を公開せずGame Viewの最小描画経路を成立できる
- Resize中もGPU完了前ResourceまたはDescriptorを再利用しない
- Runtime Renderer APIをClearだけの要件で先取りしない

### Trade-offs

- Tool Host内のD3D12実装責務が一時的に増える
- Panel実寸反映はImGui計測の都合で1 Frame遅れる
- 初期契約は一つのOffscreen Surfaceだけで、Game ViewとDebug Viewの同時表示を扱えない
- SurfaceごとのRTV Heapは最小実装として明瞭だが、将来のDescriptor ArenaよりObject数が多い

## Follow-up

- Issue #332で本契約に従う固定色ClearとGame View表示を実装する
- Shader／PSO／固定Geometryへ進む前にRuntime共有可能なRenderer／RHI Resource境界を別Research Issueで決定する
- Debug Viewまたは複数Viewportが必要になった時点でStable Surface ID、複数Surface Collection、Layout永続化を決定する

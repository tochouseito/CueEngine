# ADR-0027: Camera, Render Data, and Editor View Boundaries

- Status: Accepted
- Date: 2026-09-18
- Decision Owners: CueEngine Project
- Relates to: ADR-0011（Custom Math Convention）、ADR-0026（Built-in Cube Mesh Contract）

## Context

M22では、SceneのMain Cameraを使うGameViewと、Editor専用DebugCameraを使うDebugViewが必要となる。
現在のGameViewはToolHostが所有する単一Offscreen Render Targetを固定色でClearするだけであり、Scene、GameCore、Rendererとは接続されていない。

CameraとMeshをScene Componentとして永続化すると、Play開始時には同じDataをRuntimeWorldへ実体化する必要がある。
一方で、GameCoreをRendererへ依存させること、RuntimeWorld PointerをEditor UIへ公開すること、DebugCameraをSceneへ保存することは既存のArchitecture Invariantに反する。
また、GameViewとDebugViewは独立SizeのGPU Resourceを持つため、ToolHostのDescriptorとFence契約を維持した複数Surface所有が必要となる。

## Decision

### Module and Dependency Direction

- Portableな`Cue.Renderer` Moduleを追加する
- `Cue.Renderer`は`Cue.GameCore`、`Cue.Scene`、`Cue.Schema`、`Cue.Math`、`Cue.EngineAssets`を利用する
- `Cue.GameCore`、`Cue.Scene`、`Cue.Runtime`から`Cue.Renderer`への依存は追加しない
- Windows／D3D12 GPU Resourceと描画Commandは`Cue.ToolHost.WindowsD3D12`が所有する
- Editor UIはGPU Resourceを所有せず、ImGui Texture IDを含む非所有Surface Viewだけを受け取る

### Authoring and Runtime Components

- Main CameraはObject名ではなく`Cue.Renderer.Camera` Componentの`isMain` Fieldで明示する
- Camera Componentはvertical FOV、near plane、far planeを保持する
- Meshは`Cue.Renderer.Mesh` ComponentのStable Asset Referenceで指定する
- M22では`cue://engine/mesh/cube`だけを解決対象とし、Project AssetやFile Pathを直接読み込まない
- Camera／MeshのStable Type ID、Field ID、Schema Versionを`Cue.Renderer`が所有する
- Editor Composition RootはCore SchemaとRenderer Schemaを同じRegistryへ明示登録する
- Standalone RuntimeHostへのRenderer Schemaと描画Loop統合はM22のEditor View検証後へ延期する

### Runtime Registration and GameCore Connection

- `RuntimeSystemRegistration`はSystemと同時に、同じSessionで必要な`RuntimeComponentBuilderFactory`を返せるようにする
- Runtime Application SessionはWorld初期化後、Scene実体化前にFactoryからSession-local Builderを生成する
- BuilderがCamera／Mesh Component TypeをWorldへ登録し、Scene Componentを型付きRuntime Componentへ変換する
- Renderer Runtime SystemはTransformとCamera／Meshの型付きQueryだけを使用してCPU所有のRender Snapshotを生成する
- GameCoreはRenderer型や描画APIを認識しない
- Render SnapshotはMatrix、Camera値、Built-in Mesh IDの所有Valueだけを保持し、World、Entity、ComponentへのPointerを保持しない

### Main Camera Selection

- ActiveなCamera Componentのうち`isMain == true`であるものをMain Camera候補とする
- 候補が正確に一つならGameView Cameraとして使用する
- 候補が0または2以上の場合はSnapshotを描画不能状態とし、GameViewは安全な診断Clearへ退避する
- 複数候補をObject順で暗黙選択しない
- CameraのTransform ScaleはView Matrixへ使用せず、TranslationとRotationだけを使用する

### Debug Camera

- DebugCameraはEditor Tool Sessionが所有する値であり、SceneDocument、Undo／Redo、Scene File、RuntimeWorldへ保存しない
- 初期DebugCameraはScene原点を見下ろす固定Poseとし、操作入力は別Issueへ延期する
- DebugViewはEdit中とPlay中のどちらでも同じDebugCameraを使用し、描画対象Snapshotだけを切り替える

### Coordinate and Projection Convention

- ADR-0011の左手World Space、row-vector、row-major Matrix規約を継続する
- Camera forwardはlocal `+Z`とする
- Perspective ProjectionはDirect3DのDepth Range `[0, 1]`を使用する
- Aspect Ratioは各Panelの現在のRender Surface Sizeから計算する
- FOV、near、farは有限値かつ`0 < fov < 180`、`0 < near < far`を満たす場合だけ描画に使用する

### GPU Surface Ownership and Synchronization

- ToolHostはGameViewとDebugViewの二つのSurface Slotを固定Identityで所有する
- 各SlotはColor Texture、RTV、SRV、Depth Texture、DSV、Size、Generationを所有する
- Panelごとに独立して生成、Resize、非表示化できる
- Resizeまたは破棄前に既存FrameのFence完了を待ち、GPU参照中Resourceを解放しない
- SRV DescriptorはToolHostのImGui Descriptor Poolから割り当て、Surface Viewは次の再生成までだけ有効とする
- ClientはUI描画完了後にCPU Render SnapshotとGame／Debug Camera値を非所有Viewで返す

## Deferred Decisions

- DebugCameraのFree-look、Orbit、Focus操作
- Camera Priority、複数Game Camera、Camera Stack、Viewport Rect
- Orthographic Camera
- Project Mesh Asset、Asset Import、Cook、Runtime Bundle
- Material、Texture、Lighting、Shadow
- Standalone RuntimeHostのGame描画Loop統合
- GPU Driven Rendering、Mesh Shader

## Consequences

### Positive

- GameCoreのRenderer非依存を保ったままRuntime Entityを描画Dataへ変換できる
- GameViewとDebugViewが同じScene Snapshotを異なるCameraで描画できる
- DebugCameraがProject DataやGame実行状態を汚染しない
- GPU Resourceの所有と同期を既存ToolHost境界へ集約できる

### Negative

- Runtime System登録APIへScene Component Builder Factoryを追加するため、既存Composition RootとTestの更新が必要となる
- M22時点ではBuilt-in Cube以外のMeshを描画できない
- Camera入力を延期するため、初期DebugViewは固定Poseとなる
- Main Camera不備は描画不能として現れるため、Editor診断表示が必要となる

## Validation

- Camera／Mesh Schema、Value Schema、Component Templateを単体Testする
- Scene ComponentがRuntimeWorldへ実体化され、Renderer Runtime SystemがPointer非保持Snapshotを生成することをTestする
- Main Camera 0件、1件、複数件をTestする
- GameView／DebugViewのSurface生成、独立Resize、非表示化をToolHost Smoke Testで確認する
- Built-in CubeをMain CameraとDebugCameraの両方から描画し、Editor上で目視確認する
- Debug、Development、ReleaseでBuildとCTestを実行する

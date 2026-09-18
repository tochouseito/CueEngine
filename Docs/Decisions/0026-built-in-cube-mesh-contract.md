# ADR-0026: Built-in Cube Mesh Contract

- Status: Accepted
- Date: 2026-09-18
- Decision Owners: CueEngine Project
- Relates to: ADR-0011（Custom Math Convention）

## Context

M19では、Project AssetやAsset Importに依存せず利用できるEngine所有の基本Primitiveが必要となる。
最初の対象はCubeであり、M22の固定Geometry描画と将来のScene Mesh Componentから同じGeometryを参照できる必要がある。

一方、Asset Database、Source AssetからRuntime Assetへの変換、Runtime Bundle、汎用Mesh形式、Material、UV規約は未確定である。
これらをCube追加だけで固定すると、Asset Pipeline着手時に不要な互換契約を残す。

本ADRはBuilt-in CubeのIdentity、Geometry規約、所有権、寿命だけを決定する。Scene Component、GPU Resource、Renderer、
Asset Pipeline、配布Bundle全体の形式は決定しない。

## Decision

### Ownership and Identity

- `Cue.EngineAssets`をEngine所有のFirst-party Moduleとして追加する
- CubeのStable Asset IDは`cue://engine/mesh/cube`とする
- 初期Geometry Revisionは`1`とする
- IDはProject内のFile Pathを表さず、Projectによる同名File Overrideを許可しない
- Cube GeometryはModule内の不変な静的Storageが所有する
- Public APIは非所有の`std::span`を含むViewを返し、呼出し時にAllocationまたは所有権移譲を行わない
- ViewはProcess終了まで有効とし、Thread間の同時Readを許可する

### Geometry Convention

- ADR-0011の左手World Space、`+X = Right`、`+Y = Up`、`+Z = Forward`に従う
- Cubeは原点中心、一辺`1.0`、各軸のBoundsを`[-0.5, 0.5]`とする
- FaceごとのFlat Normalを保持するため、8共有頂点ではなく24頂点を使用する
- 各頂点はPositionと単位長Face Normalだけを保持する
- 6 Faceを各2 Triangleで表し、36個の16-bit Indexを使用する
- Triangleは外側から見て反時計回りとし、右手則の代数Cross Productが外向きNormalと一致する
- Face順や頂点Index順は永続形式またはABIとして扱わず、Geometry Revision内の実装詳細とする

### Build and Distribution

- Cube GeometryはC++の静的Dataとして`Cue.EngineAssets`へCompileする
- 現在のStatic Link構成では最終ConsumerのBinaryへ取り込まれ、Loose Asset Fileを配布しない
- 将来Runtime Bundleへ移行する場合もStable Asset IDを維持し、Storage変更は別ADRで扱う

## Deferred Decisions

- Texture UV Origin、UV展開、Tangent生成
- Material、Texture、Error Resource
- Scene Mesh ComponentとSerialization Schema
- GPU Buffer、Upload、Resource Lifetime、Renderer接続
- Asset Import、Cook、Runtime Bundle、未使用Built-in Asset除外
- Sphere、Plane等の追加Primitiveと共通生成API

## Consequences

### Positive

- RendererやEditorへ依存せずCube Geometryを検証できる
- Stable Asset IDを先に固定し、後続のScene参照とStorage移行で同一性を維持できる
- 不変Storageと非所有Viewにより、呼出し時のAllocationと寿命管理を不要にできる

### Negative

- CubeをSceneへ追加しても、Mesh ComponentとRendererが実装されるまでは描画されない
- UVとTangentを持たないため、Texture付きMaterialへ直接使用できない
- 静的Dataは利用されるConsumerへLinkされるため、将来の未使用Built-in Asset除外には別のPackaging判断が必要となる

## Validation

- Stable Asset IDとRevisionを確認する
- Vertex数24、Index数36、Index範囲を確認する
- Bounds、Normal長、Triangle非縮退、WindingとNormalの一致を確認する
- Public Headerが単独Compileできることを確認する
- Debug、Development、ReleaseでBuildとCTestを実行する

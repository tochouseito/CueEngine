# M11 組み込み Cube / MeshPool の実装と検証

- 日付: 2026-11-01
- Branch: `m11/issue146/project-loading`
- 指示: 前回設計の 2（組み込み Cube）、3（MeshPool）を実装する
- 前提: 必要な MeshData も追加する。Bounds は追加しない
- 契約: `Docs/Decisions/0013-mesh-data-and-paged-mesh-pool.md`

## 変更

### CPU Geometry

`Engine/Source/Assets` に `Cue.Assets` を追加した。
`MeshData::create` は分離 Position / Normal / UV0、Uint16 / Uint32 の Index を検証して所有する。
不正な属性数・非有限値・ゼロ Normal・TriangleList 不成立・Index 範囲外を拒否する。
GPU へ転送する float32 成分列は DX12 側で明示的に詰め、Math 型の Memory Layout に依存しない。

`create_cube_mesh` は原点中心・一辺 1、面ごとに分離した 24 頂点 / 36 Index、外向き CCW の Cube を生成する。
組み込み ID は `cue://engine/mesh/cube`。Geometry の生成だけであり、Asset Catalog や Scene Component との接続は後続。

### GPU 常駐管理

`RHI/MeshPool.h` に Backend 非依存の `MeshHandle` / `IMeshLease` / `IMeshPool` を追加した。
具体実装は `DX12/DX12MeshPool.h` と `DX12MeshPool.cpp`。
`DX12Backend::create` が Pool を所有し、`get_mesh_pool` で借用できる。
GPU Page は最初の upload で確保する。

Page は Position / Normal / UV0 / Index の個別 Committed Default Buffer を持つ。
属性構成と Index 幅で分け、空き範囲不足や使用中の Page には別 Page を追加する。
既存 Mesh の移設はしない。描画範囲は Page View と baseVertex / firstIndex / indexCount で表現する。

Copy Queue で COMMON → COPY_DEST → COMMON として転送し、Fence 完了後だけ acquire できる。
SwapChain が Graphics Queue を借用したままでも upload できる。
GPU 利用は Lease から最終 Completion を登録し、CPU / GPU 参照が終わるまで Page を保持する。
retire 後は新規 acquire を拒否し、collect が領域と Slot を世代付きで再利用する。

## 実行した検証

- `pwsh -NoProfile -File scripts/codex_build.ps1`: Debug Build 成功
- 最終 Test 用 Fence 修正後: `cmake --build --preset windows-vs2026-debug --target CueDX12MeshPoolTests` 成功
- `Cue.Assets.MeshData` / `Cue.Assets.PublicHeaders`: 2 / 2 成功
- `Cue.Renderer.DX12.MeshPool`: WARP 成功（0.21 秒）
- 全 Debug CTest: **81 / 81 成功、66.98 秒**
- 最終 Engine / Test Build Log の Compiler Error / Warning: なし
- `git diff --check`: 成功

MeshPool の WARP Test は次を検証する。

1. すべての Copy Queue を Test Fence で止め、転送前の acquire を拒否する
2. 同 Page の既存 Mesh も、追加転送が終わるまで新規 acquire を拒否する
3. Page を共有した 2 個目の Cube の baseVertex=24 / firstIndex=36 を確認する
4. Position / Normal / UV0 / Index を Graphics Queue から Readback し、元 Geometry と照合する
5. retire 後も CPU Lease と GPU Completion が残れば回収せず、終了後に世代を変えて領域を再利用する
6. CPU Pin / 未完了 GPU Read が残る Page へ追加転送しない
7. Uint32 / 属性なしの別 Page と、省略属性の空 View を確認する
8. Uint16 Mesh を baseVertex=65536 に配置しても Index を書き換えず正しく読み戻せる
9. 容量不足・古い世代・異 Pool・停止後の呼出しを拒否する
10. 転送中に retire された Mesh を Fence 完了前に回収しない

初回の WARP Test は Test 用 CopyGate の Fence を保持したまま Backend の Live Object 診断に入り、
Debug Layer の LIVE_DEVICE / LIVE_MONITOREDFENCE 警告で終了コード 2170（例外 0x87a）になった。
cdb で停止経路を確認し、Queue 完了確認後に Test Fence を解放する順序へ修正した。
Engine の Debug Layer や LeakChecker を無効化して回避したものではない。
診断証拠は `out/validation/MeshPoolCdbStack.log`。

最終 Build Log は `out/validation/MeshPoolBuildVerified.log`、Test 修正の Build Log は
`out/validation/MeshPoolTestBuild.log`、個別 Test Log は `out/validation/MeshDataCTest.log` / `MeshPoolCTest.log`。
全 Debug Test の Log は `out/validation/MeshPoolFullCTest.log`。

## 未実施と制約

- Cube の実描画、Camera / Depth / MeshPass、ExecuteIndirect 提出、Editor Scene View 接続は次の工程
- 今回の MeshPool 固有の Hardware GPU / Development / Release / Windows 以外での検証は未実施
- OOM / Device Lost の実機故障注入は未実施。Completion Error の伝播は制御用 Token で確認した
- 静的 Geometry が対象。動的 Mesh の更新、Batch 転送、Defragmentation は追加していない
- 同 Page の別 Mesh への転送中も一時的に acquire できないため、描画側は status を確認する
- 借用返却と final Completion 登録、COMMON 復帰は利用者の契約として守る必要がある
- 既存の未 Commit 変更は保持した。Commit / Push / Merge / Branch 作成 / GitHub 更新は行っていない

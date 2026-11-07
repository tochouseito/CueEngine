# ADR-0006: FrameGraph と固定 Mesh の最小描画経路

- Status: Accepted
- Date: 2026-09-26
- Issues: [M04-07](https://github.com/tochouseito/CueEngine/issues/25)、[M04-08](https://github.com/tochouseito/CueEngine/issues/26)、[M04-09](https://github.com/tochouseito/CueEngine/issues/27)、[固定三角形 Pass](https://github.com/tochouseito/CueEngine/issues/32)
- 関連: [ADR-0005](0005-minimal-renderer-presentation.md)

## 決定

`FrameGraphBuilder` の公開契約は D3D12 型を含めない。Frame ごとに Resource の初期・最終状態、寿命、Pass ごとの使用状態と読書きを宣言する。Builder は矛盾する使用、別 Graph の Handle、未生成の一時 Resource の読み、依存の循環を拒否し、直列実行順と Transition／UAV Barrier を作る。D3D12 側の `D3D12GraphExecutor` がこの計画を一つの Direct Command List に記録する。`Present` は Graph の外で Submit 後に行う。

Back Buffer は `PRESENT` 状態で Import し、終了時も `PRESENT` に戻す。Frame Slot ごとの Offscreen Color は Graph 上では一時 Resource とし、物理 Resource は対応する Fence の完了まで `D3D12ResourcePool` が保持する。`D3D12SurfacePool` はその Handle と既存の固定描画 View を管理する。Depth は Resize 間で永続する。どちらも Graph の終了時に `COMMON` に戻す。Clear、固定 Mesh、Back Buffer への Copy の順で実行する。最初の実装では Resource Aliasing と非同期 Queue を使わない。

`D3D12ViewManager` は Back Buffer と Offscreen の RTV Slot、および Depth の DSV を管理する。Shader-visible CBV Heap は `D3D12PipelineCache` が別に所有する。定数は 256 Byte 境界で Frame Slot ごとに分け、対応 Fence 完了後だけ更新する。`D3D12PipelineCache` は固定 Mesh 用 Shader、Root Signature、Graphics PSO を一度生成して再利用する。`D3D12StaticMeshPool` は固定 Triangle の Vertex／Index を Upload Heap から Default Heap へ転送し、転送完了後に Upload 資源を解放する。Mesh Handle は世代を持ち、破棄後の使用を拒否する。

汎用の Buffer、Color／Depth Texture と RTV／DSV／SRV は `IGpuResources` の世代付き Handle から生成する。`D3D12ResourcePool` は解放前に Graphics／Compute／Copy Queue を待ち、View が残る Resource の解放を拒否する。固定描画の View は既存の `D3D12ViewManager` が引き続き所有する。SRV を固定 Mesh Pass に Bind する処理はこの決定の範囲外とする。

固定三角形の Shader は `Engine/Shader/FixedMesh.hlsl` を正本とし、CMake が Build Directory に配置する。`D3D12PipelineCache` はその File を読み込んで PSO を生成する。旧 CueEngine の `FrameGraphPass` と同じく、D3D12 内部の `D3D12TrianglePass` が `setup` で Color／Depth の使用を宣言し、`execute` で固定 Mesh の描画を記録する。Renderer は Pass の組み立てと Graph 実行を調整する。

Resize 時は GPU 完了を待ち、Command List を解放してから Back Buffer と Offscreen／Depth を更新する。途中失敗後は同じ Size でも再試行する。停止時は Worker を止めた後に GPU 完了を確認してから資源を破棄する。

## 検証

Graph の依存、循環、不正 Handle、未生成 Resource、Transition／UAV Barrier を単体 Test で確認する。WARP の固定 Mesh Test は Readback Pixel で中心と背景の違いを確認し、D3D12 Debug Layer の Error／Corruption Message を拒否する。実 Window の連続 Frame と Resize／最小化／復帰を Host Test で確認する。Debug／Development／Release の Build と CTest を実行する。

## 範囲

固定 Triangle のみを描画対象とする。汎用 Scene、Material、Texture Import、Resource Aliasing、Async Queue、ImGui 描画、Indirect／LOD は別の設計で扱う。

# ADR-0014: Camera と最初の Scene 描画経路

- 日付: 2026-11-02
- 状態: Accepted
- 対象: 設計手順 4。RotateCube Scene を Editor の Scene View に表示する

## Camera と保存

CameraComponent は Runtime に置き、ECS Module は汎用管理だけを担当する。
Component は垂直視野角（Radian）、Near / Far の透視投影設定を所有し、位置・回転は TransformComponent から得る。
Aspect は保存せず、実際の FinalColor の幅 / 高さから計算する。

Math の `perspective_rh_zo` と `view_from_transform` を使い、Runtime / Renderer 内に行列の計算式を重複させない。
規約は右手系、上方向 +Y、Camera 正面 -Z、行ベクトル、`World * View * Projection`、NDC Depth 0 ～ 1。
View は有限な Transform Scale を無視し、位置・正規化回転の剛体姿勢だけを逆変換する。
Clip / FOV / Aspect の不正値や計算範囲外は Result で拒否する。

Scene v1 に任意の Camera / Mesh Component を追加する。従来の Transform だけの Scene は引き続き読める。
Camera / Mesh を持つ Object には Transform を必要とし、不完全な設定や未知の投影方式は拒否する。

```text
mesh.assetId=cue://engine/mesh/cube
camera.projection=perspective
camera.fovYRadians=1.0471976
camera.near=0.1
camera.far=1000
```

この時点で Mesh ID は組み込み Cube のみ受け付ける。Scene に MeshHandle / GPU Pointer を保存しない。
SceneRuntime は全必要 Component の型容量を事前検証し、World への実体化にも対応する。

## World と Renderer の接続

`Cue.Renderer.SceneRender` の公開 Data は Math と所有値だけで表現する。
接続処理 `extract_render_scene(World&)` の実装だけが Runtime World を読み、Renderer 本体は World を保持しない。

- 同期済みで有効な Transform + MeshRendererComponent を抽出する
- 有効な Camera が複数ある場合は明示選択がまだないため失敗する
- Mesh がある場合は Camera が一つ必要。従来の Camera / Mesh のない Scene は Clear のみで動く
- Dirty Transform は update 境界で同期してから渡す
- Snapshot は Camera の View / 投影値、Object の行列 / Mesh ID を所有し、World 破棄後も独立する
- 抽出時の入力検証や Allocation 失敗を World の System 失敗へ波及させない

Editor Preview World の Owner は Main Thread のままとする。
Main が `update(0ns)` で Transform を同期し、採用 Frame ID とともに Snapshot を公開する。
Render は同じ Frame ID の Snapshot と ImGui Snapshot を消費し、取消・失敗でも借用を終える。
これは Preview の経路であり、将来の Play World / Script 更新 Thread を決定するものではない。
現時点の抽出は UI 構築前。手順 5 の編集操作を同 Frame へ反映する場合は公開順を見直す。

## MeshPass

`create_scene_renderer` は Backend へ対応する実装を生成し、初期 Cube を MeshPool に転送する。
初回だけ `wait_ready` で転送完了を待つ。毎 Frame の Geometry 再生成・転送は行わない。
追加する Pass の名前は `MeshPass` とし、PIX Marker / GPU 計測でも同じ名前を使う。

Graph の順序は ClearFinalColor → MeshPass → 表示 Pass。
MeshPass は FinalColor RenderTarget と枠ごとの SceneDepth を使い、Depth Clear は 1、Depth 比較は Less。
Depth Texture は D32Float、DSV は Backend の専用 Dsv Heap に置く。
Depth / Color の形状・用途・PSO と DSV の組合せを検証する。

Shader は `Engine/Shader/Hlsl/StaticMesh.hlsl` に置く。
Position / Normal を別の VB Slot から読み、最初は Normal に基づく固定面色を出す。
Material / Texture / Lighting 管理はこの工程に含めない。

同じ Page の VB / IB を一度 Bind し、CPU が作成した Indirect 引数を使って ExecuteIndirect する。
Signature は DWORD の Transform Index + DRAW_INDEXED の 24 Byte。
Root 0 は Draw Index、Root 1 は行列 StructuredBuffer の GPU VA とし、各 Draw は別の World 行列を参照する。
Uint16 は Mesh 内の Index のまま使い、MeshPool の firstIndex / baseVertex で範囲を指定する。

行列 / Indirect 引数の Upload と Default Buffer は GPU 描画枠ごとに確保して再利用する。
FrameGraph の begin_frame が同じ枠の前回 Fence を待った後だけ CPU 上書き・Lease 返却を行う。
Copy と Draw は同じ Graphics Command List 内で並べ、Default Buffer は終了時 COMMON に戻す。
MeshLease は GPU 完了まで Frame に保持し、Graph の GPU 完了・破棄後に返却する。
GPU 完了前に Lease を返す利用経路では、従来どおり mark_submitted の登録を必要とする。
Bounds / GPU カリングは追加しない。

## Editor の Scene View

既定の Editor 表示 Pass は FinalColor の ShaderRead と BackBuffer の RenderTarget を宣言する。
Scene View の ImGui Image は Native Texture ではなく予約した論理 Texture ID を保持する。
Render 記録時に現在の Graph の物理 Frame の FinalColor を取得し、UI 専用 Heap の SRV に接続する。
ImGui 描画 Snapshot 内の論理 ID だけを一時的に置換し、記録後に元へ戻す。

Scene 用 SRV は GPU 枠ごとに分離し、前回の使用 Fence 完了後だけ書き換える。
Resize では CPU / GPU を排出して Graph の Color / Depth を作り直し、古い Texture を次 Frame の UI へ渡さない。
Scene View Panel のサイズ変更は画像の Aspect を保つ表示拡縮とし、描画解像度は Window のサイズを使う。
独自の表示 Pass を指定した利用者へ既定 Scene View を注入しない。

停止は Runtime / Worker Join → Graph GPU 完了・破棄 → SceneRenderer → ImGui → Backend の順。
Scene 用 SRV は UI Queue の完了後に返し、Descriptor Heap を GPU 実行中に再利用しない。

## 到達点と後続

Sample に Cube の Mesh 参照と Camera を追加し、静的な初期回転で三面を確認できるようにする。
この工程の完了は Cube の Editor 表示まで。手順 5 の追加ボタン・Inspector・保存操作、Script 回転、
Editor からの製品 Build / 起動は後続とする。

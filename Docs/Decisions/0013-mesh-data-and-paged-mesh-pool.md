# ADR-0013: CPU MeshData と Page 単位の静的 MeshPool

- 日付: 2026-11-01
- 状態: Accepted
- 対象: RotateCube の組み込み Cube と MeshPool。Camera / MeshPass / Editor 操作は後続

## 境界

`Cue.Assets` は不変の CPU Geometry を所有する。Position は必須、Normal / UV0 は任意とし、
存在する属性は Position と同数を持つ。同じ Index が全属性を参照する。
Normal は生成時に正規化する。TriangleList のみ対応し、Index は Mesh 内の頂点番号とする。
Bounds は今回追加しない。Transform / Material / GPU Handle / 永続 Asset Identity は Geometry 本体へ含めない。

組み込み Cube は原点中心・一辺 1、外向き CCW、面ごとの Normal / UV を持つ 24 頂点 / 36 Uint16 Index とする。
予約 ID は `cue://engine/mesh/cube`。Factory は独立した CPU Geometry を返し、可変 Global Cache を持たない。
ID の Catalog・Scene Component・Package 収録経路は後続とする。

`IMeshPool` は Backend に依存しない Renderer 契約、`DX12MeshPool` は具体実装とする。
Backend が Pool を生成・所有する。初期化時には Page を作らず、最初の Upload で確保する。
Runtime World や Scene はこの Pool を所有しない。

## Page と描画範囲

- Position / Normal / UV0 / Index はそれぞれ別の Committed Default Buffer に置く
- 同じ属性構成・Index 幅の Mesh を同じ Page にまとめる
- Page 全体の VB / IB を Bind し、Mesh ごとの `baseVertex` / `firstIndex` / `indexCount` を渡す
- Index は書き換えない。Uint16 Mesh でも Page 内の配置頂点位置は 65536 を超えられる
- View の Byte 容量は UINT、BaseVertex は INT の範囲を守る
- 不足時は Page を追加し、既存 Mesh を移動しない。大きい Mesh は必要量へ拡大した新 Page に置く
- 空き範囲は頂点・Index 別に再利用し、隣接範囲を結合する
- 最大 Page / Mesh 数に達した場合は待機や無制限拡張をせず、Result で容量不足を返す

既定は 65536 頂点 / 196608 Index、最大 64 Page / 4096 Mesh。
`DX12MeshPool::create` の `MeshPoolConfig` で変更できる。

## 転送と公開

唯一の Graphics Queue は SwapChain が排他的に借りるため、転送には Copy Queue を使う。
CPU Geometry は呼出中に専用 Upload Buffer へコピーする。戻った後は元 Geometry を破棄できる。
全転送資源・共有 Completion・Slot 保持先は提出前に確保し、成功した submit の後で Allocation しない。
提出結果不明の失敗は既存 CommandPool の致命的失敗処理へ従い、通常の再試行可能な失敗と区別する。

Buffer は COMMON → COPY_DEST → COMMON と遷移する。
Handle は Uploading で返し、`status` の Fence 完了確認後だけ Ready として `acquire` できる。
CPU が Copy 完了を確認してから描画対象へ公開するため、描画側の追加 Queue Wait は不要。
Device Lost は未完了として隠さず Error を返す。

別範囲でも Buffer の State は Page 全体に影響する。
CPU Lease または未完了 GPU Read がある Page には追加転送せず、別 Page を使う。
同 Page の別 Mesh 転送中は既存 Mesh も一時的に Uploading とし、新規 acquire を拒否する。
利用者は Ready の Mesh だけを Snapshot に含め、Unavailable の Mesh を黙って描画しない。
この初期版は転送の一括 Batch・移設・圧縮・動的 Mesh 更新を実装しない。

## Snapshot と遅延破棄

`MeshHandle` は Pool ID / Slot / 世代を持つ非所有参照。
CPU Snapshot は `meshLease` を所有して Page を Pin し、Handle だけを Frame 越しに保存しない。
Buffer は読み取り専用で借用し、描画後は COMMON へ戻すか、Buffer の暗黙 promotion / decay のみを使う。

Lease は一度だけ `mark_submitted` する。完了点は同一 Queue Timeline の最後の提出、または全 Queue を Join した提出とし、
すべての参照を覆う。異なる Queue の無関係な最後の Token だけでは寿命を保証しない。
登録が失敗した場合は Lease を返さず、保持したまま再試行する。

`retire` は新規 acquire を即時に拒否する。取得済み Lease は停止前なら提出できる。
転送完了・全 CPU Lease 返却・全 GPU Read 完了の三条件を満たすと、`collect` が領域を戻し世代を進める。
世代を使い切った Slot は再利用しない。
`collect` は定期的に呼び、生存中 Mesh の完了 Token と転送用一時資源も回収する。

公開操作は内部 Mutex で直列化する。同じ Lease の操作と破棄は呼出側で直列化し、再入を禁止する。
Backend 停止前にすべての Lease を返す。shutdown 開始後の新規提出は禁止する。
借用が残った shutdown は失敗し、返却後に再試行できる。最後の Lease は Pool 本体の破棄後も Page を保持する。
停止は MeshPool → ResourcePool → CommandPool → QueuePool の順とする。

## この段階の到達点

CPU Cube の生成、GPU 常駐 Mesh の転送・範囲参照・寿命管理までとする。
World からの描画 Snapshot 抽出、Camera、Depth、MeshPass、ExecuteIndirect の実提出、FinalColor の Editor 表示は後続。
Editor に Cube が表示された状態とは扱わない。

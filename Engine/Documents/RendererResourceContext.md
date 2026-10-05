# Renderer の依存 Context と View 管理

関連 Issue: #69

`DX12Backend` は Device、DescriptorAllocator、ViewManager、PipelineManager、CommandPool、QueuePool、ResourcePool を所有する。
生成が完了してから、非所有の `DX12ResourceContext` と `DX12ExecutionContext` を公開する。
Context は型付き Getter で参照を渡し、Object の生成・破棄や Global 登録を行わない。

- ResourceContext は Device、ViewManager と PipelineManager を参照する。SwapChain と FrameGraph の生成に使う。
- ExecutionContext は CommandPool と QueuePool を参照する。Graph の記録と提出に使う。
- Pass 記録中の Command と Resource 表は `DX12FrameGraphRecordContext` にまとめる。参照先は記録中だけ有効とする。
- Frame 数、Clear 色、追加 Pass Callback は `DX12MainFrameGraphConfig` にまとめる。
- 枠数と借用 RTV の論理 Handle は `DX12FrameGraphFramesConfig` にまとめる。

利用者は Backend の停止前に停止・破棄する。Context のコピーも停止時に失効する。
Context は依存先の Thread 契約を変更せず、Host は Render 処理を停止してから Backend を停止する。
低水準の Device、Allocator、ViewManager の生成では必要な依存を直接渡し、生成時の循環参照を作らない。

`DX12ViewManager` は RTV/SRV の Resource 所属、用途、Format、Mip 範囲を検証し、Slot を割り当てて Native View を作る。
現行 RHI が扱う単一配列要素・非 MSAA の Color Texture2D が対象である。
Typeless、Depth、Texture Array、Buffer View などは対応機能の導入時に契約を追加する。
Native View 生成 API は HRESULT を返さないため、Native 診断は Debug Layer で確認する。

View Handle は用途と Descriptor の所属・世代を保持する。利用者が Handle を保持し、GPU 完了後に Manager へ返却する。
Manager は Resource を所有しない。Resource の Owner が最終 GPU 利用まで実体を維持する。
外部 Texture は Slot を先に予約し、描画枠の GPU 完了後にその枠の Slot へ View を書き込む。
SwapChain の Back Buffer にも同じ View 生成 API を使い、Graph はその RTV を借用する。

停止順は Render 処理、Graph、SwapChain、Pool、PipelineManager、ViewManager、DescriptorAllocator、Device とする。
Pool の Lease が残る異常停止では既存の共有状態が Native Object と Heap を保持するが、Context からの再取得は許可しない。

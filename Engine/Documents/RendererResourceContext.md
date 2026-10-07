# Renderer の依存 Context と View 管理

関連 Issue: #69

同期・再利用契約の更新: #90、#91、#97、#99

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

## Queue と提出の完了点

`DX12QueuePool` は Graphics 1、Compute 4、Copy 4 個の Queue を生成する。同じ種類でも Queue ごとに独立した Fence Timeline を持つ。

- `IQueueContext::identity()` は Timeline の識別子を返す。Completion が Fence を保持する間は同じ Timeline の識別子として使える。識別子自体は非所有であり、Queue の借用や寿命を延長しない。
- `latest_fence_value()` は Engine の tracked submit または signal が正常発行した最新値を Thread-safe に取得する。Native Queue への外部提出は含まれないため、外部提出の完了点が必要な場合はその後に Engine の Signal を発行する。
- `ICommandCompletion::queue_identity()` と `fence_value()` は、その提出と同じ排他区間で発行した Timeline と値を返す。Graph の依存待機では保持中の Queue の識別子を照合し、この値を使う。提出直後に同じ作業のための Signal を追加しない。
- 識別子と値が 0 の場合は未対応または未設定であり、有効な依存点として扱わない。異なる Timeline の Fence 値を大小比較して提出順を判断しない。

Completion は Queue Lease と独立して Native Fence と Device の寿命を保持する。Queue 間の GPU 待機には引き続き生存する Queue Lease が必要であり、呼出側が循環依存を防ぐ。
Signal 失敗後に GPU 投入済みの可能性がある場合は、単なる失敗として Resource を解放せず既存の Fatal 契約に従う。

`SharedCommandCompletion` は一つの提出 Token を複数の Resource と Graph の返却 Token で共有する補助である。Wrapper は提出前に確保し、成功した提出 Token を `set()` で一度だけ登録してから公開する。公開後に Token を差し替えない。未設定では完了を返さず、`wait()` は InvalidState を返す。

## CommandPool の容量と再利用

`DX12CommandPool` は Graphics、Compute、Copy の Context 数がそれぞれ 0 の状態から始まる。利用時に必要な種類の Allocator と Command List を一組ずつ生成し、種類ごとの総保持数は最大 32 である。Frame ごとに 32 個を追加する方式ではない。

`acquire()` の順序は以下とする。

1. CPU に借用されていない完了済み、または未提出の Slot を Reset して再利用する。
2. 再利用可能な Slot がなく、総保持数が 32 未満なら一組を生成する。
3. 32 到達時に返却済みの GPU 未完了 Slot があれば、Pool 内の提出順で最古を選び予約する。
4. Pool Mutex を解放してその Slot の Fence を CPU で待ち、完了後に停止・Fatal 状態を再検証して貸し出す。

待機する Slot は内部的に予約されるため、別の `acquire()` が同じ Allocator を Reset しない。待機中も他の Slot の提出と Lease 返却は進められる。
全 Slot が CPU 借用中の場合は `DX12CommandPool.acquire.capacity.borrowed` の InvalidState を即座に返す。CPU Lease の返却を同じ Thread で待つ循環を作らない。
GPU 完了前の Allocator Reset は行わず、Device Lost や完了を証明できない失敗では Fatal 状態へ移る。

## ResourcePool の非待機借用と安全待機借用

`acquire()` は従来どおり非待機であり、CPU Lease または GPU 利用との競合に InvalidState を返す。Read 同士は共有でき、Write は排他である。

`acquire_wait()` は GPU 利用との競合だけを CPU で完了待機する。FrameGraph の常駐 Default Resource の連続更新や、Upload 領域へ CPU が書き直す入口に使う。

- active な CPU Writer、または Write 要求に対する active CPU Reader が残る場合は待たずに `active_conflict` を返す。
- GPU の Read と Write、または Write と後続 Read が競合する場合は、保持した Completion の完了を Pool Mutex 外で待つ。
- 待機中は Resource の非所有 Pointer を持ち出さず、Completion の共有所有だけを保持する。
- 待機後に Pool 停止、退役、Handle の世代と Pool 所属、CPU 競合を再検証する。古い Resource を新しい世代として借用しない。
- 待機失敗を呼出側へ返し、完了を確認できない Resource の CPU 上書きや解放を許可しない。

この入口は安全な CPU 待機経路であり、Queue 間の依存を GPU 側の Wait に置き換える機能ではない。Resource State 遷移と Queue 依存の配置は引き続き FrameGraph または呼出側が管理する。
Upload の CPU 書込みは必ず排他の Write Lease で行い、借用期間が終わる前に対応する GPU 提出 Completion を登録する。Default だけに待機規則を限定して Upload の利用中上書きを許可しない。

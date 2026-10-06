# 公式 ImGui DX12 Backend 接続（M05-05）

Issue: [#81](https://github.com/tochouseito/CueEngine/issues/81)

## 接続と所有

EditorHost は WindowsHost の SwapChain 生成後・Graph 構築前の Callback から `ImGuiManager::initialize_renderer(IBackend&, frameCount)` を呼ぶ。Manager が Editor 内部の DX12ImGuiBackend を一意所有し、公開 API は ImGui / DirectX の具体型を公開しない

Adapter は DX12Backend とその Device、SwapChain 所有の Graphics Queue を借用する。唯一の Graphics Queue は SwapChain が Lease を保持するため、QueuePool から再取得しない。Backend と Context は Adapter の shutdown 完了まで生存させる。全操作は Manager の Owner Thread 上で直列に行う

公式 `ImGui_ImplDX12_InitInfo` に Device、CommandQueue、専用 SRV Heap、Descriptor Callback、CPU 描画枠数、実 BackBuffer の RTV Format を設定する。`NumFramesInFlight` は FrameController / Graph の `maxFramesInFlight` に合わせ、SwapChain の BufferCount とは独立させる。初期対応は既存 SwapChain と同じ RGBA8 UNORM / SampleCount 1。二重接続、SwapChain 未生成、不正な枠数や容量は Result で拒否する

| 所有者 | 資源 |
| --- | --- |
| Engine DX12Backend | Device、Graphics Queue と Fence、Graph / Command Pool、SwapChain |
| Editor DX12ImGuiBackend | UI 専用 DX12DescriptorAllocator、世代 Handle と Native Pair 対応表 |
| 公式 ImGui DX12 Backend | PSO、Root Signature、描画枠ごとの Vertex / Index Upload Buffer、Font / 動的 Texture、Texture転送用 Buffer / Allocator / List / Fence / Event、内部 DXGI Factory |
| ImGui Context | CPU Draw Data、Font Atlas と Texture 更新要求 |

公式 Source は固定版 `v1.92.9b-docking` を変更せず使用する。Engine が作る専用 Heap は `Cue Editor ImGui SRV Heap` と命名する。公式内部の Resource 所有を Engine Pool と二重管理しない

Frame 数は現 FrameController の対応範囲に合わせて 1 / 2 のみ許可する。公式 Init は Context 登録後に C++ 配列を生成するため、この内部生成での bad_alloc は安全な Rollback ができず Fatal 停止とする。通常の入力不備、Heap 生成や DeviceObjects 生成の失敗は Result で返す

## Descriptor と Texture

UI は Renderer の共有 Heap と異なる Shader 可視 CBV/SRV/UAV Heap を使う。容量は `ImGuiManagerConfig::rendererDescriptorCapacity`、既定 64。Allocator は Heap を所有し、対応表は世代付き Handle、CPU / GPU Handle と予約状態を保持する

公式割当 Callback は void で、失敗を公式 Backend へ返せない。記録直前に Draw Data の Texture 更新リストを確認し、WantCreate の必要数を予約する。容量不足では予約を Rollback し、公式 CreateSRV に入る前に Result で失敗する。Callback は予約済み Handle を取り出すだけで、再割当や例外を起こさない。未予約呼出しや不正な返却は内部契約違反として Fatal 停止する

GPU 完了後、返却可能な WantDestroy を先に処理する。その後に新規予約を行うため、満杯でも返却条件を満たした Slot は再利用できる。まだ参照保持期間内の旧 Texture は回収せず、一時的な容量不足を返す。Free Callback は Native Pair を元の世代 Handle へ戻して Allocator に返却する

Font と動的 ImTextureData の WantCreate / WantUpdates / WantDestroy は公式 UpdateTexture に渡す。公式は Graphics Queue へ独自の Command List を直接提出し、独自 Fence で転送完了を待つ。Engine CommandPool はこの提出を所有しない。Adapter は後続の Engine Queue Signal / wait により Native 提出も停止時の完了点に含める

任意の別 Heap の Texture ID は描画前に拒否する。#82 はユーザー指定の Test / TEST だけを表示するため、FinalColor の UI Heap への View 登録は未実装のまま。共有 Context / Atlas や Worker 転送で Texture RefCount を増やす運用は #83 以降の対象とする

## 記録と State

`record_draw_data(FrameGraphContext&)` は直前に確定した CPU Frame を一度だけ記録する。呼出 Pass は対象 Texture の Write / RenderTarget を宣言して `set_render_target()` を先に呼ぶ。Adapter は DX12 Context、Graphics / Recording 状態、同じ Native Device、描画枠範囲、設定済み RTV Format を事前に検証する。Reset、Close、Submit、Graph Resource の Barrier は行わない

Texture 更新後に専用 Heap を Bind し、公式 RenderDrawData を呼ぶ。Texture 更新は先に完了させたため、公式描画中の自動更新を一時的に止めて二重処理を避ける。任意の User DrawCallback は未申告 Resource 利用や例外を避けるため拒否し、公式の Reset / Sampler 切替だけを許可する

DX12FrameGraphContext の外部記録入口は Callback の前後、例外時とも Pipeline / Root / Heap / Target / Viewport / Binding の Cache を失効させる。同じ Pass 内で Engine 描画を続ける場合はすべて再設定する。後続 Pass は新しい Context から始める。Heap を戻すだけでは以前の Root Table は復元されない

記録した List は提出または破棄してから次の UI Frame に進む。Manager の shutdown 前に Graph を停止・破棄し、未提出 List の借用も終える

## 同期と停止

初期版は記録前に Graphics Queue へ新しい完了点を発行して CPU 待機する。公式 Buffer Ring は RenderDrawData 呼出回数、Graph 枠は Runtime Frame 番号で進むため、最小化・取消し後に両者がずれても GPU 利用中の VB/IB を上書きしないようにする。GPU の並列進行を抑える方式であり、性能改善は主張しない。#83 では ImGui の描画枠と提出 Completion を対応させ、全面待機の削減を検討する

停止は Graph の停止・GPU 待機・Pass 破棄 → Adapter の新規 Fence 待機 → 公式 DX12 Shutdown → 専用 Heap 解放 → Win32 Handler / Backend 停止 → Context 破棄 → Engine Backend 停止の順。GPU 完了確認失敗では Context / Heap と下位 Backend を保持し、明示 shutdown を再試行する。Destructor まで完了を証明できない場合は Fatal 停止する

DeviceObjects は初期化時に bool を確認して明示生成し、NewFrame の遅延作成 Assert に依存しない。ただし、公式 Backend の一部 GPU Resource 作成・Map・提出処理は void / Assert / 無通知 return であり、内部のすべての GPU 失敗を回復可能な Result に変換する保証はない。予測できる Descriptor 不足や契約違反を事前に Result 化し、取得できた Device Removed は Fatal として返す

## 検証

`Cue.Editor.ImGuiDX12Backend` は Test 専用 Pass を Graph に注入し、公式描画の実画素を Readback する。UI Heap の後に同じ Context で標準 Renderer 描画へ戻し、Heap / Pipeline 再設定も画素で確認する

容量 1 / 描画 1 枠は Font と User Texture の同時生成を拒否し、未生成 Texture を取り下げて次の Frame で回復する。容量 2 / 描画 2 枠は満杯の旧 Texture を返却して新規 Texture を同じ Slot に作り、変更領域 Upload を実行する。Context の State 失効、同一 Frame 再記録、Owner Thread 拒否、CPU Frame 中断後の GPU 後付け拒否を確認する。Debug Layer の Error / Corruption と、全 Owner 停止後に Device 自身以外の GPU Object が残っていないことを確認する

2026-10-06: `scripts/codex_build.ps1` の既定 Debug Build 成功。`ctest --preset windows-vs2026-debug --output-on-failure` は 35 / 35 成功。公式 UI の Red Pixel と、通常 Renderer を再設定した Blue Pixel を Readback で確認した。Backend は既定の HardwarePreferred を利用し、WARP を強制した検証は行っていない。Leak 検査用 Device を一時保持するため、Test の終了検査中だけ Warning の対話 Break を止め、Error / Corruption と残存 Object は Message 内容で判定する

既定 Editor の [ImGuiPass](ImGuiPass.md) は #82 で接続した。UI はユーザー指定の Test / TEST のみ。Multi-Viewport、Worker 転送、任意 DrawCallback と GPU 失敗注入はこの検証には含めない

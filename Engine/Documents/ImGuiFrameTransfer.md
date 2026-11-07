# ImGui 描画 Snapshot の転送（M05-07）

Issue: [#83](https://github.com/tochouseito/CueEngine/issues/83)

## Frame の進行

Editor の既定設定は CPU 描画枠 2、Worker 有効。UI / Win32 入力 / Context の所有者は MainThread のまま、Update と Render は FrameController の Worker で実行する。`useWorkerThreads = false` と枠数 1 / 2 も同じ転送経路を使う

1. Main Callback が `build_frame()` で UI を構築し、`publish_frame(frame, token)` で公開する
2. FrameController が採用 Frame を Update → Render へ進める
3. WindowsHost の `recordFrame` Scope が `render_frame()` を呼ぶ
4. ImGuiPass がその Frame の Snapshot を記録し、Graph が提出または取消を完了する
5. Graph の発行済 Fence を使用枠と Texture に接続し、Snapshot / Texture Pin を回収した後、WindowsHost が Present する

Snapshot は頂点、Index、Command 配列を所有する。Texture は Native ID に固定し、Viewport の Metadata を複製する。公式 Backend の Viewport Data は停止まで借用する。次の NewFrame が原本を変更しても前の Frame の描画は変わらない。CloneOutput が複製しない任意 Callback UserData は持ち出さず、公式 Reset / Sampler 切替だけを許可する

## Texture と GPU 寿命

Main の公開時に公式 Texture 生成 / 更新要求を処理する。更新対象を旧 Snapshot が参照していれば回収を待つ。待機は Context Lock を解放し、Main の StopToken で取消可能。Worker の失敗では FrameController がこの Token を停止し、最初の Worker Error を主原因として保持する

Texture の `RefCount` は Context / Font Atlas 用のため転送参照数に使わない。Snapshot が参照中の Texture に `QueueUserData` Pin を設定し、最後の Snapshot の回収で解除する。WantDestroy は Pin がある間処理しない。#93 では最後の GPU 利用 Fence を Texture ごとに保持し、更新・破棄のときだけ必要な完了を待つ。Pin 解除だけでは GPU 完了を意味しない

## M06 の同期・容量再利用

#94 では render_frame の開始と回収、公式 ImGui 記録だけを Context Mutex で保護する。同期 Graph Callback 全体では保持しないため、通常 Pass 記録中に Main が次の UI を構築できる。借用中の Snapshot は枠に残し、Main が再利用できない状態を維持する。Texture 更新待機は同じ Pin と停止 Token の規約に従う

#95 では回収した Snapshot と DrawList を描画枠ごとに保管し、Vertex / Index / Command 配列を resize と Copy で更新する。ImVector の代入は容量を捨てるため使用しない。UI が減ったときも確保済み容量を保持し、増加時だけ拡張する。非所有 Texture ID の固定と公式 Callback の許可規則は維持する。transfer_info の snapshotAllocations は Snapshot 本体・DrawList・出力配列の確保、copiedBytes は出力配列の複製量を計数する

公式 Vertex / Index Ring の再利用と Texture 変更前には Graphics Queue の完了待機を維持する。GPU 並列性の最適化や高速化を主張しない。全面待機を減らすには、公式 Ring と実際の提出 Completion の対応付けが必要

## Thread と停止

固定版の GImGui は TLS ではないため、Manager / Win32 / DX12 Adapter 共通の再帰 Mutex で直列化する。begin_frame から end_frame / cancel まで直接 ImGui API の呼出しも排他に含める。Owner 専用 API の WrongThread 判定は Lock 前に行う

RenderThread は固定し、`record_draw_data()` を render_frame の同期 Scope 内からだけ許可する。Present を Scope 外へ置くのは DXGI が MessageThread を待つ場合の相互待ちを避けるため。渡した Graph Callback を保存・非同期実行してはならない

Frame ID は 0 始まりの単調増加、Slot は Frame ID % 描画枠数。満杯、順序違反、二重公開、二重記録を Result で拒否する。Graph の Skip / 失敗 / Callback 例外でも Snapshot を回収する。停止は Runtime Worker Join → Graph GPU 完了 / 破棄 → 未消費 Snapshot 回収 → ImGui DX12 / Win32 / Context → 下位 Backend / Window の順

## 検証

- GPU Test は Main で赤 / 緑の 2 Frame を先に公開し、固定 Render Worker で順番に記録・Readback してそれぞれの画素を確認する
- 停止 Token による Skip、Graph 記録失敗、未消費 Frame の停止回収で Queue Pin を残さない
- EditorHost は Main UI と別 Thread の連続描画、単一 Thread の Frame 対応、初期化 Rollback と UI 失敗伝播を検証する
- FrameController は Main が待機中の Update 失敗で取消通知が届き、主原因が保持されることを検証する

構成別の実行結果と未実施の操作は [Completion Gate](ImGuiCompletionGate.md) に記録する

2026-10-06: Debug Build と全 CTest 36 / 36 成功。取消 Test 追加後の関連 CTest 10 / 10 成功。Main に先行公開した異なる画素の Frame、Worker 記録、Skip / Graph Error / 未消費 Frame の Pin 回収、停止済み Token による Texture 更新取消を確認した

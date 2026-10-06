# ImGuiManager と Win32 / DX12 接続（M05-04 / M05-05）

Issue: [#80](https://github.com/tochouseito/CueEngine/issues/80)、[#81](https://github.com/tochouseito/CueEngine/issues/81)

## 所有と初期化

`Cue.EditorHost` の ImGuiManager が ImGui Context、Font Atlas、Style、Layout 保存先と公式 Win32 Backend を所有する。`create(Window&, ImGuiManagerConfig)` は Result で一意所有権を返し、部分初期化の失敗では Context と入力登録を回収する。Window は非所有参照で、Manager の shutdown 完了まで生存させる

Window 生成後、Renderer Backend 生成前に EditorHost が Manager を生成する。Message Handler の枠を Win32 Backend の初期化より先に確保し、同じ Window への二重生成失敗が既存 Backend の Window Property を書き換えないようにする。公開 Header は ImGui / Win32 の具体型を含めない

create は Font を登録するだけで Atlas を Build しない。GPU Backend は初回 begin_frame より前に接続し、動的 Atlas は公式 NewFrame に構築させる。CPU 単独利用は初回 begin_frame で Legacy Atlas を Build する。Legacy Build 後の GPU 後付けは方式が衝突するため InvalidState で拒否する

旧 CueEngine の Context、Win32 Backend、Font / Style、Frame 進行を Editor 側で管理する構成を採用する。Keyboard Navigation と Docking を有効にし、Multi-Viewport は無効とする。旧 Engine の Font File は現 Repository に未導入のため、ImGui 内蔵 Font を既定 18px で利用する。日本語文字の入力は配送するが、日本語 Glyph の表示は別途 Font 導入が必要。第三者 ImThemes の Style Source は移植せず Dark Style を初期値にする

## Thread と UI Frame

UI 構築と破棄は生成 Thread、転送した Snapshot の記録は固定 RenderThread の同期 Scope 内で行う。異なる Thread は WrongThread、二重 Frame 開始や Callback からの begin / end / shutdown 再入は InvalidState を返す。EditorHost は #83 の [Snapshot 転送](ImGuiFrameTransfer.md) を利用し、Worker 構成を既定とする

通常は `build_frame(editorUiCallback)` を使う。DX12 接続済みなら DX12 NewFrame、その後 Win32 NewFrame → ImGui NewFrame → UI Callback → ImGui Render で CPU Draw Data を確定する。Callback 中は対象 Context を Current にし、終了後は呼出元の Context に戻す。EditorHost は Runtime / FrameController の汎用 Main Callback に接続し、採用 Frame の Update 前に構築 Thread で UI を確定するため、Message Pump だけの step や Close で終了する step では UI Frame を増やさない

Callback の Result 失敗と例外は Error として返し、未完了の Frame を回収する。固定版 `v1.92.9b-docking` の内部 Error Recovery API で Callback 前の Stack を保存し、失敗時に残った Begin / Style 等を回収する。回復中だけ Assert を抑制し、通常の Assert と回復 Log は維持する。これは ImGui の回復可能な Stack の処理であり、Context の破壊や不正な Pointer 操作を復旧する契約ではない。ImGui の Version 更新時には内部 API と回復 Test を再確認する

個別の `begin_frame()` / `end_frame()` も使用できる。開始から終了まで Context を切り替えず、ImGui の正常な Begin / End の組を守る。`shutdown()` は開いた Frame を回収する

`frame_info()` / `EditorHost::ui_frame_info()` は確定済み Frame 数、頂点 / Index 数、Mouse / Keyboard / Text Input の Capture を所有値で返す。#81 の `initialize_renderer()` / `record_draw_data()` は Draw Data を公開せず GPU 記録へ接続する。#82 の [ImGuiPass](ImGuiPass.md) が実 Editor の Test UI を表示する。Render Thread への転送は #83 の Snapshot に接続済み。GPU 接続の詳細は [ImGuiDX12Backend](ImGuiDX12Backend.md) を参照する

## Message と Capture

Platform.Windows が Window ごとに一つの外部 Handler を保持し、世代付き Token で解除する。登録・解除は Window Owner Thread に限定し、Message 処理中の登録・解除・Window destroy を拒否する。Callback の例外は Message Pump の Result へ伝播する

Manager の Handler は Window に対応する Context を Current にして公式 Win32 WndProcHandler を呼ぶ。Mouse、Wheel、Keyboard、Unicode WM_CHAR、Focus を配送する。Handler の処理済み返却でも Close / Size / Destroy / NCDESTROY の必須処理は Platform が実行する

Capture Flag の真偽にかかわらず入力を ImGui に配送する。Gameplay 側は Capture を参照して自身の処理を抑制する。公式の Trickle Queue はイベントを順序どおり複数 Frame に分配する場合があり、すべてが最初の NewFrame に取り込まれるとは限らない

## 設定保存と停止

`settingsFile` は UTF-8 Path、既定 `out/editor/imgui.ini`。相対 Path は実行時 Working Directory を基準とし、空なら File 保存を無効にする。ImGui の IniFilename は null にして自動 File I/O を止め、Manager が Memory API から読込・保存する。初回 File 不在は許容し、既存 File の読込失敗は初期化失敗にする

保存要求と明示 `save_settings()`、shutdown 時に一時 File `<path>.tmp` へ書き、成功後に rename して置き換える。書込みや置換の失敗は Result で返す。同じ保存先を複数 Process / Manager から同時更新する運用は対象外とする

停止順は Runtime / UI Callback 停止 → Graph の GPU 完了と Pass 破棄 → ImGui DX12 の GPU 完了確認と資源解放 → Message Handler 解除 → 開いた UI Frame 回収 → Layout 保存 → Win32 Backend 停止 → Context 破棄 → Renderer Backend / Window 破棄。GPU 完了確認や Handler の解除失敗では Context と下位 Owner を保持し、明示 shutdown を再試行できる。Layout 保存だけの失敗では解除済みの Context を回収し、失敗を返す。Destructor まで借用解除に失敗する場合は terminate し、不正な参照を残す破棄を続けない

## 検証対象

- `Cue.Platform.Windows.MessageHandler`: 登録、世代付き解除、Thread / 再入拒否、UI 処理済み時の Close / Size / Destroy、例外と Window 破棄後の接続解除
- `Cue.Editor.ImGuiManager`: Context の復元、Win32 入力 / Unicode / Focus、CPU Draw Data、Capture、二重生成、Callback 失敗時の Stack 回復と次の Frame、保存と再読込、読込 / 保存失敗時の回収
- `Cue.EditorHost.Lifecycle`: 採用 Main Frame と UI Frame の対応、初期化 Rollback、UI 失敗伝播、Thread / 再入拒否、停止後の操作拒否
- `Cue.WindowsHost.Lifecycle`: 上位停止 Callback 失敗時の Window 保持と再試行

Layout の Test は各 Process が新規生成した一時 Directory を使い、利用者の Layout を変更しない。`Cue.Editor.ImGuiDX12Backend` は Test Pass から公式 GPU 描画、動的 Texture と不足回復を検証する。Worker Data 転送は [Frame 転送](ImGuiFrameTransfer.md)、実 UI 操作と構成別の結果は [Completion Gate](ImGuiCompletionGate.md) を参照する。Multi-Viewport は対象外とする

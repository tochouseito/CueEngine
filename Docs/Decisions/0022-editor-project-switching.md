# ADR-0022: Editor の Project 再選択

- Status: Accepted
- Date: 2026-11-05
- 対象: 同じ Engine Version の Project を、同じ Editor Process で開き直す

## 操作

Hub は Project の作成・管理・Editor 起動を担当する。
Editor は `File > Open Project...` と `Ctrl+Shift+O` で編集対象を切り替える。
Folder Dialog で選択した Directory の直下に、一つの通常 `.cueproj` File が必要。
未検出・複数候補・Link はエラーとして表示し、列挙順で勝手に選択しない。

Editor の `--project` は任意とする。未指定なら Project 未選択画面で起動し、File Menu の操作を待つ。
未選択時は Project / Scene / SceneDocument / Preview World を生成せず、UI と表示基盤だけを持つ。
描画 Frame は Camera / Object のない Snapshot を渡し、選択後に Document と World を準備する。
明示した不正な引数は従来どおり起動失敗として扱う。
Visual Studio の既定起動引数は全構成で空とし、構成変更だけで別の Project を開かない。
Build の既定 `CUE_GAME_PROJECT` も空とし、Preset で以前の Sample 選択 Cache を解除する。
Hub から渡された明示的な `--project` はそのまま利用できる。
Hub に Engine 開発専用機能を追加しない。

## 保存と失敗

切替先の Project・Engine Version・開始 Scene・Preview World・Script 接続の候補を準備してから、
現在の未保存編集を ImGui の保存 / 破棄 / 取消で確認する。
保存取消・保存失敗・候補の検証失敗では、現在の Project と Document を維持する。
開始 Scene がない Project は、新規 Scene を持つ編集状態として開く。
同じ Project の再選択でも File を再読込する。

Play / Metadata 取得 / Build / 製品アプリ / IDE 操作の稼働中は切替を拒否する。
別 Engine Version が指定されている場合は現在の Editor で開かず、Hub で指定版を選ぶ対象とする。

## 所有権と公開境界

Project の一意所有先は移動しても住所を変えない。Script Controller はその安定した値を借用する。
候補は保存確認中も Host が所有し、UI からは FileSystem / World を直接操作しない。
UI の Action 列と Document の借用を終えた次の `step()` 境界で公開する。
旧 Controller / Play 接続を先に破棄し、Project / Document / Preview / Script 接続を切り替える。

Window、ImGui、Renderer、Frame ID は継続する。Render Worker は Project / World を借用せず、
既に公開された所有 Snapshot を完了する。Project 切替では GPU Mesh や Graph を破棄せず、
SceneRenderer の Frame ID や未完了 Snapshot をリセットしない。
これにより Visual Studio のデバッガー接続を維持する。

Build Service は静止時に `reset_project()` で旧 Project、成功成果物、ログと起動可否を無効化する。
Tool、Engine Lease、FileSystem は継続所有する。旧成功ゲームを新 Project の Run として起動しない。
この操作を提供しない独自 Service は切替を拒否し、旧 Document を維持する。

## 検証

引数なし起動、不正引数拒否、未選択 UI、Folder 候補の数と種類を確認する。
Single / Worker の WARP Test で A → B → A と同じ Project の再読込、保存 / 破棄 / 取消、
読込失敗・不正 Scene・Version 不一致・Busy・別 Thread・旧 Build 成果物の失効を確認する。

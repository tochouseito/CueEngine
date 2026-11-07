# ADR-0012: .cuescene v1 と World への実体化

- Status: Accepted（起動 Scene と Transform の最小経路）
- Date: 2026-10-30
- 関連: [Project](0009-project-loading.md)、[Math](0010-math-values-and-transform.md)、[World](0011-runtime-world-and-scene-instances.md)

## 境界

- `Cue.Scene` が永続データの所有・検証・Parse / Serialize・抽象 FileSystem からの読込を担当する。
  公開型は Foundation / Math の値だけを使い、Runtime / ECS / Editor / Renderer へ依存しない。
- `Cue.SceneRuntime` が Scene 定義と World を接続する。Scene / Runtime のどちらからも Bridge へ逆依存しない。
- Editor は保存用 Scene と、Main 所有の編集表示用 Preview World を別に所有する。
  Play 用 World、Update Worker への実行接続、描画 Snapshot は後続で扱う。

拡張子はユーザー指定の `.cuescene`。第三者 Parser は追加せず、小さい Version 付きの UTF-8 テキストとする。
これは Scene の最小 Schema であり、汎用の INI / JSON Parser として他の形式を解釈しない。

## 保存形式

```ini
formatVersion=1
sceneId=8d94c2b1-9b6a-435c-ade7-c01f473c3951
name=RotateCube

[object 9a00d731-7759-424c-9ade-fd086d624551]
name=Cube
active=true
transform.position=0,0,0
transform.rotation=0,0,0,1
transform.scale=1,1,1
```

- 最大 4 MiB、最大 4096 Object。BOM は先頭で読み飛ばし、LF / CRLF、空行、行頭の空白を除いた `#` Comment を許可する。
- `key=value` と `[object UUID]`。区切り周辺の Space / Tab / CR を除去する。
  引用符、Escape、行末 Comment は解釈しない。値内の `=` / `#` は表示名の文字として保存できる。
- Header の `formatVersion` / `sceneId` / `name` は必須。Version は 1 のみ。
- Scene / Object ID は非零の小文字 UUID 表記。Scene 移動・改名で変更しない。
  Project / Scene で共通の検証を `Foundation/Uuid.h` に置き、Version / Variant の制約は追加しない。
- Scene / Object 名は非空 UTF-8、最大 256 Byte、前後空白・NUL・ASCII 制御文字なし。
- Object ID は同一 Scene 内で一意。名前の重複は許可し、Runtime の名前検索では曖昧な一致を拒否する。
- `active` は任意で、既定 true。指定時は `true` / `false` のみ。
- Transform は任意。含める場合は position 3 成分、rotation 4 成分、scale 3 成分をすべて必須にする。
- 回転は Quaternion の x / y / z / w。Euler を保存の正本としない。
- 数値は comma 区切りの float32。Locale 非依存で全体を解釈し、余剰成分、非有限値、範囲外、無効 Quaternion を拒否する。
- 未知 Key / Section / Component、重複 Key、Component の部分欠損を拒否する。
  将来の Mesh / Script / 親子関係を無言で捨てず、対応 Schema と Version / 移行方針を設計して追加する。

`Scene::create` も Parse と同じ ID / 名前 / Component 契約を検証する。成功した Scene は不変の所有値。
`serialize` は最大有効桁数で float を出力し、Parse で値を復元できる文字列を返す。File へは書き込まない。
保存する利用側は既存 `IFileSystem::replace_file` を呼び、公開前の旧 File 保全と Durability の結果を扱う。

## Project の起動参照

`.cueproj` v1 に任意 Key `startupScene` を追加する。

```ini
startupScene=Scenes/RotateCube.cuescene
```

- Content Root 基準の相対 Path。絶対 / Drive / ADS / 正規化後の Root 外を拒否する。
- `.cuescene` 拡張子と Regular File の存在を Project 読込時に検証する。末尾要素の Link / Directory を拒否する。
- Scene 本文の形式検証は Scene Module が担当する。
- 未指定なら従来の Project のみの起動を継続できる。既存六項目の `.cueproj` を拒否しない。
- 旧 Binary は未知 Key を拒否する場合がある。新しい任意 Key を古い Binary が無視する保証はしない。
- 既存 Project と同様、親の Symlink / Junction を含む物理的な Root 閉じ込めは保証しない。

## World への実体化

`instantiate_scene(const Scene&, World&)` は保存用定義を呼出中だけ借用し、新しい SceneInstanceHandle を返す。
World は Owner Thread の通常操作または defer 内で利用する。

1. 全データは不変 Scene の生成時に検証済み。必要な Component 型の容量を Instance 作成前に確認する。
2. Scene ID で新しい Instance を作り、Object ID / 名前から Entity を生成する。
3. 既知 Component と Active を復元し、全 Object が完成したときだけ Handle を返す。
4. Transform 行列は World の次の初期化・更新境界で同期する。

同じ Scene を再実体化しても別の Entity 群を作り、ID の対応は各 Instance 内に閉じる。
Scene データの寿命は実体化後の Entity / Component に影響しない。Scene / Entity Handle を File に保存しない。

入力の不正や Component 容量不足は World 変更前に拒否する。途中の回復可能な失敗では作りかけ Instance を回収する。
構造操作の確保失敗などで World が終端失敗となった場合は、World 自体を停止・破棄する。
**任意の OOM / Callback 失敗に対して既存 World の継続利用を保証する Transaction ではない**。
起動時や Scene 置換は候補 World 全体を新規生成し、成功後に公開することで旧実体と分離できる。

## Editor 起動と終了

Main は Project と起動 Scene の File / 形式検証を Window / Renderer / Worker 生成前に完了する。
`EditorHostConfig::initialScene` が所有値を渡し、EditorHost は Owner Thread 上で候補 World を実体化・初期化してから表示基盤を起動する。
Scene / World の生成失敗は候補を回収し、部分実体を公開しない。

- Preview World は Main Thread 専用。ゲーム System の更新や Script 実行は今回登録しない。
- 既定 Test UI は Project 名と Scene 名・定義 Object 数を表示する。Cube の Mesh 描画はまだ行わない。
- 初期化の試行済み・停止済み・再入を Host 側でも検証し、二度目の呼出しで World を作り直さない。
- 表示基盤の初期化失敗では Preview World を回収する。
- 通常停止は Worker / Pass / GPU の停止が成功してから World を停止・破棄する。
  下位停止に再試行が必要な間は World を保持する。
- `preview_world` / `preview_instance` は Owner Thread へだけ借用を返す。保存用 Scene は Host の破棄まで保持する。

## 検証範囲

Parse / Serialize の往復、UTF-8 / UUID / 重複 / 数値・上限、不正 Component、実 File の日本語 Path と I/O 失敗、
同じ Scene の二回実体化・独立更新・型容量拒否・別 Thread 拒否を検証する。
Project の任意参照と旧形式、Editor の Preview 生成・Backend 失敗回収、起動 Process の不正 Scene 拒否も確認する。
Scene Editor の編集 UI、Undo / Redo、Save ボタン、未知 Component の保持、Schema Migration、Script / 描画接続は後続。

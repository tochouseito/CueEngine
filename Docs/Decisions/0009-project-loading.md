# ADR-0009: Project 設定と Editor 起動時の読み込み

- Status: Accepted（M11-01 の最小実装。後続機能の設定は各 Issue で追加する）
- Date: 2026-10-28
- Issue: [#146](https://github.com/tochouseito/CueEngine/issues/146)
- 関連: [Architecture 境界](0001-architecture-boundaries.md)、[Build 正本](0002-build-system-source-of-truth.md)

## 目的

Editor は `--project <path>` で受け取った Project を開く。最初の Sample は Repository Root の
`Samples/RotateCube/RotateCube.cueproj`。Hub、Project 管理アプリ、ショートカット生成は後続とする。

## 設定形式

`.cueproj` v1 は UTF-8 の `key=value` 形式。既存の設定に近い小さい形式を使い、新しい第三者 Parser は導入しない。
最大 64 KiB、LF / CRLF、先頭の UTF-8 BOM を許可する。空行と、最初の非空白文字が `#` の行は読み飛ばす。
Key と値の前後の Space / Tab / CR は除去する。値の引用符・Escape・行末 Comment は解釈しない。
Key は大文字小文字を区別し、全項目を一度ずつ指定する。未知 Key、重複、空値、未対応 Version は拒否する。

| Key | 契約 |
| --- | --- |
| `formatVersion` | 現在は `1` のみ |
| `projectId` | 小文字の UUID 形式 `8-4-4-4-12`、全零は禁止。Project の移動・改名で変更しない |
| `name` | 空でない UTF-8 の表示名、最大 256 Byte。NUL / 制御文字を拒否し、Path 構文から独立させる |
| `applicationName` | 最大 128 Byte、ASCII 英数字・`_`・`-`。生成アプリ名として使う識別名 |
| `sourceRoot` | Project Root 内の相対 Directory。既存 Directory が必要 |
| `contentRoot` | Project Root 内の相対 Directory。既存 Directory が必要 |

起動 Scene は [ADR-0012](0012-scene-format-and-instantiation.md) により任意 `startupScene` を追加した。
Content Root 内の相対 `.cuescene` を指定し、従来の六項目のみの設定も受け付ける。
Script 登録、Runtime / Presentation / Build 設定は対応 Issue で追加する。
互換性を変更するときは形式 Version と移行方針を同時に設計する。PC 固有の Engine / Compiler の絶対 Path、
Editor の選択・Layout、EntityHandle、MeshHandle を永続設定へ入れない。

## 責務と所有権

- `Cue.Project` が形式検証と配置解決を担当する。公開 Header は Foundation の値型だけを含み、FileSystem は前方宣言する。
- `Project::load` は抽象 `IFileSystem` を呼出中だけ借用する。実装依存は `Cue.Platform` への Private 依存とする。
- 成功した `Project` は設定と絶対配置を値で所有する。変更 API はなく、FileSystem / File / 入力文字列への参照を保持しない。
- Editor の起動入口で全検証を終え、`EditorHostConfig::project` から EditorHost へ移譲する。
- EditorHost が下位 Host、Callback、UI の停止・破棄まで所有する。Project は World / GPU Resource を所有しない。
- EditorHost Library の既存利用と Test は Project 未指定でも利用可能。製品 Editor の起動入口は指定を必須とする。
- Runtime / Renderer / WindowsHost は Project の具体データを保持せず、従来の設定値と Callback を利用する。

## Path 契約

1. Windows の起動入口が `CommandLineToArgvW` で Quote と空白を含む引数を分割し、UTF-8 へ変換する。
2. 共通の引数検証は、実行 File 名を除く `--project <path>` 一組だけを受け付ける。
3. `IFileSystem::absolute_path` が呼出時の CWD を基準に Project File を一度だけ絶対化する。CWD は変更しない。
4. 設定 File の親を Project Root とし、内部配置はこの Root へ結合する。以後の File 操作へ絶対 Path を渡す。
5. 内部配置の絶対 Path、Drive / ADS 指定、正規化後の先頭 `..` を拒否する。Source / Content 自体が Link の場合も拒否する。

字句的な配置検証であり、親 Directory の Symlink / Junction を含む物理的な Root 閉じ込めは保証しない。
信頼できない Project の実行・Package / File 編集を扱う段階で別途設計する。現在は設定の読み取りだけを行う。

## 起動設定と失敗

CMake の `VS_DEBUGGER_COMMAND_ARGUMENTS` に RotateCube を開く固定引数を設定する。
Editor の実行 File に既定 Project を埋め込まない。将来の Hub とショートカットも同じ引数契約を利用する。

引数・File・形式・配置の検証に失敗した場合は Window / Renderer / Worker を作らず、部分 Project を返さない。
既存 FileSystem の Error は処理名と Native Code を保持する。起動入口は Logger がない段階でも MessageBox で理由を示し、
終了 Code 1 を返す。予期しない C++ 例外は従来の最上位境界で終了 Code 2 にする。

## 検証

- 公開 Header が Windows / Editor / Renderer の Header を要求しないこと
- Unicode / 空白、絶対 / 相対 Path、別 CWD、Project 移動後の ID 維持、所有値の独立性
- 必須項目欠損、重複、未知 Key、Version、NUL / 不正 UTF-8、Root 外参照、Directory 欠損、File 上限と I/O 失敗
- 実 Editor の起動、Project 名、連続描画、Resize・最小化・復帰、正常 Close
- 既定 Debug Build と関連 CTest

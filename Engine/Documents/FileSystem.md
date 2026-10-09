# File 操作基盤

- Issue: [M08-01 #108](https://github.com/tochouseito/CueEngine/issues/108)
- 基準: 旧 CueEngine develop の Core/IO/Path、IFile、IFileSystem と PAL/win/IO

## 配置と所有権

| 層 | 責務 |
| --- | --- |
| Foundation/Path | UTF-8 検証、区切りの統一、字句的な正規化、結合、親、File 名と拡張子 |
| Platform/FileSystem | IFile / IFileSystem、Open / File 情報、共通の完全転送 Helper |
| Platform/Windows/WindowsFileSystem | Win32 の同期 File Handle、Known Folder、Directory 操作、保存先の置換 |
| WindowsHost/StoragePaths | 実行 Mode、製品 Identity、保存先 Override の解決と Directory 作成 |
| WindowsHost | FileSystem と解決済み Path の所有、利用者への非所有依存の注入 |

WindowsHostConfig.fileSystem が未指定なら Windows 実装を生成する。指定した実装も Host が一意所有する。
初期化時に保存先を解決・作成してから Window / UI / Backend を作る。取得・作成に失敗した場合は起動を中断し、CWD へ暗黙に Fallback しない。

EditorHost は WindowsHost の初期化 Callback で FileSystem と保存 Root を借用する。
Backend は PipelineManager / DXC へ同じ FileSystem を渡す。停止順は Worker → Graph / GPU → UI → Backend → Window → FileSystem。
WindowsHost::file_system と storage_paths の取得は構築 Thread に限り、初期化 Callback から停止完了まで利用できる。

Manager / Backend を単独生成する既存利用経路では、FileSystem 未指定時に内部で Windows 実装を一意所有する。
明示的に指定する場合は借用であり、生成した Manager / Backend より長く生存させる。
可変 Global な FileSystem や Current Directory Service は設けない。

既存 Host / ImGui の内部 State は周辺の記録形式に合わせ、公開 Member の lowerCamelCase を維持する。
無関係な State Member / 関数の一括整形は行わない。

## Path の契約

- 区切りは `/`。空の既定値は未指定を表し、`Path::create` の空入力は Error とする
- 厳密な UTF-8、埋め込み NUL、Drive 相対 Path、Device Namespace、壊れた UNC Root を検証する
- `.` / `..` は字句的に整理する。相対 Root より上の `..` は残し、絶対 Root より上への移動は拒否する
- 絶対の右辺を join した場合は右辺で置換する。Symlink を解決せず、Project Root 内への閉じ込め機構ではない
- Windows 実装では Device 予約名、ADS、Wild Card、末尾 Dot / Space、制御文字を通常 File Path として拒否する
- Windows の File 操作は検証後に絶対 Native Path と `\\?\` 表現へ変換し、長い Path を扱う
- 相対 File Path は直接利用者の CWD が基準となる。Host の保存先は必ず絶対 Path に解決する

Path の文字列処理は FileSystem へ問い合わせない。Foundation の既存値型と同様、Allocation 失敗は例外として伝播する。

## File 操作と Error

IFile は Native Handle を一意所有する。操作と破棄は同一 Object ごとに外部で直列化する。
WindowsFileSystem の独立した操作は複数 Thread で同時に呼べるが、Process の CWD を並行変更しない。

- `read` / `write` は部分転送数を返す。read の成功値 0 は EOF。失敗時は Buffer や Cursor が部分変更され得る
- `seek` は新しい位置を返す。負の位置や不正な Origin は失敗する
- `close` 成功後の再 Close は成功し、他の File 操作は InvalidState。Destructor は未回収 Handle の最後の回収を行う
- Open 設定には Access、Creation、Read / Write / Delete Share を明示する。Read 専用の破壊的 Creation は拒否する
- `stat` の未存在は空の成功値、権限・共有・不正入力などは Error。Reparse Point は Link として報告する
- `remove` は単一 File / Link / 空 Directory のみ。再帰削除を API に含めない
- `rename` は既存保存先を上書きせず、別 Volume への Copy / Delete Fallback をしない
- `copy_file` / `write_all` は移動先を直接変更するため、失敗時の旧内容保全を保証しない
- `read_all` は既定 64 MiB、ImGui 設定は 4 MiB が上限。部分 Read を反復し、終端前の縮小・追加 Byte・最後の Size 変化を検出する
- 同じ Size の外部書換えを含む厳密な File Snapshot は保証しない。既定の Windows Open は Write Share を許可しない
- `write_all` は完全転送、進捗、Flush、Close を確認する

OS 失敗は ErrorCategory::PlatformFailure と処理名 / Native Code へ変換する。
FileSystem Factory、read_all、保存先 Resolver、既存 Manager の入口では Allocation 失敗も Result に変換する。
それ以外の値構築の Allocation 失敗は例外として伝播し、RAII により Handle と一時 File を回収する。
存在確認と後続操作には競合余地があり、後続操作も Result を必ず確認する。

## 保存先の置換

`replace_file` は次の手順で実行する。親 Directory の作成は利用側の責務とする。

1. 同じ親 Directory に GUID 名の一時 File を CreateNew で新規生成する
2. 全内容を書き、Flush / Close を確認する
3. MoveFileExW の REPLACE_EXISTING / WRITE_THROUGH で一時 File を公開する

Cross Volume Copy は許可しない。公開前の失敗では旧保存先を保持し、一時 File を回収する。
共有違反などの公開失敗では、旧 File を削除して再試行する処理を行わない。File の使用者を停止した後で呼出側が再試行できる。
通常の Error 経路では、一時 File / Close の回収失敗も一次 Error の診断へ追加する。
Exception 中の回収失敗は再帰的な File Logging を使わず、Platform の緊急診断へ出力する。

成功値 FileSaveOutcome は **公開済み** を表す。Windows 実装では Directory Entry の電源断耐性を証明できないため、isDurabilityConfirmed は false とする。
公開後の Durability 未確認を公開前の失敗と混同しない。ImGui は公開成功で WantSaveIniSettings を解除する。
File / Directory / Remote FS の性質を超えた原子性・電源断耐性は保証しない。[MoveFileExW の契約](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)

置換では保存先の ACL / File Identity の維持を保証しない。複数 Writer 間の Revision 競合検出は利用側の別契約で扱う。

## 保存先の選択

StorageMode は Debug / Development / Release の Build 構成から独立している。
Host の既定は Product。Repository の CueWindowsHost / CueEditorHost 実行入口は、全構成で Development と CMake の RepositoryRoot を明示する。
将来の製品 Host は Product を指定し、配布対象へ RepositoryRoot を引き継がない。

| Mode | データ Root | logs | Shader Cache | PSO Cache |
| --- | --- | --- | --- | --- |
| Development | `<RepositoryRoot>/out` | `logs` | `cache/shaders` | `cache/pso` |
| Product | `%LOCALAPPDATA%/<会社名>/<アプリ名>` | `logs` | `cache/shaders` | `cache/pso` |
| Portable | 実行 File の配置 Directory | `logs` | `cache/shaders` | `cache/pso` |

Windows の Product Root は SHGetKnownFolderPath(FOLDERID_LocalAppData) で取得する。
会社名 / アプリ名を単一 Directory 要素として検証し、製品別に領域を分ける。
StoragePathsConfig で dataRoot / logs / shaderCache / psoCache の絶対 Override を指定できる。
利用者へは解決済みの Path を渡し、Logger / Cache 内で Repository や実行 File を推測しない。

```cpp
cue::WindowsHostConfig config;
config.storage.mode = cue::StorageMode::Product;
config.storage.companyName = "ExampleCompany";
config.storage.applicationName = "ExampleGame";
// ポータブル配布では config.storage.mode = cue::StorageMode::Portable とする
```

Logger 本体と Shader / PSO Cache は未実装であり、共通 File API と保存先だけを提供する。
Shader Cache は #104、PSO Cache は #107 で接続する。

## 既存利用者の移行

ImGui の既定設定名は `editor/imgui.ini`。EditorHost が相対名を保存 Root へ結合し、明示した絶対名は優先する。空なら読込み・保存を行わない。
Repository の Editor は従来と同じ `<RepositoryRoot>/out/editor/imgui.ini` を使い、CWD に依存しなくなる。
読込みは Context 排他の取得前、保存は Lock 内で文字列を複製した後の排他外で実行する。
保存成功後だけ Context 排他を取り直し、WantSaveIniSettings を解除する。

DXC の実行 File 配置取得、配置済み DLL / Shader Directory の問い合わせは同じ FileSystem を使う。
本体 Shader は read_all の所有メモリを DxcBuffer として渡す。
Custom IncludeHandler も FileSystem 経由で取得し、CreateBlob により所有 Blob へ複製する。
Include 候補は DXC の Source / -I 解決に従い、入れ子 Include、UTF-8 Path、File I/O の失敗を診断へ反映する。
Compiler 内で CreateDefaultIncludeHandler / LoadFile を利用しない。

対象の Engine/Source に std::filesystem / fstream 操作を残さない。
既存 ImGui Test の std::filesystem Fixture は独立した検証用であり、製品コードの依存にはならない。

## 検証

- Cue.Foundation.Path: UTF-8、Root、UNC、結合、親、拡張子、NUL / 不正入力
- Cue.Platform.PublicHeaders: Windows SDK なしの File API 公開 Header の Compile
- Cue.Platform.Windows.FileSystem: 日本語 / 長い Path、転送、EOF、Seek、Flush、Close、予約名、共有違反
- 同 Test: 置換失敗時の旧内容保全、一時 File 回収、再試行、Directory 作成・列挙、Copy / Rename / Remove
- 同 Test: Scripted IFile の部分 Read / Write、上限、Size 変化、進捗なし、Flush / Close 失敗、Owner 回収
- 同 Test: 保存先の 3 Mode、Override、CWD 非依存、取得・作成失敗、Host の初期化 Rollback
- Cue.Renderer.DX12.PipelineManager: 実 DXC の本体 / 入れ子 Include、日本語 Path、FileSystem の失敗注入と再試行
- Cue.Editor.ImGuiManager: 設定の公開・再読込み、FileSystem の保存 / 読込み失敗、旧保存内容保全
- 既存 Host / Editor / DX12 Tests: 起動・描画・Resize・最小化・Close の回帰

権限に起因する Error 伝達は代替 FileSystem のアクセス拒否と Native の共有違反で確認する。
OS ACL を変更する実権限拒否、Remote Share、電源断、Directory / File の競合削除、Allocation 障害、Native Close 失敗の実機再現は実施対象外。
Fault Injection は API の失敗処理を確認するものであり、これらの OS / Hardware 条件を実機検証済みとは扱わない。

### 2026-10-09 の検証結果

`pwsh -NoProfile -File scripts/codex_build.ps1` と構成別指定で Build し、各 CTest Preset を実行した。

| 構成 | Build | CTest | 全 Test 経過時間 |
| --- | --- | --- | --- |
| Debug x64 | 成功 | 41 / 41 | 29.87 s |
| Development x64 | 成功 | 41 / 41 | 23.71 s |
| Release x64 | 成功 | 41 / 41 | 25.66 s |

初回のサンドボックス内 Configure は MSVC を検出できなかったため、通常の Windows 環境で同じ Build Script を実行した。
Raw Log は `out/validation/M08-FileSystem/{debug,development,release}-{build,tests}.log` に保存した。
NuGet restore は実行していない。生成 Build File / Log は Git 管理しない。

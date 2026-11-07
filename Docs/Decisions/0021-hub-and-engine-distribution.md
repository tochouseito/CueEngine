# ADR-0021: Hub、Project 作成、Engine 配布と更新

- Status: Accepted（2026-11-04 のユーザー指定に基づく初期契約）
- 対象: Windows / x64、Hub は C# / .NET 10 / WPF
- ゲームアプリのオンライン更新、公開リファレンスサイト、MCP は対象外

## バージョンと公開

Engine の正本は `Engine/Version.json`、Hub は `Tools/CueHub/Version.json`。
初期値はそれぞれ `0.1.0-preview.1`。Hub と Engine は独立して更新する。
Git Tag は `engine-v<version>` と `hub-v<version>` を使い、Tag と定義が一致しない公開を拒否する。
Preview を含む SemVer は GitHub の Pre-release に対応させる。
GitHub Releases には Engine と Hub の別パッケージ、Manifest、検証情報を添付する。
Hub が列挙するのは公開済み Engine Release のみ。正式版と Preview を合計し、公開日時順で最新 10 件とする。
Draft、Hub Release、必要な配布物がない Release を件数に含めない。
それ以前は GitHub へのリンクで案内し、ローカルパッケージの導入・既存導入版の利用を維持する。

## Engine SDK の初期配置

現在のゲーム Build は Engine の Source / CMake 定義を利用するため、初期配布は Source-backed SDK とする。
Root CMake、CMakePresets、cmake、Engine/Source、Shader、必要な ThirdParty の Header / Library / Notice を含む。
Microsoft の Compiler / Windows SDK / Visual Studio 自体は含めない。利用者の導入を検出する。
配布する Editor / PlayHost / ProjectTool は Development 構成とし、Microsoft Debug CRT を含めない。
SDK の Source から利用者が自身の Toolchain で Debug Build することと、Debug CRT の公開配布を分ける。

```text
<EngineRoot>/
  engine.cueengine
  Engine/Version.json
  Engine/Source/...
  Engine/Shader/...
  CMakeLists.txt
  CMakePresets.json
  cmake/...
  ThirdParty/...
  Templates/...
  Bin/Editor/CueEditorHost.exe
  Bin/Editor/CuePlayHost.exe
  Bin/Editor/VisualStudioBridge.exe
  Bin/Editor/EngineResources/Shader/...
  Bin/Tools/CueProjectTool.exe
  Licenses/...
```

`engine.cueengine` は UTF-8 の `key=value`。v1 の既知 Key は次のとおり。

| Key | 値 |
| --- | --- |
| formatVersion | 1 |
| product | engine |
| version | SemVer |
| buildId | Build の所有識別値。公開時は元 Commit SHA を含む |
| platform | Windows |
| architecture | x64 |
| configuration | Development |
| sdkAbiVersion | 1 |
| projectFormatMin | 1 |
| projectFormatMax | 2 |
| editor | Bin/Editor/CueEditorHost.exe |
| projectTool | Bin/Tools/CueProjectTool.exe |
| useLeaseId | ASCII の `engine-<version>-windows-x64` |

Manifest 内 Path は Engine Root 内の相対 Path とし、Link / Traversal を拒否する。
配布版は実行 File から Engine Root を探索し、生成元 Repository の絶対 Path へ fallback しない。
開発版は従来の checkout を利用できる。配布版の Log / Cache は既存 Product Storage を使う。

## Project 作成と指定 Engine

`.cueproj` v1 の読み取りを維持する。v2 は `engineVersion` を必須で追加し、移植可能な SemVer だけを保存する。
Engine の絶対 Path、ユーザー設定、インストール状態は Project に書かない。
Hub は登録した Engine の Version を照合して起動し、指定版がない場合に別版へ自動置換しない。
開発 checkout はローカル登録とし、Hub の修復 / 削除 / 自動更新の対象にしない。
一覧への登録は Project の記述内容を検証するだけとし、指定 Engine が未導入でも許可する。
開く / 作成 / Build するときは指定版を照合し、未導入の場合は導入案内を表示する。
初期の Hub はメタデータ読み取りに ProjectTool を使うため、登録には少なくとも一つの SDK が必要。

`Cue.ProjectTools` は抽象 FileSystem と UUID 生成 Callback を借用し、呼出中に Project を作る。
`CueProjectTool.exe` の v1 CLI は次のとおり。

```text
CueProjectTool.exe validate --project <absolute .cueproj>
CueProjectTool.exe create --directory <new absolute directory> --name <display name>
  --application-name <identifier> --template Empty3D|RotateCube --engine-version <version>
```

stdout は一つの JSON Object（`schemaVersion=1`, `success`, `projectFile`, `projectId`, `name`,
`engineVersion`, `message`）を返す。成功 Exit 0、入力 / 運用失敗 Exit 1、予期しない失敗 Exit 2。
名前を Shell の Command として扱わず、Hub は引数を個別に渡す。
作成は独立した正規 Template を利用し、Samples の編集内容をコピーしない。
同じ親 Directory 内の所有 Staging で生成・検証し、上書き不可 Rename で完成 Project を公開する。
失敗時に回収するのは自分で生成した File だけとし、不明な File が混入した Staging は残して診断する。

## インストールと利用中排他

Hub の Engine 導入は Staging で完結し、署名 / Hash / 内容を検証した後だけ完成場所へ公開する。
上書き更新を行わず、Engine は Version ごとに共存する。
設定・Project・Cache と Engine 本体の寿命を分離する。
Repair / Uninstall は対象が Hub 管理導入版であると確認し、専用 Root 外や Reparse Point を拒否する。

Editor、PlayHost、Build Service は配布版 Engine の利用中 Lease を保持する。
共通 Path は `%LOCALAPPDATA%/CueEngine/Locks/<useLeaseId>.lock`。
利用者は Read/Write access と Read/Write sharing で開き、破壊的操作は同じ File を FileShare.None で保持する。
Lease は Engine 内ではなくユーザー領域に置き、共有・排他を取得できない場合は操作を開始しない。
保持期間は Editor / PlayHost の全寿命と Build 子孫の終了まで。

## 配布 Manifest と信頼

Engine の Release Asset は `engine-release.json`、その署名、`CueEngine-<version>-windows-x64.zip`。
Release Manifest は schemaVersion=1、product=engine、version、tag、platform、architecture、
configuration、buildId、packageFileName、packageSha256、packageSize、expandedSize を持つ。
署名は Manifest の元 Byte 列を ECDSA P-256 / SHA-256 で検証する。公開鍵は Hub の信頼設定に固定し、
ネットワーク由来のパッケージから信頼鍵を自己申告させない。
開発 Test は一時鍵・ローカル Feed を使い、本番の秘密鍵や Token を Source / Log に残さない。
公開鍵 / 署名用 Secret が未設定ならオンライン配布を成功扱いせず、理由を示す。
HTTPS、サイズ上限、Hash、Zip Slip / 重複 Path / Link / 展開容量の検証を行う。

## Hub の Build と自己更新

Hub / Packaging の C# Project に限り、ユーザーは NuGet restore を明示許可した。
既存 C++ と Framework 4.8 VisualStudioBridge の Restore 禁止は維持する。
Package は Version 固定、Lock File を保持し、License を配布物へ含める。
Hub は Windows x64 self-contained の Publish を基本とし、実際の Runtime Pack の License / Notice を含める。
Velopack 1.2.161 を採用し、実 Setup / Update.exe で検証する。起動 Bootstrap は WPF 初期化前に実行する。
更新の確認 / 取得 / 検証は Hub、差替えと再起動は独立 Updater が行う。
自己更新は Hub の配置だけを対象とし、実行中の Editor / Project を巻き込まない。
GitHub Actions は Tag に一致した Version から Build / Test / Package を生成し、検証済み Asset を公開する。
実際の Tag / Push / GitHub 公開は別の外部操作であり、実装・ローカル検証だけで実施済みとはしない。

## 検証と未確定条件

Project 作成、正常 / 異常 Manifest、Release の混在・10 件・Draft・欠損・通信失敗、
署名・Hash・不正 Zip、取消・部分失敗・旧版保持、利用中排他、開発版保全、
Hub インストール・自己更新を Local Feed と専用 Test Root で検証する。
Engine は元 Source と異なる配置 / CWD で起動し、ゲームの Build / Play を確認する。
本番署名証明書、オンラインの公開サイト / MCP は後続 M12 で扱う。

# ADR-0020: Editor の Visual Studio 連携と Debug Play

## 決定

Script の編集と Debugger 接続を Editor から要求する。Runtime の Process 分離と製品の静的リンク方式は維持する。
Visual Studio は現在の Windows 用 Provider とし、共通契約へ COM / DTE / Windows の型を持ち込まない。
生成 Solution と Project は CMake から生成し、手書きや管理対象への追加は行わない。

| 所有する層 | 責務 |
| --- | --- |
| `Cue.EditorIde` | `IEditorIdeService`、要求・状態、Script Type ID から Source への解決 |
| `Cue.EditorIde.Windows` | `create_windows_ide_service`、Bridge Process / Pipe / Job、PID と実行 File の検証 |
| `Tools/VisualStudioBridge` | C# / STA / DTE による Solution 表示、Source 表示、Native Debugger 接続 |
| `Cue.ProjectBuild` / `.Windows` | IDE 用 Configure、開発用 DLL Build、生成物検証と Project 単位排他 |
| `Cue.EditorHost` | Owner Thread 上の `EditorScriptController`、Build → Reload / Debug 起動の順序 |
| `Cue.Editor.Ui` | メニューと Inspector の表示、所有 Action の返却 |
| `Cue.ScriptPlay` / Play Host | DLL 読込前の起動 Gate、所有子 Process の継続と停止 |

## Editor の操作

- `Script > Open Visual Studio`: 専用 Tree を Configure し、その Solution を開く。
- Inspector の `Edit Source`: Script Type ID を Source Map で解決して開く。確認済み Solution が残っていれば再利用する。
- `Script > Build Scripts`: 開発 DLL を Build / 検証し、成功時だけ Script Types Reload を要求する。
- `Debug Play`: DLL の Build 成功後、Debug 待機子を起動して Visual Studio を接続する。
- `Script > Cancel Script Operation`: 現在所有している要求を取り消す。
- 既存の `Reload Script Types`、通常の `Play` / `Stop`、製品の `Build Settings...` / `Run` は別の操作とする。

UI は File / Process / COM を操作しない。Host は ImGui の借用を終えてから要求を実行する。
`EditorScriptController` は `open_solution` / `open_script` / `build_scripts` / `debug_play` / `cancel` / `poll` を持つ。
重複要求、Owner Thread 違反、DLL の置換と Play が重なる要求を拒否する。Source 表示は通常 Play 中も許可する。
失敗は Editor の状態と診断に残し、失敗した Build の DLL を Reload / 起動しない。

## IDE と開発用 Build

`GameBuildPurpose` を `Player` / `ScriptModule` / `IdeProject` に分ける。
旧 `start(Project, Target)` は Player として維持し、目的付き overload を追加する。
`GameBuildStatus` は目的に加えて `solutionFile` / `moduleFile` / `symbolsFile` を所有する。

| 目的 | CMake Tree | 終了条件 |
| --- | --- | --- |
| Player | `Intermediate/EditorBuild/<Engine識別値>/Windows/x64` | `Cue.GamePlayerPackage` の完成物検証 |
| IdeProject | `Intermediate/IDE/<Engine識別値>/Windows/x64` | Configure と生成 Solution の検証 |
| ScriptModule | IDE と共通 | `Cue.GameProjectBuild` と DLL / Static / Manifest / PDB の検証 |

IDE と Script Build は同じ CMake 正本・Tree を使う。Engine 自身を開いた生成 Tree を書き換えない。
生成先直下の `<applicationName>.slnx` / `<applicationName>.sln` を確認し、不在・曖昧な結果を成功としない。
Editor は Configure に `CUE_GAME_SOLUTION_NAME` を渡し、CMake が Project のアプリ識別名と照合する。
ゲーム Script Target を生成 Solution の既定 Target とする。通常の Engine 開発 Solution 名は `CueEngine` のままとする。
旧 `CueEngine.slnx` が生成先に残っていても選択せず、ファイルの削除や手動の Solution 改名は行わない。
Configure は `BUILD_TESTING=OFF` とし、Test / 性能 Tool を要求しない。
IDE / ScriptModule は `gameTarget` を必要とするが、製品用 `startupScene` は要求しない。
`Run` は Player 成功時だけに許可し、IDE / DLL の成功を製品の起動許可へ流用しない。

開発 DLL は実行中 Editor の Native 構成に合わせる。製品 Build Panel の構成選択とは独立する。
DLL の MSVC Build は `/Zi` / `/DEBUG:FULL` を指定し、PDB を DLL と同じ Runtime Directory に出す。
既存の 11 Key の `GameModule.cuebuild` は変更せず、DLL / Static の Hash と識別を検証する。
PDB の通常 File も要求するが、Service 単独で DLL の CodeView と PDB GUID を照合する契約ではない。

3 目的とも `Intermediate/.cue-editor-build.lock` による Project 単位排他を共有する。
ただし Visual Studio 上で直接開始した Build はこの Lock を取得しない。
**外部 IDE から Build するときは Play を停止し、Editor の Build / Script 操作の完了を待つ。**
IDE の Build Event への排他組込み、外部 Build の完了検知と自動 Reload は今回の対象外とする。

## Script Source Map

`Source/ScriptSources.cueide` を Editor 専用の対応表とする。
`.cuescene` / Script ABI へ絶対 Source Path を追加しない。

```ini
formatVersion=1
script.sample.RotateCube=RotateCube.cpp
```

値は Project の Source Directory 基準の相対 Path。`resolve_script_source` は最大 64 KiB の UTF-8 を読み、
未知 ID、未知 Version、重複 Key、絶対 Path、`..`、Source 外参照、Link、存在しない通常 File を拒否する。
Source Map 自体も読込時点で親 Directory を含めて確認する。Inspector からの表示位置は現在 1 行目とする。
自動生成・Source 変更監視・型の定義行の解析は行わない。

## Debug Play の起動 Gate

1. 編集 Scene を所有 Snapshot として保持し、ScriptModule を Build / 検証する。
2. `PlayClient::start_debug` で所有 Play 子を起動する。
3. 子はゲーム DLL を読み込む前に `WaitingForDebugger` を通知する。
4. `IEditorIdeService::attach_debugger` に Solution、所有子 PID、期待する Play Host 実行 File を渡す。
5. Bridge は Native Debugger 接続を確認する。親は所有 Session / PID が変わっていないことも確認する。
6. `PlayClient::continue_debug` と子側の接続確認を経て DLL 読込・Script 開始へ進む。

通常 Play / Metadata 取得にはこの Gate を入れない。
Debugger 接続前は 120 秒の待機期限を設け、通常起動の期限も維持する。
Debug 接続済みの子が Break している間は通常の起動 / 応答期限で誤終了させない。
Stop の期限は解除せず、停止できない所有子は既存の Job による回収へ進む。
取消・失敗時に停止するのは自身が開始した同じ Debug Session に限る。
後から開始した通常 Play や別 Process を巻き込まない。

## Bridge の実行と導入条件

Bridge は .NET Framework 4.8 / C# 7.3 の従来形式 Project とする。
VS の MSBuild とインストール済み Reference Assemblies で Build し、NuGet Restore と `dotnet run` は行わない。
`Cue.VisualStudioBridge` が事前 Build し、`VisualStudioBridge.exe` と `.exe.config` を Editor 横へ配置する。
製品アプリから依存させず、Runtime やゲーム Script に C# の実行契約を追加しない。

Native Provider は非同期で一要求ずつ Bridge を起動する。結果は要求 ID / 操作 / 対象 PID と照合し、
上限付き応答と期限を持つ。既定期限は `WindowsIdeConfig::timeoutMilliseconds = 120000`。
STA 上の DTE 呼出しは Bridge に閉じ込め、既存 VS を完全な Solution Path で照合する。
別 Solution を開いている Instance の内容を勝手に置き換えず、必要なら別 Instance を起動する。
取消は Bridge を回収するが Visual Studio 自体を終了しない。
接続対象は PID だけで信用せず、保持した Process Handle と実行 File を確認する。

## 検証と制限

実施済みの自動検証と実 Visual Studio 操作の成否は
[検証記録](../Research/M11-VisualStudio-Integration-Validation.md) に分けて記録する。
Fixture の接続成功は、実 VS 上での Source 表示・Symbol Load・Breakpoint 到達の証明とは扱わない。
Visual Studio と .NET Framework 4.8 の導入が必要で、他 IDE / 他 OS の Provider は未実装。
Source 編集後の自動 Hot Reload、実行中 DLL の安全な置換、Source Map の自動更新は含めない。

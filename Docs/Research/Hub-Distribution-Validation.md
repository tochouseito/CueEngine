# Hub / Engine 配布の統合検証

日付: 2026-11-04 ～ 2026-11-05。対象は Windows x64。
Branch は `m11/issue146/project-loading`、検証開始時の HEAD は `9492422768c3b0350a3033aac07aa920b184450e`。
既存の未コミット変更を保持したまま実装した。Commit / Push / Tag / 実 Release 公開は行っていない。

## 実装範囲

1. Engine / Hub の Version 正本、配布 Manifest、署名、Project v2 の契約を定義した。
2. C++ `Cue.ProjectTools` と `CueProjectTool.exe` で独立 Template から Project を原子的に作成する。
3. Windows WPF Hub に Projects / Engines / Settings を設け、一覧・作成・登録・起動を接続した。
4. Stable / Preview を区別し、GitHub の Engine Release 最新 10 件を列挙する。
5. Source-backed Engine SDK と Development Editor の署名付き ZIP を生成する。
6. 署名 / Hash / Zip 検証、導入、修復、削除、取消、利用中排他を実装した。
7. 実行 File からの Engine Root 探索、配布版の Source checkout fallback 抑止、版照合を実装した。
8. .NET Runtime を含む Hub Setup と Full Package を Velopack で生成する。
9. Hub が更新を検証し、独立 Update.exe が Hub 終了後に差し替えて再起動する。
10. Tag 駆動の Build / Test / Package / Release Workflow と外部送信しない公開回帰 Test を追加した。

## ローカル成果物

検証 Root は `out/validation/HubDistributionCurrent.txt`、SDK の導入先は
`out/validation/HubInstalledEngineCurrent.txt` から参照する。
鍵はローカル試験用 ECDSA P-256 であり、本番の信頼鍵ではない。
正本 Version は Engine / Hub とも `0.1.0-preview.1`。
更新試験用 Hub preview.2 / .3 / .4 はローカルの VersionOverride だけで生成した。
最終成果物は検証 Root の `EnginePackageValidated/` と `HubPackageValidated/Release/`。
Engine ZIP は 84,055,133 Byte、Hub Setup は 75,289,695 Byte。
最終 Engine ZIP も別 Root へ再導入し、両 Template の作成と Lease を再確認した。
この検証は `out/validation/HubEngineFinalInstallProbe.log` に保存した。

## SDK の実利用

- 署名付き Engine ZIP を Hub と同じ EngineInstallService で Temp の別 Root に導入した。
- 導入 SDK の実 ProjectTool で Empty3D / RotateCube を作成し、Project ID の再生成と検証を確認した。
- 実 Native Editor と同じ共有 Lease がある間、Engine 削除が拒否されることを確認した。
- Source checkout の Editor を使わず、導入済み `Bin/Editor/CueEditorHost.exe` を ProjectService から起動した。
- Cube の表示、Editor の Build Scripts 成功、Play 子 Process で回転する Cube、Stop を確認した。
- 同じ導入 SDK の GameBuildService で独立 Player を Build した。
- Player を別の場所へ移し、WARP 90 Frame、Static Script 登録、回転、正常終了を Process Test で確認した。
- Player に Editor / Play 用ゲーム DLL を必須依存として持たせていない。

代表記録:

| 記録 | 内容 |
| --- | --- |
| `out/validation/HubEngineInstallProbe.log` | 実 ZIP 導入、二つの Template、利用中排他 |
| `out/validation/HubInstalledGameBuild.log` | 配布 SDK からの単独ゲーム Build、Succeeded |
| `out/validation/HubInstalledGameRun.log` | Relocated Player Process Test、Exit 0 |
| `out/validation/HubInstalledEditorPlay.png` | 配布版 Editor から起動した回転 Cube の Play |
| `<導入検証 Root>/result.json` | 実行 File / Project / Build ID と各検証結果 |

Temp 内での MSBuild には MSB8029 の配置 Warning が出る。Build と実行は成功した。
製品 Project を Temp に置くことは推奨しない。

## 自動検証の結果

| 検証 | 結果 |
| --- | --- |
| 規定の `scripts/codex_build.ps1` | Debug Build 成功 |
| Native CTest 全件 | 124 / 124 成功、133.72 秒 |
| Hub Core + 実 C++ CLI | 52 checks 成功 |
| WPF STA | 起動・3画面・Binding・未導入版の選択 / 起動抑止成功 |
| Package 契約 | 21 checks 成功、CTest にも登録 |
| 模擬 Tag Release | 59 checks 成功、実 GitHub へ送信なし |
| DXC | 固定版の展開・破損 Archive / DLL 拒否・Shader Compile 成功 |

全 Native Test は `out/validation/HubFinalNativeTests.log`、Build は
`out/validation/HubFinalNativeBuild.log`、Hub は `HubCoreTests.log` / `HubWpfUiTests.log`、
模擬公開は `TaggedReleaseOfflineTests.log` に保存した。
公開入口でも署名済み Version と Tag を直接照合し、異なる Version の公開を拒否する。

### 統合中に見つかった成果物更新漏れ

Editor の Build Scripts 後に別の Build Tree で Player を Build すると、共有 Static Library が更新され、
`GameModule.cuebuild` の Hash が古くなって Play が `GameBuild.artifact_hash_mismatch` で止まった。
誤った Artifact の実行は拒否されており、Build Scripts の再実行で Play と Stop が成功した。

`Cue.GamePlayerPackage` の完了条件へ `Cue.GameProjectBuild` を加え、Player Build が更新した
DLL / Static Library の Hash を再公開するよう修正した。Player 配布物にはゲーム DLL を含めない。
単体の Player exe Target を静的 Link する契約も維持する。
Package Target は検証情報を揃えるためゲーム DLL も Build するので、その分の Build 作業は増える。

回帰 Test は Source を変更して Static の Hash 変化と Manifest の両 Hash 一致を検証する。
修正前は失敗、修正後は成功（14.85 秒）。
記録は `HubPlayerManifestBeforeFix.log` / `HubPlayerManifestAfterFix.log`。
この変更後は規定 Debug Build と影響する CMake Fixture を再検証し、無関係な全 Test は繰り返さない。
変更後の Build も成功した。記録は `HubFinalNativeBuildAfterManifestFix.log`。

## Hub と更新

実 Setup を専用の HubInstalled Directory に導入し、実 Update.exe による
preview.3 → preview.4 の適用・自動再起動に成功した。
詳細は [Hub 検証記録](../../Tools/CueHub/VALIDATION.md) に記載する。

初回の画面確認で ProgressBar の TwoWay Binding による起動時例外を検出し、OneWay に修正した。
Core だけでなく STA 上に Window を生成し、全3画面の Binding と版選択を確認する回帰 Test を追加した。
修正後の追加の対話操作は Computer Use の許可待ちでタイムアウトしたため未実施。
WPF STA の画面生成 Test と実更新・再起動の成功を、対話操作の確認と区別する。

## ライセンス

指定された `Copyright (c) 2026 tochouseito` を Root と自作 ECS の MIT に反映した。
第三者の Copyright と許諾文は保持する。
ImGui の Font / stb / 日本語 Glyph Range の不足 Notice を追加し、DXC を公式の固定配布 Archive へ変更した。
Microsoft Debug CRT / Compiler / Visual Studio / Windows SDK / PIX Tool はパッケージに含めない。
Hub の .NET Runtime は一律 MIT とせず、実 Runtime Pack と Microsoft の該当 License を収録する。

監査詳細:

- [Third-party 監査](Hub-ThirdParty-Distribution-Audit.md)
- [Windows 配布条件](Hub-Windows-Redistribution-Audit.md)
- [Project 作成検証](Hub-Project-Creation-Validation.md)

## 未実施と残るリスク

- GitHub への実 Tag / Push / Release、オンライン更新は実施していない。Workflow はローカルの模擬公開 Test と区別する。
- 本番 Publisher 鍵と Actions Secret は未設定。ローカル試験鍵のパッケージを本番として公開しない。
- 本番 Authenticode / SmartScreen、別 PC、ネットワーク Proxy、再起動・電源断を挟む復旧は未検証。
- 極端に深い Directory 配置は未対応の Win32 Path 長制限に達する場合がある。最終再導入は短い専用 Root で成功した。
- SDK は Source-backed の初期形式。完全な Prebuilt import target SDK ではない。
- 同一 Project の共有成果物へ、別 Build Tree から同時に Build する競合は今回の逐次更新修正の対象外。
- Runtime の対応先を増やす実装は含めない。Hub の UI / インストーラーは Windows 専用。
- ローカル開発 checkout の削除・自動更新は行わない。

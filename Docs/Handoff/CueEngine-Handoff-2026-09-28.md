# CueEngine 開発引き継ぎ書（2026-09-28）

この文書の前半は 2026-09-28 時点の作業状態、後半は 2026-09-23 の最初の引き継ぎ書の原文である。後半は経緯と長期方針を保存する履歴資料であり、古い Path、Branch、担当分担、作業順序を現在への指示として扱わない。新しいチャットでは、ユーザーの最新の依頼、Repository の `AGENTS.md`、Live の Git／GitHub 状態を優先する。

## まず確認すること

1. 作業対象は `C:\Users\sinse\source\repos\CueEngine`、Remote は `https://github.com/tochouseito/CueEngine.git`。`CueEngineLegacy` と取り違えない
2. `git status --short --branch`、`git log -1 --oneline`、`git remote -v` で Live 状態を確認する
3. Code 変更前に `Engine/Documents/CODING_RULES.md`、設計変更前に関連 ADR を読む
4. Build は CMake を正本、Visual Studio 2026／MSBuild を Backend とする。既定 Debug Build は `pwsh -NoProfile -File scripts/codex_build.ps1`
5. Commit、Push、Branch 作成、Merge、GitHub の Issue／PR 更新は、その時点のユーザーの明示依頼に従う。今回のユーザーは #24〜#27 の現在の変更の Merge を明示依頼した

## Repository と Git の状態

| 項目 | 2026-09-28 の確認結果 |
| --- | --- |
| 新 CueEngine | `C:\Users\sinse\source\repos\CueEngine` / `https://github.com/tochouseito/CueEngine.git` |
| 開発統合先 | `develop`。`release` は製品側 Branch |
| Merge 前の `origin/develop` | `e5b73ff835ff870524dab45df235a706b866c531`（M04 #22 統合） |
| #24〜#27 統合 Branch | `m04/issue24-27/renderer-foundations` / `f6ab758` |
| `develop` の Merge Commit | `19a69488abae7ee0a8f1fce1455b31b9ac815e83` |
| `release` の確認時点 | `d5cc3cde37a3c26d050ed40ca32c8a08dbee6ba2` |
| Worktree 方針 | 追加 Worktree は使わず、上記 CueEngine Folder 内で Branch を切り替える |

この SHA は執筆時の値である。新しいチャットでは必ず Live の HEAD と Remote を再確認する。GitHub CLI の `gh auth status` は 2026-09-28 時点で無効な Token を報告した。GitHub Connector による読み取りは動作した。Git Push の結果は、この文書と `origin/develop` を照合して確認する。

### 比較対象の Repository

- Legacy: `C:\Users\sinse\source\repos\CueEngineLegacy` / `https://github.com/tochouseito/CueEngineLegacy.git`。2026-09-28 に Local `develop` は Clean、Local の `origin/Rebuild` は `f63884f658efc543855cd80045f734003bc610be` と確認した。Remote の最新状態は再確認する
- DramaEngine: `C:\Users\sinse\source\repos\DramaEngine` / `https://github.com/tochouseito/DramaEngine.git`
- 元文書にある `CueEngine-Rebuild` Path は現在存在せず、Legacy の Local Path は上記 `CueEngineLegacy` へ変わっている
- その他の過去 Engine は存在と Remote を使う前に確認する。旧 Code の再利用時は所有権、第三者由来、License、前提、Provenance を確認する

## 現在の構成と決定

- `Docs/Research/M00-01-Legacy-Feature-Parity-Matrix.md` に Legacy 機能の到達基準がある
- 会話で作成された GitHub Project は `CueEngine`、表示形式は Table。Project の URL と Field／Item の Live 状態はこの引き継ぎ時点で未確認
- `Docs/Decisions/0001-architecture-boundaries.md` から `0006-framegraph-and-fixed-mesh.md` までの ADR が設計判断を記録する。CMake を Build 定義の正本とする判断は ADR-0002
- `Engine/Source/Foundation` は Result／Error 等、`Platform` は Window と Windows Service、`Runtime` は共通 Runtime と Frame 制御、`WindowsHost` は Windows 固有の Composition Root
- `Engine/Source/Renderer/FrameGraph` の公開契約には D3D12 型を出さない。`Renderer/D3D12` は Device、View、Presentation、Command、Surface、Pipeline、Mesh の Owner を内部実装に分ける
- 現在の描画経路は FrameGraph の Clear → 固定 Triangle の Depth 付き Mesh Pass → Offscreen から Back Buffer への Copy → Present。初期実装は Direct Queue の直列実行
- Editor は Windows／ImGui 前提、Runtime は将来の複数 Platform を想定する。一般 Scene、Material、Asset Import、ImGui、Indirect Draw、LOD はこの Renderer Scope に含めない

## M04 と GitHub Issue の実態

2026-09-28 の GitHub 読み取りでは M04 Milestone `M04 Minimal Renderer and Presentation` は Open。#19〜#27 はすべて Open と表示された。Code の Merge 状態と Issue の Close 状態は一致していない。Issue の状態を Code だけから推測しない。

| Issue | 現在の Code 状態 | 次に確認すること |
| --- | --- | --- |
| #19〜#22 | `develop` に以前から統合済み | GitHub Issue は Open。Completion Gate で Close 判断 |
| #23 | M04 Completion Gate。Open | 全完了条件と実画面を確認した後に着手 |
| #24 | Device／Presentation／Command Context／View Manager 分離を今回統合 | GPU 待機異常時の解放と部分初期化失敗を重点確認 |
| #25 | FrameGraph Builder、依存・Barrier 計画、D3D12 Executor を今回統合 | 実 Window の Clear／Present と Debug Layer 診断を継続確認 |
| #26 | 固定用途の Offscreen／Depth と Frame Slot 寿命、Resize を今回統合 | Issue にある汎用 Texture／Buffer 作成・世代付き Handle 検証・破棄の受け入れ条件は未充足。Close しない |
| #27 | 固定 Triangle、Shader／PSO、CBV、WARP Pixel Test を今回統合 | Shader／PSO の無効 Handle 診断と一般的な StaticMesh 登録 API は未実装。Close しない |

上記の未充足項目は「今回の変更を Merge した」ことと区別する。#24〜#27 を完了として Close する前に Issue 本文の完了条件を一つずつ照合する。

## 検証記録と残る Risk

- 2026-09-28、`pwsh -NoProfile -File scripts/codex_build.ps1` の Debug Build 成功
- 同日、Debug／Development／Release の CTest は各 17 件すべて成功。Development／Release の Build は 2026-09-26 に成功し、2026-09-28 は既存 Build から Test を再実行した
- `Cue.Renderer.D3D12.FixedMeshPixel` は WARP の Readback で Triangle 中心 Pixel と背景の違いを確認し、D3D12 Debug Layer の Error／Corruption Message を拒否する
- 実機 GPU での画面目視、GPU 待機が連続失敗する経路、Device Lost の破棄順は未検証
- `D3D12Renderer::shutdown` は GPU 待機失敗時に State を保持して再試行できるが、Destructor まで連続して失敗した場合の資源寿命を再検討する
- `D3D12StaticMeshPool::create` の Upload 完了待機失敗時に、GPU が Upload Resource を使い終える前に失敗経路で解放しない保証を確認する
- `git diff --check` と Stage 差分の空白検査は成功した

## 次の作業候補

1. `origin/develop` に今回の Merge が到達していることを確認する
2. #24〜#27 の完了条件と上の未充足項目を照合し、必要な修正を Issue 単位で行う
3. 実 Window の Hardware 画面と Close／Resize、Debug Layer、異常停止経路を確認する
4. #23 Completion Gate を実施し、ユーザーの依頼に従って該当 Issue と Milestone／Project を更新する
5. 次の Milestone は長期目標（Editor／Project／File／Build／Package／Runtime を先に成立させる方針）と現状を比較して決める

## 元の引き継ぎ書について

以下は 2026-09-23 に渡された `CueEngine-New-Repository-Handoff.md` の原文である。歴史的な判断、Legacy 機能一覧、長期方針を欠落させないため全文を残す。ここから下の「最初に行うこと」や旧 Path／役割分担は当時の記録であり、現在の直接の作業指示ではない。

---

# 新CueEngine開発 引き継ぎ書

このタスクは、既存のCueEngine-RebuildをLegacyとして保存し、新しいRepositoryでCueEngineを再構築する作業です。

記録されているGit、GitHub、Issue、Milestoneの状態は2026年9月23日時点の情報を含みます。作業開始時には必ずLive状態を再確認し、現況を優先してください。

## 1. 新しい開発体制

新しいRepositoryでは、ユーザー本人がコーディングの主担当になります。

ただし、Issueの内容、規模、難易度、ユーザーの希望に応じて、Codexが実装を担当する場合もあります。Codexが実装する場合は、ユーザーの明示的な依頼を受け、Issue単位の最小Scopeで作業します。

### ユーザーの担当

- Repositoryの作成と改名
- Source Code実装の主担当
- Branch作成
- Commit
- Push
- Pull Request作成
- Merge
- 必要に応じた実行確認

### Codexの担当

- Architectureと設計案の整理
- Legacy機能の棚卸し
- Roadmap、Milestone、Issueの設計と作成
- Issue ScopeとAcceptance Criteriaの作成
- 実装前の設計確認
- Push前のローカル差分レビュー
- Code、所有権、寿命、依存関係、Error処理の確認
- 修正案の提示
- Build／Test結果の確認
- Architecture、冗長性、Performance Riskの監査
- Legacyとの機能差分管理
- 必要なADR候補の提示
- GitHub Projectの構築と進捗管理
- ユーザーから明示的に依頼されたIssueの実装または修正

Codexが実装した場合も、Commit、Push、Pull Request作成、Mergeはユーザーが担当します。Codexは変更内容、検証結果、未検証範囲、残るRiskを報告し、ユーザーの確認を待ちます。

### 基本Workflow

1. CodexがIssueの目的、Scope、Acceptance Criteriaを整理する。
2. ユーザーまたは明示依頼を受けたCodexが実装する。
3. CodexがPush前のローカル差分をレビューする。
4. ユーザーまたはCodexが指摘を修正する。
5. Codexが再レビューし、Build／Test結果を確認する。
6. ユーザーがCommit、Push、Pull Request作成、Mergeを行う。
7. 必要に応じてCodexがIssue、Milestone、GitHub Projectを更新する。

## 2. Repository

旧Repositoryはユーザーが改名し、Legacyとして保存します。

想定構成は次のとおりですが、新しいタスクの開始時に実際のURLとローカルPathを確認してください。

- Legacy GitHub: 旧`https://github.com/tochouseito/CueEngine`
- Legacy local: `C:\Users\sinse\source\repos\CueEngine-Rebuild`
- New GitHub: ユーザーが新規作成するCueEngine Repository
- New local: ユーザーへ確認する
- New default branch: ユーザーへ確認する

旧Repositoryと新Repositoryを取り違えないよう、GitHub URL、Remote、Repository Rootを毎回確認してください。

## 3. 新CueEngineの目標

新CueEngineは、Legacyの単純な縮小版ではありません。

- Legacyに実装済みの全機能へ再到達する。
- Legacyに残っている未完了Milestoneの機能も完成させる。
- 未完了だった全体影響の大きい設計を、初期Architectureから織り込む。
- Legacyで複雑化、冗長化、密結合した部分を見直す。
- 過剰な抽象化を避け、必要性を説明できる境界だけを導入する。
- 過去Engineと同等以上の機能を、より明確な所有権、寿命、Thread契約で実現する。
- Editor、Project管理、File操作、Build、Package、Runtime実行を先に成立させる。
- 高度なGraphics、Lighting、Sound、Effect、Physicsは基盤完成後とする。
- Asset Pipelineは初期Architectureへ織り込むが、本格実装順は基盤整備後とする。

「Legacy最新版まで進める」には、現在OpenのMilestoneを含む、予定されていた全機能の実装を含みます。

## 4. 基本技術方針

原則としてLegacyで決めた技術方針を引き継ぎます。

- C++中心
- C++20を初期基準とし、変更する場合はADRで決定
- Windows x64を最初のHost
- DirectX 12を最初のGraphics API
- 3Dゲームを主対象
- EditorはWindows／Dear ImGui
- Runtimeは将来的なマルチプラットフォーム対応を考慮
- RuntimeはEditorへ依存しない
- Platform固有型を上位のPlatform非依存APIへ漏らさない
- RendererはRuntime Worldへ直接密結合せず、抽出されたRender Dataを受け取る
- Authoring Scene、Runtime World、Editor Document、Asset／Resourceを区別する
- 永続形式にはVersionとMigration方針を持たせる
- Object、Entity、Component、Asset、Serviceを万能基底型へ統合しない
- 無制限なGlobal Singletonを導入しない
- 公開APIごとに所有権、寿命、Thread、失敗時挙動を定義する
- DLL／Plugin境界へSTL型、C++例外、生ポインタ所有権を安易に公開しない
- 製品Buildではプレイヤー側の安全性、Security、改変耐性を優先する
- 最終製品はMonolithic Buildを基本方針とする
- 開発時のEditor PlayとStandalone Runtimeは、共通Runtime基盤を使用しつつHost構成を分離できる設計とする

### Build Systemの注意

Legacyには次の指示が混在していました。

- CMakeを正式なBuild定義とする方針
- `Cue Engine.slnx`とMSBuildを直接使用する後期ルール

新Repositoryでは、この矛盾を持ち込まないでください。最初のArchitecture IssueまたはADRで、次を明確に決定します。

- 正式なBuild定義
- Visual Studio Solutionの扱い
- CMakeとMSBuildの責任範囲
- Debug／Development／Release構成
- Test Buildを通常の開発Solutionへ含めるか
- Editor、Tools、Tests、ProductのTarget分類

推奨初期案は「CMakeを正本とし、Visual Studio／MSBuildをGeneratorとBuild Backendとして使用する」です。ただし、ユーザーの判断を得て確定してください。

## 5. 外部Library方針

Legacyの方針を基本的に引き継ぎます。

- 新規Library導入またはVersion更新前にユーザーへ確認する。
- 対象、用途、License、Version、取得元、Build／配布への影響を示す。
- vcpkg Manifest Modeを使用する。
- Manifest、Install Tree、License、Noticeは`ThirdParty`配下へ集約する。
- 第三者Source、Header、BinaryをEngine所有Sourceへ混在させない。
- Git Submodule、FetchContent、手動Copy済みBinaryを安易に使用しない。
- `ThirdParty/THIRD_PARTY_NOTICES.md`と`ThirdParty/Licenses`で追跡する。
- 第三者Codeを直接改変せず、First-party Adapterで接続する。
- Dear ImGuiは公式Core、Win32 Backend、DirectX 12 Backendを候補とする。
- 新Repositoryで採用するDear ImGuiのVersionとLicense記録は、導入Issueで改めて確認する。

## 6. 旧EngineからのCode利用方針

新CueEngineでは、旧CueEngine、CueEngine-Rebuild、Theiatria Engine、Drama Engineなど、ユーザーが所有する過去EngineのCodeを、必要に応じて参考またはCopyできます。

Legacyの「旧Codeを一切Copyしない」という規則は、新Repositoryへそのまま適用しません。

ただし、Copyは自動的には行わず、候補ごとに次を確認します。

1. Copy元Repository、Branch、Commit、File
2. ユーザーが所有するCodeか
3. 第三者Codeや第三者由来部分が混在していないか
4. 旧実装が解決していた問題
5. 旧実装の前提、制約、既知の問題
6. 新CueEngineの現在要件に適合するか
7. そのまま利用、修正して利用、設計だけ参考、新規実装のどれにするか
8. 移植後のBuild／Test方法
9. ProvenanceとLicenseの記録方法

古い設計上の問題までCopyしないよう、機能と実装を分けて評価してください。

第三者CodeのCopyについては、所有権、License、ユーザー承認が確認できない限り行いません。

## 7. Legacyの到達状態

2026年9月23日時点のローカルRemote記録では、Legacyの`origin/Rebuild`は次のCommitです。

- SHA: `f63884f658efc543855cd80045f734003bc610be`
- M18 Editor Distribution and Installer完了時点
- Open Pull Request: 確認時点で0件

新しいタスクでは、改名後のLegacy URLと実際の最新Commitを再確認してください。

Legacyで実装済みの主な機能は次のとおりです。

- Result、Error、Assert、Fatal、Logger
- Windows Window Lifecycle
- D3D12 Adapter、Device、InfoQueue、DRED
- Queue、Fence、Frame Context、RTV／DSV、Swap Chain
- Resource Barrier、Clear、Present、Resize
- Cue.Math
- CPU／OS／GPU Capability
- Root境界付きFilesystem、Atomic Storage
- Project Descriptor、Blank Project、Recent Registry、Compatibility
- Schema Registry
- Entity、Component Storage、Query、Structural Command
- Runtime World
- SceneDocument、Hierarchy、Transform、Component Data
- Scene Serialization、Migration、未知Data保持、Atomic Save
- SceneからRuntime Worldへの実体化とRollback
- EditorDocument、Selection、Dirty State
- Command、Transaction型Undo／Redo
- Scene Save、Reload、Recovery
- Project Hub Service、ViewModel、ImGui UI
- Hierarchy、Inspector、Files UI
- Project作成、追加、一覧、削除、Folder表示
- Editor Docking UI
- GameView、DebugView
- Main Camera、Debug Camera
- Input抽象層とWindows入力
- Debug Camera操作
- CubeなどのBuilt-in Mesh基盤
- 最小Scene描画、Depth Test、Back-face Culling
- Editor Play入力Routing
- Runtime Loop、Frame Timing、System Registry
- Runtime Scene Session
- Game Module ABI
- Game Source Workspace生成
- Toolchain検出
- Cancel可能なProcess Runner
- Build Plan、Build Service、Build UI、Diagnostic Bundle
- Runtime Package、Runtime Data、Manifest、Publisher
- RuntimeHost
- EditorからPackage／Runtime実行までのWorkflow
- Dynamic開発BuildとMonolithic Shipping Build
- Shipping Product Security
- Editor Distribution、Installer、Update、Rollback、Uninstall

これらは「同じClass構成を再現する一覧」ではなく、新CueEngineが最終的に満たす機能基準です。

## 8. 未完了Milestone

改名後にGitHubのLive状態を再確認してください。

### M19 Built-in Assets and Primitive Resources

確認時点では5 Issue完了、3 Issue未完了でした。

- Runtime Scene v3とPrimitive Scene描画
- Shipping Built-in Reference Closureと未使用Payload除外
- M19 Completion Gate

新設計では、Built-in Assetも通常Assetと同じIdentity、Reference、Cook、Package Closureの原則で扱えるようにします。

### M24 Standalone Runtimeの最小Scene描画

確認時点では全10 Issueが完了し、Milestone Closeだけが残っていました。機能自体は新CueEngineの到達基準へ含めます。

### M25 Gameplay Scripting Foundation

LegacyではResearch段階でした。初期Architectureから少なくとも次を考慮します。

- ScriptとRuntime Worldの境界
- Script Instanceの所有権と寿命
- Game Module ABI
- Editor Play／Standalone／Shippingの違い
- Serialization対象
- Reload時のState移行
- Error Isolation
- 将来のHot Reload

実装言語や公開ABIはResearch／ADRで確定してから実装します。

### M26 Frame Scheduling and Main／Update／Render Thread Pipeline

LegacyではResearch段階でした。初期Architectureから次を考慮します。

- Main Thread
- Update Thread
- Render Thread
- Frame pacing
- CPU／GPU frame overlap
- Input取得Timing
- Scene mutation point
- Render Data extraction
- FenceとFrame Resource
- Shutdown、Error、Device Lost時の停止順序

Thread契約を後付けにしないことが重要です。

### M27 General-purpose Job System

LegacyではResearch段階でした。初期Architectureから次を考慮します。

- Worker ownership
- Task dependency
- Wait／Completion
- Cancellation
- Exception／Error伝達
- Shutdown
- Thread affinity
- Main／Update／Render Threadとの関係
- Profiling／Diagnostics
- Asset Import／Cookとの共有可能性

Job Systemを導入する前に、実際に並列化するWorkloadを定義してください。

### M28 Asset Identity, Database, and Import／Cook Pipeline

LegacyではResearch段階でした。初期Architectureから次を考慮します。

- Source AssetとRuntime Assetの分離
- Stable Asset ID
- Metadata
- Dependency Graph
- Asset Database
- Importer
- Cooker
- Cache
- Incremental Build
- Built-in Asset
- Package Closure
- 未使用Asset除外
- Editor Asset変更監視
- RuntimeからSource Assetを直接読まない契約
- Version、Migration、Compatibility

M28の実装自体は後でも、Asset参照を単なるFile Pathで固定しないよう、初期Scene／Project設計へ反映します。

## 9. Legacyから引き継がない問題

新CueEngineでは、Legacyで確認された次の問題候補を初期から監視してください。

- Module境界より細かい型やServiceの過剰分割
- 公開APIからPlatform依存が漏れる構造
- RuntimeHost CoreがWindows、Input、D3D12などを過剰に公開する構造
- Production CodeへTest Probeが混在する構造
- Package WriterとRuntime Reader間の重複処理
- Editor Composition Rootの分散
- Test／Probe EXEがVisual Studio上で大量に並ぶTarget構成
- Build／CTest時間の増大
- File I/O、Hash、Allocationの重複
- Architecture上の安全性のために導入した仕組みが、通常経路まで過度に複雑化する問題

ただし、推測だけで単純化しません。変更前に測定、依存関係、失敗経路を確認してください。

安全性のために維持すべきものは次のとおりです。

- Atomic Publish
- Rollback
- Staging／Post-publish Validation
- TOCTOU対策
- Dynamic／Static Trust分離
- Game Module ABIとLifetime管理
- Debug／Development／Release検証
- Authoring Scene／Runtime World／Editor Document分離
- Shipping ProductのSecurity設定

## 10. GitHub Project運用

新CueEngineではGitHub Projectsを使用します。

推奨Fieldは次のとおりです。

- Status: Backlog／Ready／In Progress／Review／Done
- Milestone
- Area
- Type: Research／ADR／Implementation／Test／Gate
- Priority
- Size
- Risk
- Legacy Parity
- Target Configuration

Milestoneは機能成果単位、IssueはユーザーまたはCodexが実装できる最小作業単位とします。

高Riskな次の項目にはResearch IssueまたはADRを先行させます。

- Architecture
- ABI
- 永続形式
- 公開API
- 所有権と寿命
- Thread契約
- GPU同期
- Asset Identity
- Script境界
- Package／Security境界

各Milestoneには最後にCompletion Gate Issueを置きます。

GitHub CLIでProjectsを書き換えるには`project`権限が必要です。2026年9月23日時点では、このPCのGitHub CLI認証が無効になっていました。新しいタスクでIssue、Milestone、Projectを作成する前に認証状態を確認してください。

## 11. Push前レビュー基準

レビューではFindingsを先に報告します。各Findingには次を含めます。

- Severity
- FileとLine
- 問題が発生する条件
- 影響
- 修正案
- 必要なTest

特に次を確認します。

- Issue Scope外の変更
- Coding Rules
- 所有権と寿命
- Null、範囲外、Overflow
- Error伝達
- File Path、Encoding、Atomicity
- Thread safety
- Shutdown順序
- ABI
- Serialization Version
- Backward／Forward Compatibility
- D3D12 Resource State
- Descriptor ownership
- Fence／Frame Resource
- Upload Resource lifetime
- Editor／Runtime依存方向
- Test HookのProduction混入
- Security
- Third-party provenance
- 不要な抽象化や重複
- Build／Test不足

問題がない場合も「重大な指摘なし」と、残る未検証範囲を明記します。

## 12. 新しいタスクで最初に行うこと

新しいタスクでは、直ちに実装を始めず、次の順序で進めてください。

1. ユーザーから新旧GitHub URL、新旧ローカルPath、Default Branchを確認する。
2. 新Repositoryの`git status`、Branch、HEAD、Remoteを確認する。
3. 改名後のLegacy RepositoryのRemoteと`Rebuild`最終Commitを確認する。
4. LegacyのOpen／Closed Milestone、Issue、Pull RequestをLive確認する。
5. LegacyのSource、Tests、Documents、ADR、Build定義を読み取り調査する。
6. `Legacy Feature Parity Matrix`を作成する。
7. 過去Engineから再利用できるCode候補を調査する。
8. 新CueEngineのArchitecture原則とBuild方針を提案する。
9. M19、M25、M26、M27、M28の要求を初期設計へ織り込む。
10. 新しいRoadmap、Milestone、Issue案をユーザーへ提示する。
11. ユーザー確認後にGitHub Project、Milestone、Issueを作成する。
12. Codingは原則ユーザーが行い、必要に応じてCodexへIssue単位で実装を依頼する。

## 13. Legacyローカル環境の状態

2026年9月23日時点のLegacyメイン作業場所は次の状態です。

- Path: `C:\Users\sinse\source\repos\CueEngine-Rebuild`
- Branch: `codex/issue-343-debug-camera-input`
- HEAD: `06e7f36bb3eba0b09542f8d2c701c6052ca9c0cc`
- このHEADは`origin/Rebuild`の祖先
- `origin/Rebuild`: `f63884f658efc543855cd80045f734003bc610be`

登録されていた追加Worktreeはすべて解除済みで、GitのWorktree登録は上記メイン作業ツリー1件だけです。Worktree用Branchは削除していません。

旧`CueEngine-M06-Gate`に存在した未コミットの日本語コメント追加は、次へ退避済みです。

- Backup: `C:\Users\sinse\source\repos\CueEngine-Legacy-M06-Gate-Main.cpp.backup`
- Note: `C:\Users\sinse\source\repos\CueEngine-Legacy-M06-Gate-Backup.md`
- SHA-256: `2B869DAF3651F7B34DBCD527B038051ADDCF23AFEC8A0C8512174AB3B07E82F8`

`C:\Users\sinse\source\repos\CueEngine-Issue-379`はGitのWorktree登録から解除済みで、中身も空ですが、別ProcessがFolderを使用中のため空Folderだけが残っています。使用中のProcess終了後に削除できます。

## 14. 最初の成果物

新しいタスクで最初に作る成果物はCodeではなく、次の提案です。

- 新旧Repository状態報告
- Legacy Feature Parity Matrix
- Legacyの問題点と再利用候補
- 新CueEngine Architecture原則
- Build System ADR案
- Thread／Job／Asset／Scriptingを織り込んだ依存関係図
- 新しいMilestone Roadmap
- 最初のMilestoneとIssue案
- GitHub Project構成案

これらをユーザーが確認した後、MilestoneとIssueをGitHubへ作成してください。

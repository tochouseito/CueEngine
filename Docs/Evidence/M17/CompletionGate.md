# M17 Monolithic Shipping Build Completion Gate

## Gate Result

M17の先行Issue #298から#306がGitHub上でClosedであり、本Gate Issue #307がMilestone最後の1件であることを
2026-09-12に確認した。PR #316の最終HeadはCodex Reviewで指摘なし、Windows CIのDebug／Development／Releaseが
全て成功し、未解決Review Threadは0件だった。

ADR-0024が定義するM17完了条件に従い、`MonolithicLocalReady = true`、
`PublicDistributionReady = false`と判定する。Release／Monolithic／UnsignedLocal Productはローカル開発用途で
生成、検証、移設、実行、停止できるが、公開配布用Certificateと外部Trust Anchorを使った実機検証は未実行である。

| Acceptance Gate | Result | Evidence |
| --- | --- | --- |
| M17 ADRと実装の一致 | Pass | ADR-0024の二軸Build、ABI v1共有、RuntimeHost Provider分離、Manifest v2、Trust Policy、Editor Workflowを#299から#306で実装 |
| Debug／Development／Release Build・CTest | Pass | 全Targetを3構成Build成功。Debug／Developmentは266/266、Releaseは262成功、既定4 Skip、失敗0 |
| Release Shipping Build・Artifact・Package・Run E2E | Pass | `Cue.Editor.Workflow.ProcessRoundTrip`でEditor Compositionから強制Configure、Product生成、Package、Smoke／Interactive起動を確認 |
| v1 Modular／v2 Monolithic回帰 | Pass | Build Profile、Game Module ABI、Package Manifest、RuntimeHost、Editor Workflowの両経路を3構成CTestとCIで確認 |
| Game DLL Import／Dynamic Project Code Loadなし | Pass | Shipping ProductのPE Import allowlistと禁止Loader APIを機械検査し、Static Query ProviderだけをLink |
| 配布禁止Fileなし | Pass | Manifest、Product EXE、Runtime Project、Runtime Sceneの4 Fileだけ。DLL、LIB、PDB、Source、Build Logなし |
| PE Hardening／Import／署名Policy | Pass | ASLR、DEP、CFG、CET、High Entropy VA、Load Policy、Import／Relocation／Load Config、Trust分類を検査 |
| Relocation／Tamper／Malformed／Atomic Recovery | Pass | Source非表示と無関係Current Directoryからの移設起動、独立改ざん拒否、Staging／Snapshot／Rollback Test成功 |
| 実Windowの起動・終了 | Pass | Productと同じFile Identityを持つProcess所有の可視Top-level HWNDを観測後、正常Stopと再実行可能状態を確認 |
| 公開署名の未実行理由とRisk | Pass | 実運用Certificate、Timestamp、Online Revocation、CMS署名、外部Trust Anchorが未提供であることを本文へ記録 |

## Verified Source Tree

M17実装の統合先は`Rebuild`のCommit `c1488e9d31ba602888f4abd95155e049ca6d9d29`、
Git tree `bb9a2a36e0aed903740aaa0d1530fd0501fbe017`である。PR #316のReview／CI対象Head
`22545990e2c6db33d7ef96187722ae4f6b2998cd`も同一Git treeである。

全体再検証はUNC Workspace修正直前のHead `9c8b1907e6598f26ca9c567e348f274e7cd63068`で実行した。
最終修正はTest DirectoryのLong Path変換と診断に限定され、最終Headでは対象E2Eを3構成で再実行し、
Windows CI run `34675587408`が全Target Buildと全CTestを3構成で再検証した。

#307は本Completion Gate文書だけを追加する。実装Git treeは#306統合時点から変更しない。

## Architecture and Compatibility Map

| M17 Scope | Verification |
| --- | --- |
| Configuration／Build Target二軸 | `Cue.Build.Plan`、`Cue.Build.Service`、ShippingはRelease限定の負例 |
| Artifact Identity／Toolchain Evidence | `Cue.Build.Windows.ArtifactPublisher`、完全なMSVC Toolset Versionと生成Tree照合 |
| Game Module ABI v1共有 | C11／C++20 Header Compile、Dynamic／Static Query Providerの同一登録・Lifecycle Test |
| Modular RuntimeHost | `Cue.RuntimeHost.Package.Process`、Dynamic ProviderのLoad／Unload、Manifest v1回帰 |
| Monolithic Product | Static Provider、`CueGameProduct.exe`、禁止Loader API／Game DLL Import不在検査 |
| Manifest v2／Package Inventory | `Cue.Package.Manifest`、`Cue.Package.Publisher`、v1／v2相互排他とRole／Hash／Size検証 |
| PE／Publisher Trust | `Cue.Build.Windows.ProductSecurity`、Malformed PE、Import、CFG、Relocation、Authenticode分類 |
| Editor Workflow | `Cue.Editor.ImGui.Build`、`Cue.Editor.Workflow.ProcessRoundTrip` |
| Relocation／Tamper／Cleanup | Source退避、Package移設、7種改ざん拒否、Package Tree不変、Process／Staging／Workspace回収 |

## Local Validation Results

- CMake Configure: 成功、266 Test登録
- Debug: 全Target Build成功、266/266 Test成功、178.81秒
- Development: 全Target Build成功、266/266 Test成功、109.02秒
- Release: 全Target Build成功、262 Test成功、既定4 Test Skip、失敗0、234.30秒
- 最終Debug E2E: 1/1成功、20.06秒
- 最終Development E2E: 1/1成功、15.78秒
- 最終Release Shipping E2E: 1/1成功、94.78秒
- E2E Workspace: 修正後の各実行で新規残留なし
- `git diff --check`: 成功

ReleaseでSkipされた既存Testは次の4件である。Debug Layer／InfoQueue／DREDを必要とする構成条件によるもので、
M17のBuild、Package、Runtime、Security、Editor Workflow Testは全て実行された。

- `Cue.RHI.D3D12.FrameCommand.InfoQueue300`
- `Cue.RHI.D3D12.RtvHeap.InfoQueue`
- `Cue.RHI.D3D12.SwapChain.InfoQueue`
- `Cue.RHI.D3D12.SwapChain.DeviceRemovalDredFailure`

## GitHub Gate

- PR #316 Head: `22545990e2c6db33d7ef96187722ae4f6b2998cd`
- Codex Review: 最終Headに指摘なし
- Windows CI: run `34675587408`、Debug／Development／Release成功
- Review Thread: 5件全て解決、未解決0件
- Merge: `c1488e9d31ba602888f4abd95155e049ca6d9d29`
- Open PR: #307開始時点で0件
- Branch: `codex/m17-307-completion-gate`、追跡先`origin/Rebuild`
- Working Tree: #307開始時点でClean

## Scope Audit

M17開始点`cb854c1a5e4711bf653cd003809987623819a9a6`から検証対象Git treeまでの変更は、
Build、Project Generator、Game Module ABI接続、RuntimeHost Provider、Shipping Product、Package、Editor Workflow、
Windows PE／Trust検証、Test、CMake、ADR、利用手順に限定されている。

- ECS Storage／Query、並列化、永続契約を変更していない。
- Renderer、Sound、Effect、Physicsの機能を追加または変更していない。
- Asset Import／Cook、Prefab、Scripting／Hot Reload、Runtime Plugin、Modを追加していない。
- 新規第三者LibraryまたはVersion更新を行っていない。
- 旧CueEngineまたは外部ProjectからSource Codeをコピー、移植、部分抽出していない。

## Validation Commands

- `cmake --preset windows-vs2026`
- `cmake --build --preset windows-vs2026-debug --parallel`
- `ctest --preset windows-vs2026-debug --output-on-failure`
- `cmake --build --preset windows-vs2026-development --parallel`
- `ctest --preset windows-vs2026-development --output-on-failure`
- `cmake --build --preset windows-vs2026-release --parallel`
- `ctest --preset windows-vs2026-release --output-on-failure`
- `ctest --preset windows-vs2026-<configuration> -R "^Cue.Editor.Workflow.ProcessRoundTrip$" --output-on-failure`
- `git diff --name-only cb854c1a..c1488e9d`
- `git diff --check`

CIでは固定済みvcpkg ManifestからDependencyを復元後、同じConfigure、3構成Build、CTestを実行した。

## Not Run

- 実運用の公開配布用CertificateによるAuthenticode署名、Timestamp、Online Revocation、Certificate Chain検証
- Detached CMS／PKCS#7 Manifest署名と、許可Publisherを強制するInstaller／Launcher／App Control
- 権限制限Tokenまたは別Standard User AccountによるACL試験
- Engine Source／Build TreeをOS-levelでAccess Denialにした実行
- UNC共有上に実配置したE2E Workspaceの削除。UNC／Drive／既拡張Pathの変換分岐は自動Test済み
- 別Machine、Windows別Build、低性能CPU、HDDでのBuild、Package、Startup測定
- 数時間以上のSoak、Crash dump、電源断相当の復旧
- AddressSanitizer、ThreadSanitizer、UndefinedBehaviorSanitizer
- Installer、Updater、Store／Device配布
- Asset Import／Cook、Game Rendering、3D Viewport、Gizmo、Sound、Effect、Physics、ECS改良

## Existing Problems and Remaining Risks

- `UnsignedLocal`はローカル開発専用であり、公開配布の改ざん耐性を保証しない。
- `PublicDistributionReady = false`であり、実運用Certificateと外部Trust Anchorを導入するまで公開署名済み製品とは扱わない。
- D3D12 ordinal 101の許可は現行Windows SDK／MSVC出力に固定され、Toolchain更新時に再検証が必要である。
- Engine Build Tree非依存はSource退避、絶対Path非包含、移設、無関係Current Directoryで検証したもので、OS-level Access Denialではない。
- Standard User相当GateはPackage Tree不変とRoot分離による検証で、制限TokenによるACL試験ではない。
- Link／Startup Baselineは一台のMachine上の最小Productだけを表し、将来のGame CodeやAsset規模を予測しない。
- 修正前のTest実行で生成された過去E2E Workspace 14件は本Issueで削除していない。修正後の実行では増加していない。
- ReleaseでSkipされた4件はM17変更とは無関係だが、Release構成のDebug Layer／InfoQueue／DRED経路は未実行である。

## Scope-out Candidates

- 公開配布署名、外部Trust Anchor、Installer／Launcherを一体で決めるResearch Issue
- Toolchain更新時のPE Import／CFG／D3D12 ordinal互換Gate
- 過去E2E Workspaceを安全に列挙・回収するTest maintenance

## Next Action

M17 Close後は、ユーザー方針に従い、既存Moduleの冗長性、誤った境界、不要なAllocation／Copy、
性能阻害候補を変更なしで計測・調査するResearch Milestoneを定義する。

# M00-01 Legacy Feature Parity Matrix

更新日: 2026-09-24

対象 Issue: [CueEngine #1](https://github.com/tochouseito/CueEngine/issues/1)

目的: 新 CueEngine が Legacy と同等の利用体験に到達するための機能基準を定める。クラス構成やファイル配置の一致は要求しない。

## 比較対象と判定方法

| リポジトリ | 参照ブランチ | 固定したコミット | この表での役割 |
| --- | --- | --- | --- |
| [CueEngine](https://github.com/tochouseito/CueEngine) | `develop` | [`d5cc3cd`](https://github.com/tochouseito/CueEngine/tree/d5cc3cde37a3c26d050ed40ca32c8a08dbee6ba2) | 新実装の起点。各機能の実装状況は未監査。 |
| [CueEngineLegacy](https://github.com/tochouseito/CueEngineLegacy) | `Rebuild` | [`f63884f`](https://github.com/tochouseito/CueEngineLegacy/tree/f63884f658efc543855cd80045f734003bc610be) | 機能と受け入れ条件の主基準。 |
| [CueEngineLegacy](https://github.com/tochouseito/CueEngineLegacy) | `develop` / `developRef` | [`8476b65`](https://github.com/tochouseito/CueEngineLegacy/tree/8476b656c24e4ef6262761fefed9b37566932306) / [`6a9234d`](https://github.com/tochouseito/CueEngineLegacy/tree/6a9234d9e4f52b85d6da878129c304d8d6450b83) | 旧系列の比較候補。詳細な機能監査は未実施。 |
| [TheatriaEngine](https://github.com/tochouseito/TheatriaEngine) | `master` | [`35af5ce`](https://github.com/tochouseito/TheatriaEngine/tree/35af5ce93b259f1d595c60ca2828f68ca2c626b8) | 設計比較候補。Parity の基準にはしない。 |
| [DramaEngine](https://github.com/tochouseito/DramaEngine) | `master` / `develop` | [`5258076`](https://github.com/tochouseito/DramaEngine/tree/52580760b774875e14943f9846bb58b23e682192) / [`475bf51`](https://github.com/tochouseito/DramaEngine/tree/475bf51c599a8684b12cad10775f6c7c7b18d7ba) | 設計比較候補。Parity の基準にはしない。 |

「実装済み」は固定コミットのコードと完了記録で確認できた範囲、「部分実装」は機能や検証範囲に制限があるもの、「未完了」は Issue が開いているもの、「研究中」は Research Issue の段階を指す。完了記録は当時の検証結果であり、この調査でビルドや手動操作を再実行した意味ではない。新 CueEngine の列は**実装済みという判定ではなく到達目標**である。後続マイルストーン名は仮の配置候補で、正式な番号・順序は未決定。

## 機能 Matrix

| 領域 / 機能 | Legacy の状態と根拠 | 新 CueEngine の到達目標・受け入れ観点 | 先行依存 | 後続マイルストーン候補 |
| --- | --- | --- | --- | --- |
| Project / 空の 3D Project 生成 | **実装済み**。Project Generator と生成テスト、M16 の実操作記録。[実装][L01] [試験][L02] [E2E][L03] | Project Hub から空の Project を生成し、再起動後も開ける。Project ID、起動 Scene、3 構成の生成物を検証する。Windows x64 以外は別途定義。 | Project Descriptor、Filesystem、Build 定義 | Project / Hub |
| Project / Hub 一覧・再オープン・Editor 起動 | **実装済み**。Hub Service、プロセス試験、M16 E2E。[実装][L04] [試験][L05] [E2E][L03] | 複数 Project の一覧、選択、再オープン、Editor の別プロセス起動を再現する。失敗時に対象 Project を特定できる。 | Project 生成・Descriptor | Project / Hub |
| Project / Installed SDK と Editor の選択 | **開発者向けに実装済み**。M18 の配布範囲に限定。[M18][G18] | インストール済み SDK と Editor の対応バージョンを使って Project を開ける。一般配布の完了とは分けて検証する。 | Build、Distribution | Distribution |
| Scene / 新規・開く・保存・別名保存・再読込 | **実装済み**。Persistence、M12 Gate と Workflow 試験。[実装][L06] [試験][L07] [M12][G12] | 同じ Scene 内容を保存後に復元できる。Project/Scene/Object ID を再起動後も維持し、保存失敗時は既存ファイルを壊さない。 | Project、Scene Document、原子的 File I/O | Scene / Editor Core |
| Scene / 未保存確認・復旧 | **実装済み**。Editor UI と Workflow 試験。[実装][L08] [試験][L07] | 閉じる・切り替える前に未保存変更を扱い、異常終了後の復旧候補を提示できる。失敗注入を含めて検証する。 | Scene 永続化、Editor Document | Scene / Editor Core |
| Editor / Hierarchy・Inspector と Object 編集 | **実装済み**。Object 追加、親子、名前、Transform、選択を M16 で実操作。[実装][L08] [文書][L03] | Object を追加・選択・親子付け・改名し Transform を編集でき、保存・再読込でも一致する。循環した親子付けは拒否する。 | Scene Document、Editor Document | Editor Core / UI |
| Editor / Undo・Redo | **実装済み**。Editor Core の Command と試験。[実装][L09] [試験][L10] | 上記編集操作の Undo/Redo を提供する。永続 Undo 履歴は Legacy との同等範囲と分けて要件化する。 | Editor Document、Command | Editor Core / UI |
| Project Files / 一覧・検索・ファイル操作 | **実装済み**。作成・改名・移動・コピー・Trash/Restore・外部変更の再走査。[実装][L11] [試験][L12] [M13][G13] | Project 内のファイル操作と外部変更の反映を提供する。ルート外への脱出、衝突、失敗後の復旧を試験する。一般的な Asset Import/Cook とは別機能。 | Project Root、Filesystem、Editor UI | Project Files |
| Editor / Play・Stop・Console | **実装済み、描画範囲は限定**。Authoring 状態を保つ Play Session と試験。[実装][L13] [試験][L14] [M14][G14] | Editor 内で Play/Stop し、実行中の状態を Authoring Scene に混入させず、診断を Console に出す。 | Scene、Runtime World、Game Module、Editor Core | Editor Play |
| Editor / Game View・Debug View | **部分実装**。ImGui Game View と Windows D3D12 ToolHost。M23 で入力経路を検証。[実装][L15] [実装][L16] [M23][G23] | Play 中の Game View、編集用 Debug View、入力フォーカスの切替を再現する。Renderer の対応範囲は Rendering 行で判定する。 | Editor Play、Input、Renderer | Editor View / Input |
| Input / Editor と Runtime の入力経路 | **実装済み**。M23 で portable input routing と Win32 adapter を完了。Debug camera wheel の手動検証は限定。[M23][G23] | Editor UI と Game View の入力を分離し、Runtime に必要な入力を platform adapter 経由で届ける。フォーカス切替を手動確認する。 | Platform、Editor View、Runtime | Input |
| Build / Debug・Development・Release | **実装済み**。Legacy は CMake/CTest の 3 構成。M16 などで構成別ビルドを記録。[定義][L17] [M16][G16] | 新リポジトリで選んだビルド方式により 3 構成を一貫してビルド・検証できる。構成名と成果物の契約を先に固定する。 | Toolchain、Project Generator | Build Foundation |
| Build / Game Module と versioned ABI | **実装済み**。C ABI query と Build Service、ADR-0022。[ABI][L18] [実装][L19] [ADR][D22] | Project の Game Module をビルドし、ABI 不一致を明示的に拒否する。Module の所有権・寿命、失敗後の再試行を定義する。 | Build Foundation、Runtime API | Game Module / Build |
| Build / ビルド取消・成果物の固定 | **実装済み**。Build Service が cancel と immutable artifact を扱う。[実装][L19] [ADR][D22] | 取消や失敗を既存の起動可能成果物へ波及させず、選択した build artifact を package に渡す。 | Game Module、Artifact 管理 | Game Module / Build |
| Package / Build → Package → Run | **実装済み、対象は起動 Scene 中心**。Workflow と M16 E2E。[実装][L20] [試験][L21] [E2E][L03] | 生成 Project をビルド、パッケージ化、起動し、Scene と Game Module の整合を確認する。一般 Asset Cook は追加要件とする。 | Build、Scene、RuntimeHost | Package / Run |
| Runtime / Host と World の分離 | **実装済み**。RuntimeHost、RuntimeWorld、Session。[Host][L22] [World][L23] [Session][L24] | Host の platform/process 寿命と Runtime World の Scene/Object 寿命を分離する。Editor Play と standalone の両方で同じ境界を試す。 | Platform、Scene、Game Module | Runtime Core |
| Runtime / Scene 起動とデータ形式 | **v2 まで実装済み、v3 は未完了**。M24 v2 scene pixel、M19 #386 が v3。[M24][G24] [#386][I386] | 起動 Scene の形式と version 移行規則を明文化し、Editor 保存物・Package・Runtime 読込の互換性を試す。v3/Primitive は M19 追跡行を参照。 | Scene、Package、RuntimeHost | Runtime Scene |
| Graphics / 最小 standalone 描画 | **限定して実装済み**。M24 は Camera と固定 Cube pass を描画し Hardware/WARP の scene pixel を確認。[実装][L25] [M24][G24] | Camera/Cube の visible pixel を standalone で確認する。Renderer 一般化、Material、Texture、Lighting、任意 Mesh は別行程として定義する。 | Runtime Scene、D3D12 backend、Package | Graphics Baseline |
| Graphics / Editor 内の描画接続 | **部分実装**。RenderSnapshot と ToolHost の offscreen surface。扱う Scene は限定。[Snapshot][L26] [ToolHost][L16] [M24][G24] | Game View/Debug View が Runtime の描画結果を表示する。描画に渡すデータの所有権と frame 寿命を明確にする。 | Editor View、Runtime、Renderer | Graphics / Editor |
| Shipping / Dynamic・Static・Monolithic | **実装済み**。M17 で dynamic modular と static monolithic の build/package/run、shipping の Game DLL import 排除を確認。[M17][G17] | 少なくとも Legacy と同じ配布モードを再現し、依存 DLL、Game Module、起動 Scene の閉包を検証する。 | Build、ABI、Package、Runtime | Shipping |
| Distribution / SDK・Editor installer、更新・Rollback | **開発者配布として実装済み**。M18 は install/update/rollback/uninstall と 310 tests。`PublicDistributionReady = false`。[M18][G18] | 開発者配布と公開配布の判定を分け、後者は署名と外部 trust anchor を満たしてから完了とする。 | Shipping、Versioning、Installer | Distribution |

## Legacy の未完了範囲と後続研究

| 領域 / 追跡先 | Legacy の状態と根拠 | 新 CueEngine の到達目標・受け入れ観点 | 先行依存 | 後続マイルストーン候補 |
| --- | --- | --- | --- | --- |
| M19 / Built-in Primitive と Runtime Scene v3 | **未完了**。M19 milestone は open。Catalog #383、Plane/Sphere Geometry #384、Editor Create Primitive #385 は closed。Runtime Scene v3/Primitive draw #386、Shipping Reference Closure #387、Completion Gate #388 は open。[M19][M19] [#386][I386] [#387][I387] [#388][I388] | Closed issue と開いた gate を別々に移す。Primitive の生成、Scene 保存/読込、standalone pixel、shipping 参照閉包が揃った時に完了を判定する。ADR-0031 は採用済み設計の参照であって実装完了の証拠ではない。[ADR][D31] | Scene v3、Renderer、Package | Built-in Asset / Primitive |
| M24 / Scene Rendering Baseline の残差 | **機能 gate は closed、milestone は open**。10 issue closed、0 open。Hardware/WARP の Dynamic Package と static-link **test product** scene pixel は pass。生成した Shipping product の v2 scene pixel E2E は未実行。[M24][G24] [Gate Issue][I359] | Legacy で未実行の生成 Shipping product E2E を新実装の受け入れ条件に含める。Camera/Cube 以外の描画能力を M24 完了から推定しない。 | Graphics Baseline、Shipping | Graphics Baseline / Shipping E2E |
| M25 / Gameplay Scripting | **研究中**。方式、ABI、所有権、Thread 契約を決める Research Issue #367 が open。実装は Issue 対象外。[#367][I367] | 言語・実行方式と Script Lifecycle を決め、最小 Attach→Update→Scene 操作→Standalone 実行の gate を別途設ける。Hot Reload 等は未決定。 | M24、Game Module、Runtime World | Gameplay Scripting |
| M26 / Frame 制御・役割 Thread | **研究中**。Main/Update/Render Thread 契約の Research Issue #368 が open。実装は Issue 対象外。[#368][I368] | Target FPS、VSync、Update 方式、frame 先行上限、Snapshot 寿命と single-thread fallback を決定し、同条件の測定を受け入れ条件にする。 | M25 Script Thread 契約、Renderer | Frame / Thread |
| M27 / 汎用 Job System | **研究中**。API、依存、停止契約の Research Issue #369 が open。実装は Issue 対象外。[#369][I369] | 投入・完了・依存・取消・停止・失敗伝播を定義し、Worker から Editor/Runtime 状態や D3D12 Resource を直接触らない境界を検証する。 | M26 Thread 契約 | Job System |
| M28 / Asset Database・Import/Cook | **研究中**。Asset ID、Database、Import/Cook、Package 境界の Research Issue #370 が open。実装は Issue 対象外。[#370][I370] | Source Asset から Cook 済み Runtime Asset と Package への経路、原子的公開、Rollback、依存閉包を決める。形式別 Importer は後続 Issue とする。 | Project Files、Package、M19 Built-in Asset との区別 | Asset Pipeline |

M25～M28 の上記名称は現在の Research Issue に基づく。方式、具体的な機能範囲、実装 Issue と gate は研究結果で確定する。ここでは未決定範囲を既存機能として先取りしない。

## 再利用候補と出自の確認

「同じにする」は外から確認できる機能と重要な設計契約の一致を基本とする。以下の採用方法は提案であり、ソース移植の承認ではない。各候補についてライセンス、第三者コードの混入、変更履歴、依存ライブラリ、新設計の ownership/API への適合を移植前に確認する。Legacy の [MIT LICENSE][LIC] だけでは、[ThirdParty notices][TP] にある別条件のコードまで一括して再利用可能とは判定できない。

| 候補と固定した場所 | 分類・所有権 | 適合性と既知の制限 | 採用方法・未確認事項 | 関連 Matrix 行 |
| --- | --- | --- | --- | --- |
| Legacy `Rebuild` / `f63884f` / [SceneDocument.h][R01] | **実装再利用候補（条件付き）**。Scene の永続 ID と Document を Scene 層が所有する。 | Scene 保存と Runtime への変換に適合候補。M19 の Runtime Scene v3 は未完了で、現行 format のまま最終形とはできない。 | まず ID・version・失敗契約を採用し、型や format の移植は境界レビュー後。ファイル単位の由来、第三者部分、依存、権利を未監査。 | Scene、Runtime Scene |
| Legacy `Rebuild` / `f63884f` / [EditorDocument.h][R02] | **実装再利用候補（条件付き）**。Authoring 状態と Command を Editor Core が所有する。 | Authoring/Runtime 分離に適合候補。永続 Undo 履歴と複数 Scene UI は実証されていない。 | API を新境界に合わせ、必要なら再実装。ファイル単位の由来、第三者部分、ImGui 依存、権利を未監査。 | Editor、Undo/Redo |
| Legacy `Rebuild` / `f63884f` / [Build Plan.h][R03] / [Package Workflow.h][R04] | **設計のみ参考**。Build Service が artifact、Package Workflow が製品構成を所有する。 | Build→Package→Run 契約は適合候補。Legacy の CMake/CTest と新リポジトリのビルド方式は未整合。 | Artifact 固定・取消・Package 検証の契約を抽出する。コード移植を選ぶ場合は由来、第三者部分、ツール依存と権利を別途監査。 | Build、Package、Shipping |
| Theatria `master` / `35af5ce` / [SceneManager.h][R05] | **設計のみ参考**。旧 SceneManager の Scene 遷移責務を比較する。 | `LoadSceneAsync` は未実装 stub であり、Parity の到達証拠には使えない。 | 呼出設計だけ比較。Repository に第三者コードが含まれるため、由来・権利・依存を確認するまでは移植対象にしない。 | Scene、Runtime |
| Drama `master` / `5258076` / [DramaEngine.slnx][R06] | **設計のみ参考**。Solution/Build 定義の比較。 | 新 Build Foundation の候補だが、Scene/Runtime 機能を証明しない。 | Solution 構成だけ比較。[LICENSE.txt][R07] の `[year] [fullname]` は未記入で、所有権と権利を未確認。 | Build Foundation |

## 受け入れ順序と保留事項

1. Project Descriptor / Generator / Hub と 3 構成 Build の基盤を固定する。
2. Scene Document と保存・復旧、その上の Editor Core / Project Files を作る。
3. Game Module ABI、Runtime World、Editor Play を接続する。
4. Package / Run、最小描画、Shipping の E2E で「作る→編集する→実行する→配る」を確認する。
5. M19 の残りと M25～M28 の研究結果を反映し、後続機能の gate を追加する。

保留: 新 CueEngine `develop` の実装監査、Legacy `develop` / `developRef` と Theatria / Drama の全面比較、各候補のファイル単位ライセンス監査、後続 milestone の正式割当、Windows x64 以外の到達条件。これらは Matrix の欠落を隠さないため明記する。

<!-- Legacy evidence, all source links pinned to f63884f658efc543855cd80045f734003bc610be. -->
[L01]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/Project/Private/Generator.cpp
[L02]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Tests/Project/GeneratorTests.cpp
[L03]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Docs/Evidence/M16/FirstUsableWorkflow.md
[L04]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/ProjectHub/Private/Service.cpp
[L05]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Tests/ProjectHub/EditorProcessTests.cpp
[L06]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/EditorCore/Private/Persistence.cpp
[L07]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Tests/EditorWorkflow/EditorWorkflowProcessTests.cpp
[L08]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/Editor/ImGui/Private/EditorPresenter.cpp
[L09]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/EditorCore/Private/EditorDocument.cpp
[L10]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Tests/EditorCore/EditorCoreTests.cpp
[L11]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/EditorCore/Private/FilesWorkspace.cpp
[L12]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Tests/EditorCore/FilesWorkspaceTests.cpp
[L13]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/EditorCore/Private/EditorPlaySessionController.cpp
[L14]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Tests/EditorCore/EditorPlaySessionControllerTests.cpp
[L15]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/Editor/ImGui/Private/GameView.cpp
[L16]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/ToolHost/WindowsD3D12/Private/ToolHost.cpp
[L17]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/CMakePresets.json
[L18]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/GameModule/Public/Cue/GameModule/GameModuleAbi.h
[L19]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/Build/Public/Cue/Build/Service.h
[L20]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/Package/Public/Cue/Package/Workflow.h
[L21]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Tests/Package/CMakeLists.txt
[L22]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/RuntimeHost/RuntimeHostApplication.cpp
[L23]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/GameCore/Public/Cue/GameCore/RuntimeWorld.h
[L24]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/Runtime/Public/Cue/Runtime/RuntimeApplicationSession.h
[L25]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/RuntimeHost/RuntimeSceneFrame.cpp
[L26]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/Renderer/Public/Cue/Renderer/RenderSnapshot.h
[G12]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Docs/Evidence/M12/CompletionGate.md
[G13]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Docs/Evidence/M13/CompletionGate.md
[G14]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Docs/Evidence/M14/CompletionGate.md
[G16]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Docs/Evidence/M16/CompletionGate.md
[G17]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Docs/Evidence/M17/CompletionGate.md
[G18]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Docs/Evidence/M18/CompletionGate.md
[G23]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Docs/Evidence/M23/CompletionGate.md
[G24]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Docs/Evidence/M24/CompletionGate.md
[D22]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Docs/Decisions/0022-game-module-project-build-abi-contract.md
[D31]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Docs/Decisions/0031-built-in-asset-catalog-runtime-inclusion-contract.md
[M19]: https://github.com/tochouseito/CueEngineLegacy/milestone/20
[I386]: https://github.com/tochouseito/CueEngineLegacy/issues/386
[I387]: https://github.com/tochouseito/CueEngineLegacy/issues/387
[I388]: https://github.com/tochouseito/CueEngineLegacy/issues/388
[I359]: https://github.com/tochouseito/CueEngineLegacy/issues/359
[I367]: https://github.com/tochouseito/CueEngineLegacy/issues/367
[I368]: https://github.com/tochouseito/CueEngineLegacy/issues/368
[I369]: https://github.com/tochouseito/CueEngineLegacy/issues/369
[I370]: https://github.com/tochouseito/CueEngineLegacy/issues/370
[R01]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/Scene/Public/Cue/Scene/SceneDocument.h
[R02]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/EditorCore/Public/Cue/EditorCore/EditorDocument.h
[R03]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/Build/Public/Cue/Build/Plan.h
[R04]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/Engine/Source/Package/Public/Cue/Package/Workflow.h
[R05]: https://github.com/tochouseito/TheatriaEngine/blob/35af5ce93b259f1d595c60ca2828f68ca2c626b8/project/Cho/GameCore/SceneManager/SceneManager.h
[R06]: https://github.com/tochouseito/DramaEngine/blob/52580760b774875e14943f9846bb58b23e682192/DramaEngine.slnx
[R07]: https://github.com/tochouseito/DramaEngine/blob/52580760b774875e14943f9846bb58b23e682192/LICENSE.txt
[LIC]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/LICENSE.txt
[TP]: https://github.com/tochouseito/CueEngineLegacy/blob/f63884f658efc543855cd80045f734003bc610be/ThirdParty/THIRD_PARTY_NOTICES.md

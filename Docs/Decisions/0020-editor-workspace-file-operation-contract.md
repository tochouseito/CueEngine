# ADR-0020: Editor Workspace File Operation Contract

- Status: Accepted
- Date: 2026-09-06
- Decision Owners: CueEngine Project

## Context

M13では、Project内のFileとDirectoryをEditorから列挙、作成、Rename、Move、Copy、Delete、Restoreし、外部変更を
検出できる最小Files Workflowを構築する。M09で導入した`FilesystemRoot`は、検証済みRoot内のAtomic Storageと
Project生成に必要な最小操作を提供するが、Editor向けの列挙、複数Entry操作、回復可能Delete、Watcher、Native Dialog、
ViewModelとの責務境界は決定していない。

File操作をImGui Callbackまたは各Toolへ直接実装すると、Project Root外への脱出、Reparse Pointの暗黙Traversal、
既存Dataの上書き、途中Copyの公開、開いているSceneとの競合、外部変更Eventの取りこぼしを各呼び出し側が個別に
処理することになる。また、ADR-0013のUser Workspaceと、Project Root内の制作Fileを扱うWorkspace File操作を
同じ正本として扱うと、Machine固有状態と共有Dataの境界が崩れる。

本ADRは、M13のProject Workspace File操作について、Module責務、PathとRoot境界、列挙、Mutation、回復可能Delete、
外部変更、Native Dialog、所有権、Thread、失敗時状態、Headless Test境界を決定する。Asset Identity、Import／Cook、
Asset Database、Prefab、Source Control、Cloud Storage、Runtime Packagingは決定しない。

## Legacy and Prior Engine Reference

### Legacy CueEngine

旧CueEngineは、Project作成、Scene保存、File選択をEditorから短い経路で実行する必要があった。一方、Projectと
Editorの処理が絶対Pathと実行中Objectへ到達しやすく、Root Binding、Atomic Publish、Rollback、回復可能Delete、
外部変更の正本が明確ではなかった。ADR-0013、ADR-0014、ADR-0018で確認した問題と制約だけを本判断へ利用し、
旧Source Code、型、File Layout、実装手順はコピー、移植、改名、部分抽出しない。

### TheatriaEngine

TheatriaEngineは、Folder TreeとFile Gridを表示し、外部からDropされたFileをProjectへ追加する短い制作導線を持つ。
しかし、確認時点の`FileSystem`はGlobalなProject Pathと`std::filesystem::path`を所有し、再帰ScanがResource Loadを
副作用として実行する。`AssetBrowser`はUIから`AddFile`を直接呼び、Copyは既存Destinationの上書きを許可し、処理失敗時に
作成先を直接削除する。この構造は簡潔だが、Root境界、Operation所有権、部分失敗、Headless Test、Asset Pipeline分離を
M13の要件として保証できない。

CueEngineはFolder Tree／File ListとDrag操作の理解しやすさだけを参考にし、Global State、絶対Path公開、UIからの
Filesystem直接操作、ScanとResource Loadの結合、暗黙上書きは採用しない。TheatriaEngineのSource Codeは比較のために
読み取り、CueEngineへコピー、移植、部分抽出しない。

References:

- [TheatriaEngine FileSystem](https://github.com/tochouseito/TheatriaEngine/blob/master/project/Cho/Platform/FileSystem/FileSystem.h)
- [TheatriaEngine AssetBrowser](https://github.com/tochouseito/TheatriaEngine/blob/master/project/Cho/Editor/AssetBrowser/AssetBrowser.cpp)

## Current Requirements

- Project Root外、絶対Path、Drive-relative Path、UNC、Reparse Point経由の脱出を拒否する
- UI、Project Hub、Build ToolへNative Filesystem APIと任意Path Mutationを公開しない
- 同じProject File操作契約をHeadless Testと将来の別Presentationから利用できるようにする
- Create、Rename、Move、Copyで既存Destinationを暗黙上書きしない
- Deleteを即時永久削除にせず、Project単位で回復可能にする
- File操作と開いている`EditorDocument`の寿命を競合させない
- 外部変更通知の欠落、重複、順序変更、Overflowを正しい状態と誤認しない
- Source AssetとRuntime Asset、Generated、Saved、User Workspaceの役割を維持する
- M13でAsset Database、Import／Cook、Path参照書換えを先取りしない
- 新規Third-party Libraryまたは外部Source Codeを導入しない

## Reference Engine and SOL-AVES Comparison

| Reference | 参考にする点 | CueEngineでそのまま採用しない点 |
| --- | --- | --- |
| Unity | Project WindowへFile操作を集約し、削除をOS Trashへ移す回復可能な操作として提供する | `AssetDatabase`、`.meta`、GUID更新、ImporterをM13へ導入せず、Project内Trashと既存Stable Identity方針を使用する |
| Unreal Engine | Content BrowserでMove／Copy先とDelete対象を明示し、参照中Assetを操作前に診断する | `.uasset`、Package Path、Redirector、Reference Fix-up、Force DeleteをFile Serviceへ取り込まない |
| Godot | FileSystem Dock内でMove／Rename／Deleteを行い、外部変更をScanして再確認する | `res://` PathをAssetの恒久Identityにせず、Watcher EventだけでModelを直接更新しない |
| TheatriaEngine | Folder Tree、File Grid、Drop操作の短い導線 | Global Project Path、UI直結File操作、上書きCopy、Scan時Resource Loadを採用しない |
| SOL-AVES | File操作をMetadata、Build、Cache更新と分離し、Iteration待ち時間と派生状態再構築を独立した設計課題として扱う | 公開資料から非公開ABI、Database Schema、Hash Layout、分散Cache実装を推測せず、Asset PipelineをM13へ前倒ししない |

References:

- [Unity `AssetDatabase.MoveAssetToTrash`](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/AssetDatabase.MoveAssetToTrash.html)
- [Unreal Engine Working with Assets](https://dev.epicgames.com/documentation/en-us/unreal-engine/working-with-assets-in-unreal-engine)
- [Godot File system](https://docs.godotengine.org/en/stable/tutorials/scripting/filesystem.html)
- [CEDiL: 「SOL-AVES」の効率的なイテレーションを実現するアセットパイプライン](https://cedil.cesa.or.jp/cedil_sessions/view/3389)

CueEngineは、Editor内に安全な操作入口を集約するUsability、操作前の診断、外部変更後の再Scan、派生状態を再構築可能に
保つ考え方を取り入れる。一方、各EngineのAsset Object、Metadata Format、Importer、Redirector、Package、Databaseは
M13のFile操作契約へ含めない。

## Decision

### Terminology and Source of Truth

本ADRの`Project File Workspace`は、`CueProject.json`を含むProject Root配下の物理Entryと、その操作Sessionを指す。
ADR-0013の`User Workspace`はRecent Registry、個人設定、Cache Locatorを保存するMachine／User固有領域であり、別の正本である。
名前が近くても、Project FileをUser Workspaceへ保存せず、User WorkspaceのEntryをFiles UIへ混入させない。

物理FilesystemをProject Fileの正本とする。Directory Snapshot、Files ViewModel、Watcher Event、検索Index、将来のAsset Databaseは
派生状態であり、Filesystemへ存在しないEntryを成功済みとして保持しない。ただし、Scene本文の意味とAuthoring Dataの正本は
引き続き`SceneDocument`と保存済みScene Formatが所有し、File ServiceがSceneをParseまたは書換えない。

Pathは現在位置を表すLocatorであり、AssetまたはSceneの恒久Identityではない。M13はMove／Rename時のAsset参照書換え、
Asset ID生成、Import状態更新を実行しない。

### Module Boundary

責務を次のTargetへ分ける。

| Target | Responsibility | Public Dependency |
| --- | --- | --- |
| `Cue.IO` | Root-bound列挙とMutationのPortable Primitive、Snapshot型、Operation結果、Watcher Interface | `Cue.Foundation` |
| `Cue.IO.Windows` | Win32列挙、Reparse検査、同一Volume Rename、Staging Copy、`ReadDirectoryChangesW` Adapter | `Cue.IO`, private `Cue.Foundation.Windows` |
| `Cue.ProjectFiles` | Project DescriptorからのArea Policy、Operation Orchestration、Trash Record、Rollback／Reconciliation | `Cue.Foundation`, `Cue.IO`, `Cue.Project` |
| `Cue.EditorCore` | Files ViewModel、Selection、Open Document Guard、Semantic Intent、Operation進行状態 | `Cue.ProjectFiles`と既存依存 |
| `Cue.Editor.ImGui` | 不変ViewModel描画とSemantic Intent生成 | `Cue.EditorCore`, `Cue.ImGui.Core` |
| `Cue.Platform` | Native File／Folder DialogのPlatform非依存Request／Result境界 | `Cue.Foundation` |
| `Cue.Platform.Windows` | COM File Dialog、Owner Window、UTF-8／UTF-16変換 | `Cue.Platform`, private `Cue.Foundation.Windows` |

既存`FilesystemRoot`はAtomic Storage、Scene Persistence、Project生成で使用されているため、M13の全操作をPure Virtual関数として
追加しない。`Cue.IO`に別の`WorkspaceFilesystem` Interfaceを追加し、Directory列挙、Rename／Move／Copy、Trash用Rename、
Watcher生成に必要な低レベル機能を提供する。これにより既存のStorage Test Doubleへ未使用のWorkspace操作を強制しない。

`WorkspaceFilesystem`はProjectの意味、Root Role、UI、Open Document、Trash保持Policyを知らない。`Cue.ProjectFiles`の
`ProjectFileService`がProject固有Policyを適用し、低レベル操作を組み合わせる。Project Hub、Build Tool、Editorは
`WorkspaceFilesystem`を直接使用せず、用途ごとに構成された`ProjectFileService`を使用する。

`Cue.Editor.ImGui`は`std::filesystem`、Win32 File API、`FilesystemRoot`、`WorkspaceFilesystem`へ依存しない。
Native Dialogを含むPlatform接続は最終ExecutableのComposition Rootが所有する。

### ProjectFileService Ownership

一つの`ProjectFileService`は次を一意所有する。

- 一つのProject RootへBinding済みの`WorkspaceFilesystem`
- 検証済み`ProjectId`と`ProjectRoots`のSnapshot
- 用途別の不変`ProjectFileAccessPolicy`
- Collision-resistantな128-bit Operation ID発行状態
- 一つの進行中MutationまたはReconciliation状態
- Project内Trash Catalogの派生Snapshot

ServiceはCopy不可、移動可とし、Global Singletonへ置かない。Project RootのNative HandleとRoot Identityは
`WorkspaceFilesystem`が所有し、Serviceより長く別Objectが所有する非所有Pointerへ依存しない。Project Open時にDescriptorと
Rootを再検証してからServiceを公開し、途中失敗では部分Serviceを公開しない。

Scene保存用`FilesystemRoot`とProject File用`WorkspaceFilesystem`は同じ物理Projectへ別のRoot Handleを持ってよい。
M13のEditor Compositionは`ProjectFileService`と`ProjectWorkspaceSession`を同じProject Sessionが所有し、どちらか一方だけが
先に破棄された状態でUI Intentを受理しない。

### Area Access Policy

`ProjectFileAccessPolicy`は、読取り可能Area、Mutation可能Area、内部操作専用Area、保護Entryを検証済み
`RelativePath`で保持する。PolicyはCompositionまたはProject Moduleだけが作成し、ImGui入力や未検証JSONから任意に昇格しない。

M13 Editorの初期Policyは次とする。

| Area | Files UI | Mutation | Purpose |
| --- | --- | --- | --- |
| `roots.sourceAssets` | 表示する | 許可する | Sceneと編集用Source File |
| `CueProject.json` | 表示しない | 禁止する | `Cue.Project`だけが更新する共有Descriptor |
| `roots.runtimeAssets` | 表示しない | 禁止する | 将来のImport／Cook出力 |
| `roots.generated` | 表示しない | 禁止する | 再生成可能Build Data |
| `roots.saved` | 表示しない | 通常操作は禁止する | Recovery、Log、Project Trash |
| User Workspace／Cache | 表示しない | 禁止する | Machine／User固有状態 |

`<roots.saved>/Editor/Trash`は`ProjectFileService`のDelete／Restore内部操作だけが利用でき、Files UIから通常のCreate、Move、Copy、
Rename、Delete対象にできない。Project Root自身、Source Assets Root自身、各Root RoleのRoot、`CueProject.json`は保護Entryとする。

将来のProject HubまたはBuild Toolは、必要なIssueとADRで専用Policyを追加できる。Editor Policyを再利用してGeneratedやRuntime Assetsへ
書き込まず、UIからPolicyを拡張しない。

### Path and Entry Contract

M13の操作可能LocatorはADR-0014の`RelativePath`を継続利用する。`/`区切り、ASCII Portable Segment、予約Device名拒否、
1 Segment 64 byte、全体255 byte、16 Segmentの上限を変更しない。絶対Path、`..`、Drive Prefix、UNC、Alternate Data Stream、
空Pathを受理しない。

Files UIが保持するPathはSource Assets Area基準の相対Locatorとし、`ProjectFileService`だけがDescriptorのRoot Pathと合成する。
Area Rootと利用者Locatorにはそれぞれ`RelativePath`の上限を適用するが、合成後の内部Pathへ16 Segment／255 byteの利用者向け上限を
再適用しない。再適用すると、有効なArea Rootが深いほど利用者に残るPath長が減り、16 SegmentのRootでは直下Entryも表現できないためである。

`WorkspaceFilesystem`は、検証済みArea RootとArea基準`RelativePath`からだけ不透明な`BoundWorkspacePath`を生成する。
`BoundWorkspacePath`は文字列から直接Parseできず、合成Segment、Area境界、Portable Comparison Keyを所有する。生成時は全Segmentの
Portable規則、Area Root外へ解決されないこと、Project RootのNative Identity、Host Path上限を検証する。Host Path上限超過は
`CapacityExceeded`とし、利用者LocatorのFormat Errorへ丸めない。UIへ`BoundWorkspacePath`またはProject Rootの絶対Pathを
Mutation Keyとして返さない。

Portable Comparison KeyはASCII lowercaseとし、Destination衝突、同一Path、親子循環、Case AliasをHostに依存せず判定する。
Case-only Renameは、SourceとDestinationのComparison Keyが一致し、Source Native Identityが同じで、要求された最終Spellingを
Windows Adapterが検証できる場合だけ許可する。通常の同一Source／Destination操作はNo-op成功にせず`PreconditionFailed`とする。

実Filesystemには`.`開始Entry、Space、非ASCII名等、M13の`RelativePath`で操作できない名前が存在し得る。列挙はその存在を
隠さず、`UnsupportedEntry`と表示用UTF-8名または変換失敗診断を返すが、操作可能`RelativePath`を生成しない。UIは読取り専用の
Unsupported Entryとして表示できるが、Traversal、Rename、Move、Copy、Deleteへ渡せない。

### Directory Listing and Search

`WorkspaceFilesystem`はDirectory直下を一回のSnapshotとして列挙する。Snapshot Entryは少なくとも次を持つ。

- 親Snapshot Generation
- 表示用Name
- 操作可能な場合だけ存在する検証済みRelative Locator
- `RegularFile`、`Directory`、`UnsupportedEntry`の種別
- Regular FileのByte Size
- 操作可否と拒否理由

最終更新時刻は表示Hintとして追加できるが、Identity、Sortの唯一のKey、外部変更判定に使用しない。Content Digestは列挙時に
全Fileへ計算せず、保存競合または明示検証が必要なFileだけ`fingerprint_file`で取得する。

順序はDirectory、Regular File、Unsupported Entryの順とする。操作可能EntryはPortable Comparison Key、同一Keyでは元UTF-8
Byte列の順で並べる。操作不能Entryは変換できたUTF-8 Byte列、変換不能時は安定したNative Name Byte列で並べる。
FilterとSearchはこの正規化済みSnapshotへ適用し、Native列挙順を公開しない。

全列挙と再帰検索は、最大Depth、訪問Entry数、返却結果数、合計Metadata Byte数を持つ非Zeroの`TraversalLimits`を必須入力とする。
Platform AdapterはCaller Limitに加えて実装Hard Limitを持ち、どちらかの超過を部分的な完全結果として返さず、
`CapacityExceeded`と再Scan可能な診断を返す。Reparse PointとUnsupported Directoryは結果へ含めてもTraversalしない。

列挙中のEntry消失、Access Denied、Type変更は、存在しなかったことに丸めない。完全集合が得られない場合はSnapshotを
`Complete`とせず、取得済みEntryとEntry単位診断、`RescanRequired`を同じ結果で返す。

### Mutation Plan and Single-writer Rule

全Mutationは`ProjectFileService`が次の順序で処理する。

1. Semantic Requestを検証し、Operation IDを発行する
2. Source／DestinationをArea Rootへ合成し、保護Entry、同一Path、親子循環、Case Collisionを検証する
3. Open Document Guard、Entry Type、Reparse Point、Traversal Limit、Destination不存在を検証する
4. 不変`ProjectFileOperationPlan`を構築する
5. Owner Thread上で低レベルNative操作を実行する
6. Source／Destinationの事後状態を再Queryする
7. `WorkspaceChangeSet`またはReconciliation要求を公開する
8. 影響Directoryを再列挙し、新しいSnapshot Generationを発行する

PlanはOperation Kind、Operation ID、Source、Destination、期待Entry Type、期待FingerprintまたはNative Identity、Limit、
Rollback対象を保持する。任意Shell Command、未検証絶対Path、UI Pointer、ImGui ID、Runtime Objectを保持しない。

一つの`ProjectFileService`ではMutationを一件だけ実行し、実行中の別Mutationを`Busy`として拒否する。M13は同一Projectを開く
複数Editor Process間の完全なTransactionを保証しない。Native Create-new／Rename結果、事前・事後Query、既存FileのFingerprintで
競合を診断し、非協調Processによる変更を上書きしない。この残余RiskはM13 Completion Gateへ記録する。

### Create Contract

Directory作成は未存在のPortable Segmentだけを親から作成し、既存Directoryは明示されたCreate-or-open Requestでだけ再利用できる。
Files UIの`Create Folder`はCreate-newであり、同名Directoryも`AlreadyExists`として拒否する。

File作成は初期Byte列を同じ親DirectoryのOperation-owned Temporary Fileへ完全書込み、Flush、検証した後、Destination不存在を
Native Publishで確定する。空Fileも同じAtomic Create-new契約を使用する。既存File、Directory、Unsupported Entryを上書きしない。
Publish前失敗ではDestinationを作成せず、Operation所有TemporaryだけをCleanupする。

### Rename and Move Contract

Renameは同じ親Directory、Moveは同じProject Root内の別DirectoryをDestinationとする。どちらもSourceを一度の同一Volume Native
Renameで公開し、Copy-and-deleteへFallbackしない。DestinationはMissingでなければ失敗する。Source EntryがReparse Point、
Unsupported Entry、保護Entry、開いているDocument、またはMutation Area外の場合は開始しない。

DirectoryのDestinationがSource自身またはSource子孫の場合を拒否する。Directory配下は実行前に`TraversalLimits`内で検査し、
Reparse Pointまたは操作不能Entryを含む場合は移動しない。Rename成功後のDurability結果が不明な場合は元へ戻そうとせず、
Source／Destinationを再Queryして`CommittedButDurabilityUnknown`または`ReconciliationRequired`を返す。

M13ではScene FileまたはDirectoryのPath変更に伴うScene内参照、Project Default Scene、Asset参照を書換えない。参照更新が必要な
Asset Identity導入後は、File MoveとMetadata Transactionを別Research Issueで決定する。

### Copy Contract

File Copyは事前検査でSource Native IdentityとTypeをPlanへ固定し、SourceをRead Data／Read Attributes、`FILE_SHARE_READ`だけ、
`FILE_FLAG_OPEN_REPARSE_POINT`で開く`SingleFileCopyGuard`を取得する。読取り開始前にGuard HandleのIdentity、Type、Reparse Point不存在を
Planと再照合し、同じHandleからDestinationのSibling TemporaryへCreate-newでCopyする。Source FingerprintをCopy前後で照合し、TemporaryのSizeとFingerprintも
Sourceに一致する場合だけ、Guardを保持したままDestinationへ公開する。Directory Copyは全Regular FileをRead専用かつ
Write／Delete非共有Handleで固定し、全DirectoryのOplock Breakを監視する`DirectoryCopySourceGuard`を取得する。Source Rootと全Childは
`FILE_FLAG_OPEN_REPARSE_POINT`相当で開き、読取り前に事前ManifestのNative Identity、Type、Reparse Point不存在を再照合する。Guard取得後のSource
Manifestを基準に、同じFile HandleからDestinationのSiblingにあるOperation-owned Staging Directoryへ全EntryをLimit内でCopy、Flush、
再列挙する。Source Manifestの再照合、Oplock Break不存在、Staging Manifestの完全一致を確認してから、Guardを保持したまま一度の
同一Volume Renameで公開する。

途中Dataを最終Destination名で見せず、既存DestinationをMergeまたは上書きしない。Cross-volume Copy、Project外Copy、Hard Link作成、
Symbolic Link複製へFallbackしない。失敗時はOperation所有Temporary／StagingだけをRollbackし、Sourceと既存Destinationを変更しない。
Rollback失敗はPrimary Errorを上書きせずSecondary Diagnosticsへ追加する。
Copyにも`TraversalLimits`と`ContentVerificationLimits`のCaller LimitおよびAdapter Hard Limitを適用する。Guard取得不能、Sourceの
Fingerprint／Manifest変化、Oplock Break、上限超過ではDestinationを公開せず、`Busy`、`RescanRequired`、または`CapacityExceeded`を返す。

### Open Document Guard

`Cue.EditorCore`は、開いている`EditorDocument`のScene Locatorを`ProjectFileService`へ渡す非所有Guardを提供する。
Rename、Move、DeleteのSourceが開いているScene File、またはそれを含むDirectoryの場合は`InUse`として拒否する。
開いているDocumentのLocator、History、Recovery RecordをFile操作に合わせて暗黙変更しない。

Copyと新規作成は既存Documentを移動しないため許可できるが、Destination衝突と初回保存先競合は通常どおり拒否する。
Userは対象SceneをSaveまたはDiscardして閉じた後にPath Mutationを再実行する。Documentを開いたままRenameするWorkflowは、
Document Locator、Scene Identity、Recovery、Undo Historyを一つのTransactionで更新する別Issueまで対象外とする。

### Project-local Trash and Recovery

Deleteは永久削除ではなく、同じProject Rootの`<roots.saved>/Editor/Trash/<OperationId>`へEntryをMoveする。
OS Recycle BinはProject移動、Headless Test、復元Metadata、同一Volume保証を統一できないためM13では使用しない。

一つのTrash Operation Directoryは次を持つ。

```text
<roots.saved>/Editor/Trash/<OperationId>/
    Record.cuetrash
    Payload
```

`Record.cuetrash`はUTF-8 JSONのVersion付きRecordとする。初期`schemaVersion`はJSON Numberの符号なし10進整数`1`へ固定する。
Readerは`0`、小数、指数表記、負数、`uint32_t`範囲外、`1`以外の値を現行Schemaとして受理しない。少なくとも次を保持する。

- `schemaVersion`: 初期値`1`
- `projectId`
- `operationId`
- `state`: `prepared`、`trashed`、`restoring`、`restored`
- Project Root基準の`originalPath`
- `entryType`
- Fileの場合は削除開始時のByte SizeとContent Digest
- Directoryの場合は、Payload Root基準Path、Entry Type、File Byte Size、Content Digestを持つBounded Manifest

`schemaVersion` 1のContent DigestはFNV-1a 64-bitへ固定する。Offset Basisは`14695981039346656037`、Primeは
`1099511628211`とし、File Byteを先頭から符号なし8-bit値として処理する。JSONの`contentDigest`は`0x` Prefixを持たない正確に16文字の
lowercase hexadecimal Stringとし、JSON Number、大文字、桁不足、桁超過、非16進文字を拒否する。Algorithm、Bit幅、Encodingの変更は
`schemaVersion` Migrationなしに行わない。

FileとDirectory ManifestのByte Size Field名は`byteSize`へ固定し、`0`から`18446744073709551615`までの`uint64_t`値をJSON Stringの
符号なし10進整数で保存する。`0`以外の先頭Zero、符号、空文字、小数点、指数表記、Whitespace、範囲外を拒否する。JSON Numberを
浮動小数点へ変換して受理せず、表現変更は`schemaVersion` Migrationの対象とする。

Directory ManifestはRoot自身を含まず、全ての通常DirectoryとRegular FileをPortable Comparison Key、Entry Type、元UTF-8 Byte列の順で
並べる。Directory EntryにSizeとDigestを持たせず、Regular File Entryだけに両方を必須とする。作成時と読込み時に
`TraversalLimits`のDepth、Entry数、Metadata Byte数を適用し、重複Path、Case Collision、欠落Parent、Reparse Point、
Unsupported Entry、上限超過を拒否する。ManifestはDelete後のPayload検証とRestore直前の内容照合に使用する。

単一FileのFingerprintとDirectory ManifestのDigest計算には、非ZeroのFile単位最大Content Byte数とOperation合計最大Content Byte数を
持つ`ContentVerificationLimits`も必須とする。Adapter Hard LimitとCaller Limitの小さい方を適用し、Logical File Sizeを
Digest読込み前に検査する。単一FileはFile単位上限、DirectoryはFile単位と合計の両方を適用する。上限超過は
`CapacityExceeded`とし、SourceまたはPayloadをMoveせず、部分Fingerprintまたは部分Manifestを完全結果として保存しない。
Sparse FileもLogical Sizeで判定し、CallerがAdapter Hard Limitを拡張できないようにする。

`OperationId`はProject File操作ごとに新しく生成するlowercase UUID Version 4とし、Process再起動後もDirectory名とRecordを
一意に対応させる。nil UUID、Version／Variant不正、Directory名とRecord値の不一致を拒否する。

未知Version、Project ID不一致、Operation ID不一致、Path不正、重複Field、Resource Limit超過を推測して読まず、Entryを自動削除しない。
既知の将来Migrationは`N -> N + 1`をMemory上で完了して再検証し、明示更新まで元Recordを維持する。

回復可能Delete／Restoreで単一Fileを扱う場合はNative Link Countが1であることを要求し、Fingerprint検証開始からNative Rename、
移動先でのFingerprint再検証、最終RecordのAtomic Commit完了まで`SingleFileMutationGuard`を保持する。Windows AdapterはSourceを
Read Data／Read Attributes／`DELETE` Access、共有Modeは`FILE_SHARE_READ`だけ、
`FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH`で一度だけ開く。FingerprintとRenameは同じHandleを使うため、排他解除後の
Path基準RenameへFallbackしない。DirectoryではManifest列挙対象の全Regular FileについてLink Count 1を要求する。複数Hard Link、
共有違反、列挙競合を`UnsupportedEntry`、`Busy`、または`RescanRequired`として拒否し、復元不能と判明しているEntryを回復可能Deleteとして
受理しない。単一FileのGuard取得後にSource IdentityとLink Countを再確認し、別Entryへの差替えまたは不一致ではFingerprintを取得せず
`RescanRequired`とする。

Directory DeleteとRestoreは、検証開始からNative Rename、移動先でのManifest再検証、最終Record Commitまで
`DirectoryTreeMutationGuard`を保持する。Windows AdapterのGuardはSource Root Directoryを`DELETE` Accessと
`FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH`で開き、Native Renameにも同じHandleを使用する。全Regular FileはRead専用かつ
Write非共有、Delete共有Handleで固定し、全Directory HandleへRまたはRH Directory Oplockを要求してBreakを監視する。ChildのDelete共有は
Source Root Handleによる親Directory Renameを妨げないために必要であり、外部のChild Rename／DeleteはDirectory Oplock Breakと移動先の
Manifest再検証で検出する。既存Writer、Guard非対応Filesystem、Oplock取得失敗では`Busy`または`UnsupportedEntry`としてMoveを開始しない。
Directory Oplockは内容変更を阻止しない通知契約であるため、Guard期間中のBreakを失敗として記録し、移動先でLink CountとManifestを
再検証する。Move前のBreakは`NotCommitted`、Move後のBreakまたは不一致はRecordとPayloadを維持した
`ReconciliationRequired`とする。黙って成功へFallbackしない。

Delete順序を固定する。

1. Source、Area、Open Document、Destination Trash不存在、Link Countを検証し、単一Fileの排他または
   `DirectoryTreeMutationGuard`を取得してからFile FingerprintまたはDirectory Manifestを取得する
2. Operation DirectoryをCreate-newで作成する
3. `prepared` RecordをAtomic Publishする
4. Sourceを`Payload`へ同一Volume Renameする
5. Source不存在、Payload存在、Type、Link Count、FingerprintまたはManifest一致を再検証する
6. Recordを`trashed`へAtomic更新する
7. Trash CatalogとSource親Directoryを再列挙する

Step 4より前の失敗ではSourceを維持し、Operation-owned Trash DirectoryだけをRollbackできる。Step 4以後はSourceを自動的に
元へ戻そうとせず、事後状態とRecordを保持してReconciliationへ移る。Process終了等で`prepared`が残った場合は次の規則で再判定する。

| Source | Payload | Reconciliation |
| --- | --- | --- |
| Exists | Missing | Delete未CommitとしてRecordと空Operation DirectoryをCleanup可能にする |
| Missing | Exists | Type、Link Count、FingerprintまたはManifestをStep 5と同じ上限で再検証し、一致時だけ`trashed`へ昇格できる |
| Exists | Exists | 外部競合として両方を維持し、User判断を要求する |
| Missing | Missing | Data所在不明としてRecordを維持し、Errorを報告する |

`Missing / Exists`の再検証前に、単一Fileは`SingleFileMutationGuard`、Directoryは`DirectoryTreeMutationGuard`を取得し、
検証から`trashed` RecordのAtomic Commit完了まで保持する。排他取得、再検証、または上限確認が失敗した場合は
`trashed`へ昇格せず、RecordとPayloadを維持して`ReconciliationRequired`を報告する。

RestoreはRecordの`originalPath`を使用し、DestinationがMissingである場合だけ`Payload`を同一Volume Renameで戻す。必要な親Directoryは
Mutation Area内の通常DirectoryだけをCreate-or-openできる。File／Unsupported Entry衝突、Area外、Root変更、Project ID不一致では
復元しない。

File PayloadはRestore直前に`SingleFileMutationGuard`を取得して`restored` RecordのAtomic Commit完了まで保持し、同じHandleから
RecordのByte SizeとContent Digestへ一致する`RegularFile`であることを検証する。WindowsではLink Countが1であることも要求し、
別名Hard Linkまたは共有違反を
`UnsupportedEntry`または`Busy`として拒否する。Fingerprint不一致ではDestinationへMoveせず、RecordとPayloadを維持して
`RecoveryRequired`を返す。Directory Payloadは`TraversalLimits`内で再列挙し、全Regular FileのLink Count 1を確認して、Recordの
Bounded ManifestとPath、Type、Byte Size、Content Digestを完全一致させる。Reparse Point、操作不能Entry、Type不一致、Manifestの
追加・欠落・Fingerprint不一致、複数Hard Linkを含む場合はDestinationへMoveせず、RecordとPayloadを維持して
`RecoveryRequired`を返す。

Directory Payloadの検証は`DirectoryTreeMutationGuard`取得後に行い、Guardを保持したままRecordを`restoring`へ更新してPayloadを
Moveする。Move後はOriginal Path側でLink CountとManifestを再検証し、Guard Breakがなく完全一致する場合だけ`restored`へ更新する。
単一Fileも上記検証後にRecordを`restoring`へ更新し、`SingleFileMutationGuard`のHandleでPayloadをMoveする。Move後にSource存在、
Payload不存在、Fingerprint一致を検証して`restored`へ更新する。
途中終了は同じ存在行列でReconciliationする。既存Destinationを上書きしない。

M13はRecoverable Payloadの自動期限削除と永久削除UIを提供しない。成功済みRestoreの空Operation DirectoryとRecordだけを
Operation-owned Cleanupとして削除できる。未Restore PayloadはProjectが存在する限り保持し、件数と概算Sizeを診断へ出す。
保持期限、容量上限を伴う自動Purge、OS Recycle Bin統合は別Research Issueで決定する。

### Operation Outcome and Error Contract

`ProjectFileOperationResult`はOperation ID、Kind、Source、Destination、Stage、Outcome、Primary Error、Secondary Diagnostics、
再列挙対象を持つ。Outcomeは次の四つに限定する。

| Outcome | Observable State | Caller Action |
| --- | --- | --- |
| `Committed` | 必要なDurability Barrierが成功し、要求後の状態を再Query済み | Snapshotを更新して通常処理を継続する |
| `NotCommitted` | Sourceと既存Destinationを維持 | Errorを表示し、Operation-owned Cleanupを確認する |
| `CommittedButDurabilityUnknown` | 要求後の状態は見えるが永続化が不明 | 成功表示せず、再検証とUser通知を行う |
| `ReconciliationRequired` | 事前状態か事後状態かを確定できない | 両候補を維持し、再Query／Trash Record照合を要求する |

Project Policy違反は`ProjectFileError`として、少なくとも`InvalidRequest`、`ProtectedEntry`、`InUse`、`Conflict`、
`LimitExceeded`、`RecoveryRequired`を区別する。低レベルの`IoError`とNative CodeはImmediate Causeとして保持し、
UI制御FlowをWin32 Error値へ依存させない。Errorには不要な絶対User Path、Credential、File Contentを含めない。

### Durability Barrier

Namespace Mutationの`Committed`はVisibilityの再Queryだけで判定しない。Windows AdapterはADR-0014と同じく、最終Nameを公開する
Rename／MoveへWrite-through Barrierを適用し、成功後にSource／Destinationを再Queryする。Guardを必要としない操作は
`MoveFileExW(..., MOVEFILE_WRITE_THROUGH)`を使用できる。`SingleFileMutationGuard`または`DirectoryTreeMutationGuard`を保持する操作は、
Guard Handleを`DELETE` Accessと`FILE_FLAG_WRITE_THROUGH`で開き、Destination Parentの検証済みHandleと上書き禁止のRelative Nameを渡す
`SetFileInformationByHandle(..., FileRenameInfo, ...)`で同じHandleをRenameする。別Handleを開く`MoveFileExW`のためにGuardを解放しない。
ただし、`SetFileInformationByHandle`の成功またはHandleの`FILE_FLAG_WRITE_THROUGH`だけをNamespace Metadataの文書化済みDurability Barrierと
みなさない。Guardを保持したRenameはSource不存在とDestination存在を確認できても、別の文書化済みBarrierを実行できないWindows
Adapterでは`CommittedButDurabilityUnknown`とし、`Committed`を返さない。

Windows契約の根拠:

- [Microsoft: SetFileInformationByHandle](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileinformationbyhandle)
- [Microsoft: FILE_RENAME_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info)
- [Microsoft: CREATEFILE2_EXTENDED_PARAMETERS](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/ns-fileapi-createfile2_extended_parameters)

直接作成したDirectoryをBarrier済みとみなさず、Create FolderとRestore用の不足ParentもOperation-owned Sibling Directoryを
Create-newしてWrite-through Renameで一段ずつ公開する。

Operationごとの必須Barrierを次のとおりとする。

| Operation | `Committed`に必要なBarrier |
| --- | --- |
| Create File／Folder | TemporaryまたはSibling Directoryから最終NameへのWrite-through Publish |
| Rename／Move | SourceからDestinationへのWrite-through Rename |
| Copy | 検証済みTemporary／StagingからDestinationへのWrite-through Publish |
| Delete | `prepared` RecordのAtomic Commit、Payload Renameの文書化済みDurability Barrier、`trashed` RecordのAtomic Commit。Barrierを提供できないWindows Handle-based Renameは`CommittedButDurabilityUnknown` |
| Restore | `restoring` RecordのAtomic Commit、Original Path Renameの文書化済みDurability Barrier、`restored` RecordのAtomic Commit。Barrierを提供できないWindows Handle-based Renameは`CommittedButDurabilityUnknown` |

単一段のNative Publishが失敗しても、事後QueryでSource不存在とDestination存在を一意に確認できる場合は
`CommittedButDurabilityUnknown`とする。旧状態と新状態を一意に確定できない場合、またはDelete／RestoreのRecordとPayloadの段階が
一致しない場合は`ReconciliationRequired`とする。Barrier未実行または失敗を`Committed`へ変換せず、Post-query成功だけで
Durability確定としない。

### Workspace Change Set and ViewModel

ServiceがCommitしたMutationは、Watcher到着を待たずにOperation ID付き`WorkspaceChangeSet`を返す。Change SetはCreate、Remove、
Move、Invalidateの意味とProject相対Locatorを持つが、Filesystemの代替正本ではない。Editorは影響Directoryを再列挙し、
新しいSnapshot Generationと一致した後だけViewModelへ反映する。

Files ViewModelはDirectory Snapshot、選択、展開、検索条件、進行中Operation、Error、Trash Catalogを所有する。
Native Handle、Filesystem Pointer、絶対Path、ImGui Widget Stateを所有しない。SelectionはM13ではRelative Locatorでよいが、
Asset IdentityではなくSession-local UI Locatorである。再列挙後にEntryが消失した場合はSelectionを解除する。

ImGui AdapterはViewModelを描画し、Create、Rename、Move、Copy、Delete、Restore、RefreshをSemantic Intentとして返す。
Delete確認には対象Path、Entry数、概算Size、復元可能であることを表示する。永久削除IntentはM13に設けない。

### Native File and Folder Dialog Boundary

Native Dialogは`Cue.Platform`のRequest／Result境界とし、Project Hub、Editor、ImGuiがCOM InterfaceまたはOwner `HWND`を直接扱わない。
RequestはDialog Kind、Filter、Default Extension、初期Location Hint、Owner Tokenを値として持つ。Resultは`Selected`、`Cancelled`、
`Failed`を区別し、選択されたUTF-8 Pathを未検証Locatorとして返す。

Owner TokenはPlatform Hostが発行する不透明値であり、Native Windowより長く保持しない。DialogはOwner Threadで同期表示してよい。
Owner破棄、COM初期化失敗、UTF変換失敗をCancelへ丸めない。

Editor内で選択されたPathは、使用前に`ProjectFileService`がProject Root、Area、Relative Path、Reparse Point、Entry Typeを再検証する。
Project Hubで選択されたPathは、既存`ProjectHubPlatform`と`Cue.Project`がDescriptor、Project ID、Compatibilityを再検証する。
Dialogが返したPathを信頼済みProject Locatorとして直接保存しない。

### External Change Service

`WorkspaceWatcher`はRootと対象AreaにBindingし、Windows AdapterがNative Watch HandleとWorker Threadを所有する。
Worker Threadは正規化前のNative EventをBounded Queueへ追加するだけで、EditorDocument、ViewModel、Project Modelを直接変更しない。
Owner ThreadがEvent BatchをDrainし、影響Directoryを再列挙してから状態を適用する。

Watcher Eventは変更Hintであり、正本または完全なJournalではない。Create、Modify、Delete、Rename Pairを可能な範囲で関連付けるが、
重複、順序変更、Self-generated Event、Rename片側欠落を許容する。Debounce／Coalesce後も最終判断は`WorkspaceFilesystem`の再Queryで行う。

Native Buffer Overflow、Queue Overflow、不正Name、Area外Event、Rename Pair欠落は成功した空Batchにせず`RescanRequired`を返す。
`RescanRequired`では対象Area Snapshotを破棄せずStaleとして表示し、Bounded Full Rescan成功後にGenerationを進める。

外部変更が開いているScene Locatorまたは親Directoryへ影響した場合、Files Coreは`EditorController`へInvalidationを通知する。
ControllerはADR-0018のFingerprintとExternal Conflict規則で判断し、Watcher EventだけでSceneをReload、保存済み化、Closeしない。

Watcher停止はCancel要求、Worker Join、Queue閉鎖、Native Handle解放の順で行い、停止完了後にCallbackまたはEventを公開しない。
Destructorだけへ正しい終了を依存させず、Project Session終了時に明示`stop()`結果を診断する。

### Thread and Lifetime Contract

`ProjectFileService`、Files ViewModel、Operation Plan適用は作成Thread限定とし、暗黙にThread-safeとしない。UI Intent、Mutation、
Snapshot Generation更新はOwner Threadで直列化する。Watcher WorkerはBounded Event Queue以外のService状態へ触れない。

Directory SnapshotとViewModel Snapshotは不変の所有値として返し、次のMutation後も参照Pointerを保持しない。長期保持には
Project相対LocatorとGenerationを使用する。非同期Copy等を将来追加する場合も、Operation IDとSession Generationを照合し、
Session終了後の結果を適用しない。

M13では一つのEditor Process、一つのProject Session、一つのFile Mutationを基準とする。複数Process、Network Share、
Source Control Checkout、Remote FilesystemのTransactionは保証しない。

### Headless Test Boundary

Window、ImGui、D3D12、Renderer、Native Dialogなしで次を検証できるようにする。

- Area Policyと保護Entry
- Relative Path合成、Root脱出、Case Collision、親子循環拒否
- 決定的列挙、Filter、Bounded Search、Unsupported Entry隔離
- Create-newとDestination衝突
- Rename／Move／CopyのStrong FailureとRollback
- Open Document Guard
- Delete Record状態遷移、Process中断後Reconciliation、Restore衝突
- Watcher EventのDebounce、Rename Pair、Overflow、Full Rescan要求
- Files ViewModelのSelection、Progress、Error、Snapshot Generation遷移
- Native Dialog ResultのSelected／Cancelled／Failed解釈

Windows Integration TestではRoot、子Directory、File、Junction、Symbolic Link、Case-only Rename、Share Violation、Access Denied、
File Watch、Buffer Overflow相当、Owner Window付きDialogの境界を検証する。Native Dialogの実操作は手動Test手順を用意し、CIで
Modal UIを自動表示しない。

## Rejected Alternatives

### `FilesystemRoot`へ全M13操作を追加する

Scene保存とProject生成だけを必要とする既存ConsumerとTest Doubleへ、列挙、Trash、Watcher、Directory Copyを強制し、
Interface責務が広がるため採用しない。StorageとWorkspace操作は別Capability Interfaceにする。

### ImGuiまたはProject Hubから`std::filesystem`を直接使う

Root Policy、Reparse Point、Rollback、Headless Test、Platform差がPresentationへ漏れるため採用しない。

### Project Root全体をFiles UIで自由に変更する

`CueProject.json`、Runtime Assets、Generated、Saved、RecoveryをUser操作で破壊できるため採用しない。M13 EditorはSource Assetsだけを
表示・変更し、他Areaは用途別Serviceへ限定する。

### Deleteで永久削除またはOS Recycle Binを使う

Headless Test、Project単位Record、同一Volume Rename、Project移動後の復元、失敗時Reconciliationを統一できないため採用しない。

### Copy先へ直接書く、または既存DirectoryへMergeする

途中失敗で半完成Destinationを公開し、既存Dataとの境界が不明になるため採用しない。

### Watcher EventをViewModelの正本にする

Native Buffer Overflow、重複、Coalesce、外部Process競合で完全なEvent列を保証できないため採用しない。

### M13でPath変更時のAsset参照を修復する

Asset ID、Metadata、Import／Cook、Reference Graphが未決定であり、File操作だけで永続参照の意味を固定するため採用しない。

### TheatriaEngineまたは他EngineのFile Browserを移植する

所有権、Root Policy、Error契約、Asset Model、License、Runtime前提がCueEngineと異なるため採用しない。公開資料とSourceは
問題、制約、操作体験の比較だけに使用する。

## Consequences

### Positive

- UIとToolがNative Filesystemへ到達せず、Project RootとArea Policyを一箇所で強制できる
- 既存Destination、Project Descriptor、Generated、Recoveryを暗黙上書きしない
- Delete後もProject内Recordから復元でき、途中終了を存在行列で診断できる
- File Watchの欠落やOverflowを成功と誤認せず、再列挙で正本へ戻れる
- Existing Scene Storage APIを肥大化させず、Headless Test用のWorkspace Capabilityを分離できる
- Asset Pipelineを前倒しせず、後続のMetadata／Asset ID接続点を明確にできる

### Trade-offs

- Root、Area、Service、ViewModel、Watcherを分離するため型と状態遷移が増える
- Project内TrashがDisk容量を使用し、M13では自動Purgeしない
- 開いているSceneのRename／Moveは一度閉じる必要がある
- ASCII Portable Path規則により、Spaceと非ASCII名はM13で操作不能Entryになる
- Directory CopyとTrash ReconciliationのFailure Injection Test量が増える
- 複数Editor Processと非協調Writerに対する完全Transactionは保証しない

### Mitigations

- Files UIは操作不能理由、Trash件数、Stale Snapshot、Rescan状態を明示する
- CopyとDeleteはOperation ID、Staging、Version付きRecord、事後Queryで復旧可能にする
- Watcher Eventを小さくCoalesceし、影響Directoryだけを通常再列挙する
- Path文字集合拡張が必要になった場合は、Unicode Normalization、Case Folding、Asset Identityを含む別ADRで判断する
- M13 Completion Gateへ未対応の複数Process、永久Cleanup、非ASCII Pathを明記する

## Validation

- Module依存GateとPublic Header単体Compile Test
- Project Root、Area Root、保護Entry、絶対Path、`..`、Reparse Point拒否Test
- Directory／File／Unsupported Entryの決定的Sortと上限Test
- Create／Rename／Move／CopyのDestination衝突、Case-only Rename、親子循環Test
- Copy各Failure StageのSource保持、Destination非公開、Staging Cleanup Test
- Delete／Restore各Failure Stageと四状態存在行列Test
- Open Document配下のRename／Move／Delete拒否Test
- Watcher Create／Modify／Delete／Rename、重複、Overflow、Shutdown Test
- ViewModel Selection消失、Operation進行、Error、Stale／Rescan Test
- Dialog Cancel／Failure分類と選択Path再検証Test
- Debug／Development／Release Build、CTest、`git diff --check`
- Project作成からFiles操作、Delete／Restore、外部変更、Editor再起動までの手動Workflow

## Implementation Sequence

1. Issue #199で`WorkspaceFilesystem`の列挙、Snapshot、Bounded Search、Windows Adapterを実装する
2. Issue #200でProjectFileService、Area Policy、Directory／File Create-newを実装する
3. Issue #201でRename、Move、Staged Copy、Case-only Renameを実装する
4. Issue #202でProject-local Trash Record、Delete、Restore、Reconciliationを実装する
5. Issue #203でNative File／Folder DialogのPortable境界とWindows Adapterを実装する
6. Issue #204でBounded Watcher、External Change Batch、Rescan Requiredを実装する
7. Issue #205でFiles ViewModel、Open Document Guard、Workspace Operation状態をEditor Coreへ接続する
8. Issue #206でImGui Files UIを薄いPresentation Adapterとして接続する
9. Issue #207で3構成Build、CTest、Windows IO／Watcher Test、手動Workflowを検証する

## Follow-up Work

- Asset Pipeline着手時: Asset ID、Metadata、Import／Cook、Path変更時Reference Updateを別ADRで決定する
- 複数Editor対応時: Project-wide Cross-process Operation LeaseとScene Save LeaseのLock順序を決定する
- 非ASCII Path要求時: Unicode Normalization、Case Folding、Portable Name、Migrationを決定する
- Trash容量管理が必要になった時: 明示Purge、保持期限、容量上限、Source Controlとの関係を決定する
- Open Scene Renameが必要になった時: Document Locator、Recovery、History、Project参照を一つのTransactionで更新する
- Network Share／Cloud Placeholder対応時: Reparse Tag Allowlist、Offline状態、Atomicity、Watcher差を決定する

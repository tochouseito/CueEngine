# ADR-0014: Project IO and Atomic Storage Contract

- Status: Accepted
- Date: 2026-09-01
- Decision Owners: CueEngine Project
- Amendment: ADR-0019 adds a Windows Known Folder create-or-open root boundary for the Project Hub workspace

## Context

M09では、Project DescriptorとUser Workspaceを安全に保存し、Blank Projectを途中状態を公開せず作成する必要がある。
文字列連結した絶対Pathを各処理へ渡す設計では、`..`、Drive指定、UNC、大小文字Alias、Windows予約Device名、Reparse Pointを
介したProject Root外への書込みを防げない。また、最終FileまたはDirectoryへ直接書き始めると、Process終了、Disk Full、権限失敗で
半完成Dataが完成扱いの名前に残る。

ADR-0013はProject DescriptorのPortable Root表現と共有／端末固有Data境界を決定した。本ADRは、そのModelが利用する
Platform非依存Path／Filesystem契約、Windows実装境界、Atomic File Replace、Staging Directory公開、Flush、Rollback、Error分類を
決定する。Asset VFS、Network Storage、Watcher、任意Project Folder削除は決定しない。

## Legacy Reference

### Legacy Problem

旧CueEngineもProject Directoryと複数Fileを生成し、既存Projectを選択して開く必要があった。

### Legacy Approach

旧実装はEditorのProject生成処理から最終Directoryを作り、設定FileやGenerated Fileを順番に書き込んだ。Path検査、Directory作成、
JSON出力、Script生成、Default Scene生成は一つの処理に集約されていた。

### Legacy Strengths

- Project作成に必要なDirectoryとFileを一度に用意できた
- 人が確認できる設定Fileを生成できた
- 既存Directoryや不正なProject名を作成前に一部検査できた

### Legacy Problems

- 最終Directoryへ直接書くため、途中失敗で半完成Projectが残り得た
- RootにBindingされたFilesystem境界がなく、任意Pathへの書込みを呼び出し側が防ぐ必要があった
- Reparse Point、予約Device名、大小文字Alias、同一Volume要件が契約化されていなかった
- Flush、Atomic Replace、Rollback、Primary／Secondary Errorの順序が定義されていなかった
- Editor固有処理と再利用可能なIO処理を分離できていなかった

旧コードは問題と制約の確認だけに使用し、新実装へコピー、移植、部分抽出しない。

## Decision

### Module Boundary

新規Targetを次の責務で分ける。

| Target | Responsibility | Public Dependency |
| --- | --- | --- |
| `Cue.IO` | Portable Relative Path、Root-bound Filesystem契約、Storage Operation、Error分類 | `Cue.Foundation` |
| `Cue.IO.Windows` | UTF-16変換、Win32 File API、Reparse検査、Flush、Replace、Known Folder | `Cue.IO`, private `Cue.Foundation.Windows` |

`Cue.Project`は`Cue.IO`へ依存できるが、`Cue.IO.Windows`へ直接依存しない。ApplicationのComposition RootがWindows実装を生成して
Platform非依存Interfaceとして渡す。`Cue.IO`は`Cue.Platform`、`Cue.RHI`、`Cue.Editor`へ依存しない。

公開APIへ`HANDLE`、`HRESULT`、Windows Known Folder ID、`std::filesystem::path`を出さない。Path文字列はUTF-8で所有し、
Windows実装だけがStrict UTF-8／UTF-16変換を行う。新規Third-party Libraryまたは外部Source Codeを導入しない。

Project Hubの初回起動に必要なUser Workspace Rootは、`Cue.IO.Windows`の次のWindows固有境界から生成する。

```cpp
enum class WindowsKnownFolder
{
    LocalApplicationData,
};

enum class WindowsRootOpenMode
{
    OpenExisting,
    CreateOrOpen,
};

[[nodiscard]] Result<std::unique_ptr<FilesystemRoot>> create_windows_known_folder_filesystem_root(
    WindowsKnownFolder a_folder,
    const RelativePath& a_relativeRoot,
    WindowsRootOpenMode a_mode,
    const AssertContext& a_assertContext) noexcept;
```

- `LocalApplicationData`は`FOLDERID_LocalAppData`から解決し、Environment VariableやCurrent Directoryへ依存しない
- Project Hubは検証済み`RelativePath`の`CueEngine/Workspace`と`CreateOrOpen`を指定する
- `CreateOrOpen`はKnown Folder自体を作成せず、Relative Rootの各Segmentだけを順に作成または検証する
- 既存Entryが通常Directoryなら再利用し、Fileなら`IoError::TypeMismatch`、Reparse Pointなら
  `IoError::UnsupportedEntry`を返して先へ進まない
- Known Folder解決、Directory作成、Open失敗はNative Errorを保持したRecoverable Errorとして返す
- 作成後は既存のRoot Identity、Directory、Reparse Point検査を通してから`FilesystemRoot`を公開する
- APIはRoot DirectoryだけをCreate-or-openし、`CueWorkspace.json`の作成とAtomic保存は`Cue.ProjectHub`が行う
- `OpenExisting`と既存`create_windows_filesystem_root()`の契約は変更しない

### Root-bound Filesystem

全てのProject向け操作は、検証済みの絶対Rootから作成した`FilesystemRoot`にBindingする。Binding後の公開操作は
`ProjectRelativePath`だけを受け取り、任意の絶対Path、Current Working Directory、Drive-relative Pathを受け取らない。
`FilesystemRoot`は移動可能、Copy不可とし、Native Root Handleと検証済みRoot IdentityをWindows実装が所有する。

Root Bindingは次を満たす。

1. 入力は有効なUTF-8の絶対Directory Pathとする
2. Root Directoryを開き、最終ObjectがDirectoryでありReparse Pointでないことを確認する
3. Native Handleから最終PathとVolume Identityを取得し、文字列入力ではなく開いたObjectをRoot Identityとする
4. Rootが存在しない場合、明示的なCreate Root操作だけが一段のRoot Directoryを作成できる
5. Root作成時も親DirectoryをBindingし、既存Entryを上書きしない

Rootへ至るAncestorがRedirectされていても、Binding後は開いた最終Directory Objectを境界とする。Root自身またはRoot配下で操作対象となる
既存ComponentがReparse Pointの場合は、Tagの種類やTargetに関係なく拒否する。Symbolic Link、Junction、Mount Point、Cloud Placeholderを
暗黙Followしない。

Windowsの公開File APIだけでは、悪意ある別Processが検査直後にDirectory Entryを差し替える全てのTOCTOUを排除できない。
初期Threat Modelは、入力Dataまたは既存Filesystem構造によるRoot脱出を防ぎ、同一User権限の攻撃者による並行置換をSecurity境界に
含めない。各Componentは可能な限り`FILE_FLAG_OPEN_REPARSE_POINT`で検査し、Publish直前にも再検証する。この残余Riskを隠さない。

### Portable Relative Path

`ProjectRelativePath`はParse時に一度だけ検証し、検証済みSegment列を所有する値型とする。未検証`std::string`から暗黙変換しない。

共通規則はADR-0013のProject Root規則と一致させる。

- `/`だけをSeparatorとして受理する
- ASCII英数字、`_`、`-`、`.`だけをSegment文字として受理する
- 1 Segmentは1文字以上64文字以下、全体は1文字以上255文字以下、Segment数は1以上16以下とする
- 空Segment、`.`、`..`、先頭または末尾の`.`を拒否する
- `\`、Colon、Drive Prefix、UNC、Rooted Path、Alternate Data Stream表現を拒否する
- 最初の`.`より前をASCII case-insensitiveで比較し、`CON`、`PRN`、`AUX`、`NUL`、`COM1`–`COM9`、`LPT1`–`LPT9`を拒否する
- Comparison Keyは各ASCII文字をlowercaseへ変換し、重複、親子、予約名の判定に使用する
- Root Roleの先頭Segmentが`cueproject.json`のComparison Keyと一致する場合を拒否する

Windows実装は結合後のAbsolute PathについてNative APIの長さ上限も検査し、超過をPortable PathのFormat Errorではなく
`CapacityExceeded`として返す。Long Path対応の有無をProcessのCurrent DirectoryやManifestへ暗黙依存させない。

### File and Directory Operations

M09の公開契約は次の最小操作に限定する。

- Entry状態を`Missing`、`RegularFile`、`Directory`、`UnsupportedEntry`で問い合わせる
- Byte列全体を上限付きで読む
- 未存在Directoryを親から順に作成する
- 未存在Fileを排他的に作成する
- 既存FileをAtomicに置換する
- Operation所有のTemporary FileまたはStaging DirectoryだけをRollbackで削除する

存在確認後の別操作に正しさを依存しない。Create、Open、PublishはNative APIの排他結果を最終判定とし、Check-then-actだけで
上書きを防がない。File読取り上限は呼び出し側が明示し、上限超過を部分成功にせずErrorとする。

Directory作成はRoot配下だけで行い、途中で既存Regular FileまたはUnsupported Entryへ遭遇した場合は失敗する。既存Directoryは
再利用できるが、Blank Projectの最終Destinationは既存なら空・非空に関係なく拒否する。
不足DirectoryのNative Createが`AlreadyExists`相当になった場合はEntryを再検査し、Directoryなら競合成功として再利用する。
Regular Fileなら`TypeMismatch`、Reparse Point等のUnsupported Entryなら`UnsupportedEntry`を返す。
`AlreadyExists`はCreate-newまたはPublish先衝突の分類であり、Create-or-open Directoryの型衝突へ丸めない。
再検査が`Missing`へ戻った場合は同じSegmentのNative Createを再試行するが、初回を含めて最大3回に制限する。
3回目の再検査も`Missing`なら`IoError::IoFailure`と競合診断を返し、無制限なBusy Loopにしない。
再検査自体がErrorならそのErrorを保持し、別分類へ丸めない。

### Atomic File Replace

Atomic File Replaceは「同一Machine上の他Readerから、旧Contentまたは新Contentのどちらかだけが最終File名で観測される」ことを保証する。
Process CrashまたはPower Loss後の永続化を完全保証するものではない。

操作順序を固定する。

1. Destinationの親DirectoryをRoot配下で検証し、Reparse Pointを拒否する
2. 同じ親DirectoryにCollision-resistantなTemporary File名を生成し、Create-newで排他的に開く
3. 要求された全Byteを書き、Short Writeを失敗として扱う
4. Temporary FileをFlushする
5. Destinationが既存ならRegular Fileであることを確認し、Atomic Replaceする
6. Destinationが未存在ならCreate-new相当のRenameで公開する
7. Publish成功後に親DirectoryのFlushを試行する

Temporary FileはDestinationと同一Directory、したがって同一Volumeに置く。Cross-volume CopyへFallbackしない。DestinationがDirectory、
Reparse Point、Unsupported Entryの場合は置換しない。Share Violation、Permission Denied、Disk FullをRetry Loopで隠さない。

Step 5または6より前に失敗した場合、元Destinationを変更せずTemporary FileをRollbackする。Publish後の親Directory Flush失敗は、
Visibility上は新Fileが公開済みであるためRollback不能な`PublishedButDurabilityUnknown`として返す。成功と偽らず、元Fileへ戻す試みも行わない。

### Staging Directory Publish

Blank Projectは最終DestinationのSiblingにOperation専用Staging DirectoryをCreate-newで作り、全DirectoryとFileをStaging内へ生成する。
Staging名はPortableな固定Prefix、Random 128-bit Operation Id、再試行Counterから構成し、既存Entryを再利用しない。

Publish手順を固定する。

1. 最終Destinationが存在しないことを確認する
2. Sibling Staging Directoryを排他的に作成する
3. Stagingへ必要なDirectoryとFileを全て生成する
4. 各FileをFlushし、Descriptorを共通Parser／Validatorで再読込する
5. Staging配下にReparse PointまたはUnsupported Entryがないことを再検証する
6. 最終Destinationが引き続き未存在であることをNative Renameで判定し、同一Volume Renameで一度だけ公開する
7. Publish後に親DirectoryのFlushを試行する

既存の空Directoryも上書き、Merge、再利用しない。Cross-volume MoveまたはCopyへFallbackしない。Step 6より前の失敗では
Operationが作成したStagingだけをRollbackする。RollbackはProject Folder削除APIではなく、Operation所有Tokenと一致する未公開Stagingに
限定する。Rollback失敗はPrimary Errorを置換せずSecondary Diagnosticsへ追加する。

Step 6成功後のFlush失敗は`PublishedButDurabilityUnknown`であり、完成Projectは最終名で見える。呼び出し側は成功としてRecent Registryへ
追加せず、再検証とUser向け診断を行う。公開済みDirectoryを自動削除しない。

### Flush and Durability

`Flush`は書込みHandleのBufferをOS／Deviceへ送る要求であり、Storage Hardware、Controller Cache、Filesystem Journalを含む全ての電源断耐性を
保証しない。契約上の区分は次のとおりとする。

| Outcome | Visibility | Durability | Caller Action |
| --- | --- | --- | --- |
| `Committed` | New Data | Flush要求成功 | 通常処理を継続 |
| `NotPublished` | Old DataまたはMissing | Publish前失敗 | Primary Errorを表示、Staging／Temp cleanupを確認 |
| `PublishedButDurabilityUnknown` | New Data | Publish後Flush失敗 | 再読込して診断、成功扱いにしない |

WindowsでDirectory HandleのFlushがFilesystemまたはDeviceによりUnsupportedの場合、実装はその事実を成功へ変換しない。
Portable Error分類`DurabilityUnknown`の失敗として返し、Native ErrorにUnsupported理由を保持する。`DurabilityUnknown`は必ず
Outcome `PublishedButDurabilityUnknown`へ対応し、別のPortable分類名を追加しない。呼び出し側はこの分類からPublish済みと判定する。

### Reparse Point and Symlink Policy

M09ではProject Root配下のReparse Pointを全てUnsupported Entryとして拒否する。Allowlistは設けない。これはCloud同期やJunctionを利用する
Project配置を制限するが、Root脱出防止とPlatform間で一貫した意味を優先する。将来許可する場合は、Tag別Policy、Target Root Binding、
Cycle、Offline Placeholder、TOCTOUを別Research IssueとADRで決定する。

読取りも同じPolicyを使う。書込みだけを拒否してOpen時にFollowすると、検証済みDescriptorと実際のProject内容が異なるためである。

### Error Contract and Diagnostics

`Cue.IO`は少なくとも次のPortable分類を持つ。Native Error Codeは`cue::Error`のNative Diagnosticとして保持するが、公開制御Flowを
Win32 Error値へ依存させない。

| Classification | Meaning |
| --- | --- |
| `InvalidPath` | UTF-8、相対Path、Segment、予約名、長さの契約違反 |
| `OutsideRoot` | Root境界外へ解決される、またはRoot Identityを維持できない |
| `NotFound` | 必須Entryが存在しない |
| `AlreadyExists` | Create-new／Publish先が既に存在する |
| `TypeMismatch` | FileとDirectoryの期待が一致しない |
| `UnsupportedEntry` | Reparse Point等、M09 Policyで扱わないEntry |
| `PermissionDenied` | AccessまたはSharing Policyで拒否された |
| `CapacityExceeded` | File Size上限、Path上限、Disk Full等 |
| `IoFailure` | Read、Write、Flush、Rename、Replace等のその他失敗 |
| `DurabilityUnknown` | Publish後のFlush失敗またはDurability非対応 |

Windows実装では、Directory Handleに対する`FlushFileBuffers`を公開契約として利用しない。Path Identityを事前に固定しない
操作では`MoveFileExW(..., MOVEFILE_WRITE_THROUGH)`を、Reparse Pointを拒否して差替えを阻止したDirectory Handleを既に保持する
操作では`FILE_FLAG_WRITE_THROUGH`付きHandleへの`SetFileInformationByHandle(FileRenameInfo)`を、PublishとDirectory Metadataの
Durability試行を一体化したNative境界として扱う。Microsoftの`CreateFile2`契約は、NTFSでWrite-through Handleから生じるRename等の
Metadata変更も遅延なくFlushすることを明記している。
この呼出しが失敗した場合はSourceとDestinationの事後状態を確認し、Sourceが消失してDestinationが可視化済みなら
Rollbackせず`DurabilityUnknown`を返す。未公開なら通常のPublish失敗としてSourceをCleanupする。

Source: [CREATEFILE2_EXTENDED_PARAMETERS](https://learn.microsoft.com/windows/win32/api/fileapi/ns-fileapi-createfile2_extended_parameters)

ErrorにはOperation名、Root-relative Path、Stage、Portable分類、Native Codeを含める。Credential、User名、不要な絶対Pathを通常Logへ出さない。
Cleanup／Close／Rollback失敗は`append_secondary_diagnostics`でPrimary Errorへ追加し、Primary原因を上書きしない。

公開関数と公開型にはCoding Rulesに従う`/// @brief`を必須とし、所有権、寿命、Thread Safety、失敗時状態を記述する。

### Threading and Ownership

`FilesystemRoot`とStorage Operationは暗黙にThread-safeとしない。同一Instanceへの並行操作は呼び出し側が同期する。別Instance間でも
Native Filesystem競合は起こり得るため、最終結果はCreate-new／Rename／ReplaceのNative排他結果で決める。

Temporary／Stagingの所有権はOperation Objectが保持し、Commit成功または明示Rollbackで終了する。DestructorはBest-effort cleanupを
試行できるが、失敗を報告できないため正しい制御FlowをDestructorだけへ依存させない。

### Failure Injection

Platform非依存Test Doubleは少なくとも次のStageで一度だけ失敗を注入できるようにする。

- Root bind
- Create temporary／staging
- Write before first byte、mid-write、after full write
- File flush
- Validate staged data
- Pre-publish reparse validation
- Replace／rename
- Directory flush after publish
- Rollback remove

Testは各Stageについて、最終Destination、元File、Temporary／Staging、Primary Error、Secondary Diagnosticsを検証する。同じ入力と
Failure Pointから同じPortable分類が得られるようにし、Native CodeそのものをPlatform非依存Testの期待値にしない。

## Rejected Alternatives

### `std::filesystem`を公開APIにする

Native Path表現、Exception Policy、Platform差がProject APIへ漏れるため採用しない。Windows実装内部で限定利用する場合も、Reparse、
Atomic Replace、Flushの正しさをStandard Libraryだけへ委ねない。

### 最終File／Directoryへ直接書く

途中失敗で半完成Dataが完成名に残るため採用しない。

### Reparse PointをCanonical Path文字列だけで許可する

検査後の差替え、Mount Point、Cloud Placeholder、Case Aliasを扱えず、Root境界の意味が不明確になるため採用しない。

### Cross-volume時にCopyへFallbackする

Atomic Visibilityを失い、途中Copyが最終名に残り得るため採用しない。

### Flush成功を完全な電源断耐性と呼ぶ

HardwareとFilesystemが保証しない性質をAPIが約束することになるため採用しない。

### 外部Filesystem／JSON Utilityを導入する

M09の必要範囲に対して新規Dependency、License管理、ABI面積が増えるため採用しない。既存ToolchainのC++ Standard LibraryとWindows SDKだけを
使用し、実装はCueEngineの要件から新規設計する。

## Consequences

### Positive

- Root外書込みをAPI形状とWindows検証の両方で拒否できる
- 既存Projectまたは既存Directoryを暗黙上書きしない
- File保存とBlank Project公開で半完成Dataを最終名から分離できる
- Windows型をProjectの共有APIへ漏らさず将来Host実装を追加できる
- Primary ErrorとRollback失敗を同時に診断できる
- 外部Libraryや外部Source Codeを追加せず実装できる

### Negative

- Reparse Pointを使うProject配置とCloud PlaceholderをM09では扱えない
- Windows公開APIだけでは悪意ある並行差替えのTOCTOUを完全排除できない
- Flush後も全Hardwareでの電源断耐性は保証できない
- First-party実装とFailure Injection Testの量が増える

## Validation

- Project Relative Pathの正常・脱出・予約名・Case Alias・長さ境界Table Test
- Root自身と各子ComponentのReparse Point拒否Test
- Directory Create競合後のDirectory再利用、File／Reparse Point分類、Missing再試行上限Test
- Atomic Replaceの旧Content保持、新Content公開、Temp cleanup Test
- Publish後Flush失敗の`PublishedButDurabilityUnknown` Test
- Staging作成の各Failure Pointに対する半完成最終Directory不存在Test
- 既存空／非空Destination拒否Test
- Rollback失敗がPrimary Errorを保持するTest
- Public Header単体Compile TestとTarget依存Gate
- Debug／Development／Release Build、CTest、`git diff --check`

## Follow-up Work

- Issue #136: `Cue.IO`と`Cue.IO.Windows`、Failure Injection Test Doubleを実装する
- Issue #137: Project Descriptorの読取り／保存を本契約へ接続する
- Issue #138: Blank ProjectをStaging DirectoryからAtomic Publishする
- Issue #139: User Workspace RegistryをAtomic File Replaceで保存する
- Issue #162: Windows `create_directories()`の競合後再検査を実Entry種別へ分類し、Editor Saved RootのCreate-or-openへ使用する

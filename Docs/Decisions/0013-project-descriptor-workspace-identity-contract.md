# ADR-0013: Project Descriptor, Workspace, and Identity Contract

- Status: Accepted
- Date: 2026-09-01
- Decision Owners: CueEngine Project

## Descriptor version 2 amendment

ADR-0023はM16でProject Descriptorをschema version 2へ更新した。version 2は既存の`defaultScene` Memberを
`null`、または`sceneAssetId`と`sourceLocator`を持つObjectとして扱い、Startup Sceneの永続Identityと
`roots.sourceAssets`基準の現在Locatorを分離する。Blank Projectだけが両者の一致するDefault Sceneを生成する。

version 1から2へのMigrationはProject Hubの「Project Formatを更新」確認操作からだけ明示実行する。
Migrationは`defaultScene: null`を維持し、既存Sceneを暗黙選択または生成しない。公開前失敗では元Descriptorを維持し、
Atomic置換後のDirectory Flushだけが失敗した場合は`PublishedButDurabilityUnknown`として、通常の失敗と区別する。
完全なversion 2 Wire契約とPackage時の利用手順の正本はADR-0023とする。

## Context

M09では、CueEngine Projectを空の場所から作成し、検証、登録、再Openできる共通基盤を確立する。
Projectの共有データ、端末固有Workspace、生成物、Cacheを区別しない場合、Projectを別Machineへ移動しただけで
Identityが変わる、個人設定がVersion Controlへ混入する、RuntimeがSource Assetを読む、古い生成物を正本として扱う
などの問題が発生する。

Project FormatのVersionとCueEngineのVersionも別の責務である。Project Formatが読めても現在のEngineまたはHardwareで
実行できない場合があり、逆にEngine Versionが異なっていてもMigration不要で読める場合がある。この二つを一つの整数へ
統合しない。

ADR-0012は、現在MachineのSystem／Graphics Capability SnapshotをProjectへ保存せず、ProjectにはRequirementを保存する
方針を決定した。本ADRはその入力を含むProject共有Model、端末固有Workspace、Stable Identity、Root Role、Version／Migrationの
境界を決定する。Scene Format、Asset Import／Cook、Editor UI、Scripting生成は決定しない。

## Legacy Reference

### Legacy Problem

旧CueEngineも、Projectを作成して必要なDirectoryと設定Fileを生成し、Editorから既存Projectを選択して開く必要があった。

### Legacy Approach

旧実装ではEditorの`ProjectGenerator`がProject Directory、Script向けCMake File、Default Scene、`cueproject.json`を
最終Directoryへ順番に書き込んだ。`ProjectHub`は選択Directoryと`cueproject.json`の存在を確認し、開いたProjectをPathで
呼び出し側へ返した。Descriptorには表示名、単一の`engineVersion`、各種Path、起動SceneのPathを保存していた。

### Legacy Strengths

- 新規Project作成と既存Project選択を小さいUIから実行できた
- Source Asset、Script、Saved、Intermediateの初期Directoryを生成できた
- Project名と最低限のDirectory／File存在を作成前後に検査できた
- 人が確認できるJSON形式でProject設定を保存した

### Legacy Problems

- Stable ProjectIdがなく、Project Pathが事実上のIdentityになっていた
- Format VersionとEngine互換Versionが分離されていなかった
- 起動SceneをPathだけで参照しており、Asset移動に弱かった
- Project生成途中の失敗で、半完成Directoryが最終位置に残り得た
- Project Model、Validation、UI、Script Build File生成、Default Scene生成の責務が一つのEditor実装へ集約されていた
- Projectを開く処理はDescriptor内容を共通Parserで検証せず、Fileの存在だけを確認していた
- Recent Project、Pin、Missing、Moved、端末固有Engine Pathを共有Descriptorから分離する契約がなかった
- Generated、Cache、Savedの再生成可能性と正本性が明記されていなかった

### Current Requirements

- Pathまたは表示名ではなくStable ProjectIdでProjectを識別する
- Project共有データへMachine、User、Editor Session固有値を保存しない
- Project Format VersionとEngine互換Versionを分離する
- Asset参照をPathだけで永続化しない
- Source Asset、Runtime Asset、Generated、Cache、Savedの役割を分ける
- Editor、Runtime、Toolsが同じProject Model、Parser、Validatorを使用する
- 現在MachineのCapability SnapshotではなくRequired Capabilityを保存する
- Project移動、重複登録、欠損、Migration要求を診断可能にする
- 将来の非Windows Hostでも共有Descriptorを同じ意味で解釈できるようにする

## Reference Engine Comparison

| Engine | 参考にする点 | CueEngineでそのまま採用しない点 |
| --- | --- | --- |
| Unreal Engine | `.uproject`をProjectの入口とし、Content／Configと、再生成可能なIntermediate／Savedを区別する | File名やDirectory Pathを恒久Identityにせず、ProjectIdとWorkspace Registryを別に持つ |
| Unity | Authoring Sourceを`Assets`、Project共有設定を`ProjectSettings`、再生成可能な内部表現を`Library`へ分ける | Library相当を共有正本にせず、Source AssetとRuntime Assetも明示的に分ける |
| Godot | `project.godot`でProject Rootを確定し、Project相対の`res://`とUser領域の`user://`を分ける | Path参照はAsset移動で壊れるため、永続Asset参照をProject相対Pathだけにしない |

Sources:

- [Unreal Engine Directory Structure](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-directory-structure)
- [Unity Introduction to importing assets](https://docs.unity3d.com/6000.2/Documentation/Manual/ImportingAssets.html)
- [Godot File system](https://docs.godotengine.org/en/stable/tutorials/scripting/filesystem.html)

CueEngineは、単一のProject入口とProject相対RootのUsability、共有Sourceと再生成可能Dataの分離を取り入れる。
一方、ProjectとAssetのIdentityをPathへ結合せず、Project共有Descriptor、User Workspace、Runtime向けDataを独立した契約にする。

## Decision

### Project Root and Descriptor

Project Rootは`CueProject.json`を直接含むDirectoryとする。`CueProject.json`はUTF-8 JSONで保存し、BOMを受理しない。
File名はProject探索の入口であり、Projectの恒久Identityではない。

Project DescriptorはVersion Controlで共有できるProjectの唯一の共通Modelとする。Editor、Runtime、Toolsは
`Cue.Project`が提供する同じModel、Parser、Serializer、Validatorを使用し、ApplicationごとのJSON Readerを持たない。

Project Descriptorの初期論理Schemaは次のFieldを持つ。

```json
{
    "schemaVersion": 1,
    "projectId": "00000000-0000-4000-8000-000000000000",
    "displayName": "Sample Project",
    "engineCompatibility": {
        "minimum": "0.1.0",
        "maximumExclusive": null
    },
    "roots": {
        "sourceAssets": "Assets/Source",
        "runtimeAssets": "Assets/Runtime",
        "generated": "Generated",
        "saved": "Saved"
    },
    "defaultScene": null,
    "requiredCapabilities": [],
    "extensions": {}
}
```

schema version 1では次のMemberを全て必須とし、省略時の暗黙Defaultを持たない。

| Object | Required Members |
| --- | --- |
| Top-level | `schemaVersion`, `projectId`, `displayName`, `engineCompatibility`, `roots`, `defaultScene`, `requiredCapabilities`, `extensions` |
| `engineCompatibility` | `minimum`, `maximumExclusive` |
| `roots` | `sourceAssets`, `runtimeAssets`, `generated`, `saved` |

この例のProjectIdは形だけを示すための値であり、有効なProject生成に使用しない。schema version 1では
`requiredCapabilities`は必須の空Array、`defaultScene`は必須の`null`だけを受理する。未確定のWire Schemaを同じVersionへ
後付けしないため、非空Capability Requirementと非null Default Sceneは保存しない。

具体的なCapability RequirementのField集合はIssue #140でADR-0012のVocabularyに基づいて確定し、非空要素を永続化する場合は
Project Descriptorの`schemaVersion`を増加させる。

`displayName`は有効なUTF-8で1 byte以上256 byte以下とし、Unicode Control Characterを拒否する。文字列は入力されたUnicode
Scalar Sequenceを保持し、ParserまたはSerializerがUnicode Normalizationや大小文字変換を暗黙実行しない。`extensions`はJSON
Objectを必須とする。Descriptor全体はUTF-8で1 MiB以下、JSON Nestingは32階層以下、単一Stringは256 KiB以下、単一Arrayまたは
Objectは4096要素以下とし、超過をFormat Errorとして拒否する。Descriptor全体の上限は入力File Byte数をParse前に
測定し、String上限はJSON Escapeを展開してUnicode Scalar SequenceをUTF-8へEncodeした後のByte数で測定する。Nestingは
Top-level ObjectをDepth 1とし、子ArrayまたはObjectへ入るたびに1を加える。Arrayは要素数、ObjectはMember数を数える。

Descriptorには次を保存しない。

- Projectの絶対Pathまたは親Directory
- CueEngine Installation Path、Compiler Path、SDK Path
- Machine名、User名、OS固有Known Folderの展開結果
- Recent／Pin／Last Opened／Missing等のProject一覧状態
- Editor Window配置、最後のSelection、個人向けBuild設定
- 現在MachineのSystem／Graphics Capability Snapshot
- Cache、Log、Autosave、Recovery Fileの内容または存在状態

### Stable Project Identity

`ProjectId`は128-bit UUID Version 4の値型とし、生成後はProject移動、Directory名変更、表示名変更、Engine更新で変更しない。
JSONではlowercaseの8-4-4-4-12形式で保存し、nil UUID、形式不正、Version／Variant不正を拒否する。

Project Pathは現在位置を示すLocatorでありIdentityではない。Recent RegistryはProjectIdをKeyにして最後に確認したPathを保持する。
同じProjectIdを持つ複数Pathを検出した場合、自動的に別ProjectIdを割り当てずDuplicateとして診断する。Project複製時に新しいIdentityを
発行する操作は将来の明示的なFork／Clone Workflowで扱い、通常Openまたは移動検出では実行しない。

`displayName`はUI表示用であり、一意性、Directory名、Executable名、C++ Symbol名を兼ねない。

### Project and Asset Identity

Project内Assetの永続参照はStable `AssetId`を使用する。PathはAssetの現在位置、検索、診断、人向け表示に使用できるが、
PathだけをScene、Default Scene、Component、Asset間参照の恒久Identityにしない。

schema version 1の`defaultScene`は未設定を示す`null`だけを受理する。Blank Project生成時も`null`とし、M09でDefault Sceneを
自動生成しない。AssetIdのBit幅、永続表現、nil規則、一意性Scope、生成、Asset Database、Source AssetとRuntime AssetのMappingは
Asset Pipelineの別Research Issueで決定する。非null Default Sceneを導入する場合は、その決定と同時にProject Descriptorの
`schemaVersion`を増加させる。

### Version and Compatibility

`schemaVersion`はProject Descriptor Formatの単調増加する正の32-bit unsigned integerであり、CueEngine Release Versionではない。
JSON Number Tokenは1から4294967295までのASCII decimal digitだけで表し、先頭Zero、符号、小数点、指数表記を拒否する。
Fieldの意味、必須性、既定値、正規化規則を変更する場合に増加させる。

`engineCompatibility`はProjectが要求するCueEngine Version範囲であり、`major.minor.patch`の非負整数3要素を使用する。各要素は
0から4294967295までの32-bit unsigned integerとし、JSON String内ではASCII decimal digitだけを使用する。値0は`0`、正の値は
先頭Zeroなしのcanonical表現だけを受理し、空要素、符号、空白、Suffix、4要素以上を拒否する。
`minimum`はInclusive、`maximumExclusive`はExclusiveまたは`null`とする。現在Engine Versionの提供元と比較結果の分類はIssue #140で
実装する。`maximumExclusive`が非nullの場合は`minimum`より厳密に大きい値だけを受理し、空または逆転した範囲を拒否する。

Parserは次の順序で判定する。

1. JSON構文とResource Limitを検証する
2. `schemaVersion`を読み、対応可能なFormatか判定する
3. 必須Field、型、ProjectId、相対Root、version 1で固定された空Capability Arrayとnull Default Sceneを検証する
4. Project Descriptor Modelを構築する
5. Engine／Capability互換性を別Serviceで判定する

Formatが読めることとProjectを現在環境で実行できることを同じ成功値にしない。互換性判定は
`Compatible`、`Degraded`、`Unsupported`、`Unknown`と理由を返し、Project Open可否とRuntime Feature Enablementも分離する。

未来の未対応`schemaVersion`を推測して読み込まない。既知Versionでは`extensions`の値を除く全ての固定Schema Objectについて、
Top-levelとNestedの両方で未知Fieldおよび重複Fieldを拒否する。意味を理解しないDataをModel構築時に破棄して再保存しない。
ModuleまたはTool固有のOptional Dataは`extensions`内でNamespace付きObjectとして保存し、未認識EntryをOpaque JSONとして保持して
Round-tripする。未知ExtensionをProject Coreの必須条件にしない。JSON Object自身の重複Member名はOpaque Extension内も含めて拒否する。

MigrationはVersionごとの`N -> N + 1`変換として登録し、途中Versionを飛ばさない。Open時に共有Descriptorを暗黙更新せず、
Migration Requiredを呼び出し側へ返す。明示的なMigrationは元Dataを検証後、Memory上で段階変換し、現行Schemaとして再検証してから
Issue #135のAtomic Storage契約で置換する。失敗時は元Descriptorを維持する。

### Root Roles

DescriptorのRootはProject Rootからの正規化相対Pathで表し、`/`を区切り文字とする。Root全体はASCIIで1文字以上255文字以下、
Segment数は1以上16以下、各Segmentは1文字以上64文字以下とする。各SegmentはASCII英数字、`_`、`-`、`.`だけで構成し、先頭または
末尾の`.`、空Segment、`.`、`..`を受理しない。絶対Path、Drive指定、UNC、Root外へ解決されるPathも受理しない。Project Rootの
絶対Pathと結合した後のHost固有Path長検査はIssue #135のIO契約で行い、共通Validatorの相対Path検査だけで作成可能とは判定しない。
各Segmentの最初の`.`より前をASCII case-insensitiveで比較し、`CON`、`PRN`、`AUX`、`NUL`、`COM1`から`COM9`、
`LPT1`から`LPT9`に一致するWindows予約Device名を拒否する。したがって`NUL.data`のような拡張子付きAliasも受理しない。
Reparse Point／Symlinkを含む実Filesystem上の脱出防止はIssue #135で決定する。

| Root Role | Role | Source of Truth | Runtime Access | Share Policy |
| --- | --- | --- | --- | --- |
| Source Assets | DCC File、Texture、Audio、Authoring Scene等の編集用Source | Yes | Runtimeから直接読まない | Version Control対象 |
| Runtime Assets | Import／Cook後にRuntimeが読むEngine形式 | No、Sourceから生成する | Read-onlyを基本とする | Build／配布Policyで決定 |
| Generated | Generated Source、IDE情報、Build Intermediate等 | No、再生成可能 | Runtime契約に使用しない | 原則Version Control対象外 |
| Cache | Import Cache、Index、Derived Data等 | No、削除可能 | 正しさを依存させない | User Workspace配下、共有しない |
| Saved | Autosave、Recovery、Log、診断等 | No、回復補助 | Package入力にしない | Project内相対Rootだが原則共有しない |

Source AssetsからRuntime Assetsへの変換は一方向とし、RuntimeがSource Assetsを直接読むFallbackを設けない。
Runtime Assets、Generated、Cache、Savedが欠損しても、それらを共有Authoring Dataの代替正本として扱わない。

Root Roleの重複・親子判定には、各ASCII文字をlowercaseへ変換したPortable Comparison Keyを使用する。これにより
`Assets`と`assets/Generated`のようにWindows上でAliasとなる組合せをHostに関係なく拒否する。例えば`Generated`を
`sourceAssets`の子に置く設定も拒否する。各Rootの先頭SegmentのPortable Comparison Keyが`cueproject.json`に一致する場合も、
Descriptor FileとDirectoryがWindows上で衝突するため拒否する。`CueProject.json`自身をRoot Role内へ含めない。

### User Workspace and Recent Registry

User WorkspaceはProject Descriptorと別のUser領域へ保存する。Platform固有の保存先解決はPlatform／IO実装が所有し、
`Cue.Project`の公開ModelへWindows Known Folder型または絶対Path規則を出さない。

WorkspaceはProjectIdをKeyにして、少なくとも次を保持できる。

- 最後に確認したProject Path
- Last Opened時刻
- Pin状態と安定した表示順
- Editor個人設定とSession復元情報
- Local Engine／Toolchain選択
- Project用CacheのLocator

Issue #139で、Recent Project RegistryのUser Workspace Fileを`CueWorkspace.json`、初期`schemaVersion`を1とする。
このFileはPlatform Compositionが選んだUser領域の`FilesystemRoot`直下へ保存し、Project Rootまたは`CueProject.json`へ
書き込まない。初期Schemaは次の固定Memberを持つ。

```json
{
    "schemaVersion": 1,
    "nextRegistrationOrder": 2,
    "nextPinOrder": 1,
    "entries": [
        {
            "projectId": "12345678-1234-4abc-8def-1234567890ab",
            "locator": "C:/Projects/Sample",
            "lastOpenedMillis": 0,
            "isPinned": false,
            "pinOrder": 0,
            "registrationOrder": 1,
            "locatorState": "available"
        }
    ]
}
```

`locatorState`は`available`、`missing`、`moved`のいずれかとする。`lastOpenedMillis`は呼び出し側が供給する
UTC Unix Millisecond、`registrationOrder`と`pinOrder`は1から始まる単調増加値とする。非Pin Entryの`pinOrder`は0、
Pin Entryでは0以外を必須とする。File全体は1 MiB以下、単一Locatorは有効なUTF-8かつ非Control文字で1 byte以上
32 KiB以下とし、JSON Parserの共通Nesting、String、Container上限も適用する。既知Versionでは未知Member、重複Member、
重複ProjectId、重複Locator、重複Orderを拒否し、未来Versionを推測して読まない。

通常登録で同一ProjectIdが異なるLocatorに存在する場合はDuplicateとして拒否する。Project移動は、呼び出し側が同一ProjectIdを
確認した後の明示的な再関連付け操作だけがLocatorを更新する。RegistryのSerialize／Parseは安定順を保持し、Pin EntryはPin順、
非Pin EntryはLast Opened降順、同時刻は登録順で並べる。`CueWorkspace.json`の保存はIssue #135のAtomic File Replaceを使用する。

Recent Registryから除外してもProject Folderを削除しない。登録Pathが存在しない場合もEntryを暗黙削除せずMissingとして返す。
別Pathから同じProjectIdを正常にOpenした場合はMoved候補として再関連付けできる。Pathが同じでもProjectIdが変わっている場合は、
別Projectまたは置換として確認を要求する。

WorkspaceのLast Opened、Pin、PathはProject共有Descriptorへ書き戻さない。Workspace FileのVersionとAtomic保存はIssue #139で実装する。

### Required Capability Boundary

Project Descriptorは現在Machineの観測値ではなく、Projectが必要とするCapability Requirementだけを保持する。
ADR-0012のSupported、Implemented、Enabledを混同せず、互換性判定時にRequirementと現在のImmutable Snapshot、Engine実装Catalogを
入力として比較する。

Hardware QueryがFailedまたはNotQueriedの場合はUnsupportedへ変換せず、互換性を`Unknown`として理由を残す。
Project Openの可否と、個別Runtime FeatureをEnabledにする判断は別結果とする。

Issue #140では、RHIまたはPlatformのNative型をProjectへ漏らさない初期Capability Identityとして、`Baseline3D`、
`WaveOperations`、`EnhancedBarriers`、`RayTracing`、`MeshShader`、`VariableRateShading`、`SamplerFeedback`を定義する。
Requirementは`Required`と`Preferred`を区別し、VersionまたはTierで表現できる条件はNative数値ではなく任意の最小
`CapabilityVersion`を持つ。現在環境は同じIdentityごとにADR-0012の`CapabilityState`と任意Versionを渡し、Hardware対応、
CueEngine実装、Runtime有効化を別の状態として保つ。

`ProjectCapabilityProfile` schema version 1は互換性Serviceへ渡す所有値契約であり、Project Descriptor schema version 1の
Wire Formatを変更しない。Descriptor v1の`requiredCapabilities`は引き続き空Arrayだけを受理し、実機Snapshot、Adapter情報、
Vendor情報、現在のEnablementを保存しない。非空ProfileをProjectへ永続化するWire Schemaは、Migration経路と共にDescriptor
schema version 2以降として別Issueで導入する。

`ProjectCompatibilityReport`は`Compatible`、`Degraded`、`Unsupported`、`Unknown`、理由Code、Runtime Featureごとの有効状態を
返す。FormatまたはEngine Version範囲の不一致だけが`can_open = false`となる。Hardware未対応、Engine未実装、Query不明は
Project Dataを開いて診断または編集すること自体を妨げず、全体互換性とRuntime Feature判定へ反映する。RequiredのQuery不明は
`Unknown`、RequiredのHardware未対応・Engine未実装・Version不足は`Unsupported`、Preferredの未達とRuntime無効は`Degraded`
とする。これにより、Project Open可否をRuntime Feature Enablementへ暗黙連結しない。

Evaluatorは入力Profile、Snapshot、Versionへの参照を呼び出し中だけ借用し、返却Reportは理由とRuntime判定を所有する。
共有可変状態を持たず、独立した入力による並行呼び出しを許可する。共有`AssertContext`を渡す場合は、そのLoggerとFatalHandlerの
既存Thread Safety契約を呼び出し側が満たす。Format Version 0、対応Format Version 0、空または逆転したEngine範囲は
`InvalidCompatibilityInput`として回復可能に返す。Allocation失敗を含む予期しない例外は既存Project APIと同様にFatalHandlerへ
渡して終了し、部分Reportまたは変更済みProject Dataを返さない。

### Module, Ownership, and Threading Boundary

M09で`Cue.Project`をFirst-party Static Libraryとして追加する。

```text
Engine/Source/Project/
    CMakeLists.txt
    Public/Cue/Project/
    Private/

Engine/Tests/Project/
```

- `Cue.Project`は`Cue.Foundation`と、Issue #135で決定するPlatform非依存IO契約だけへ依存できる
- `Cue.Project`はPlatform実装、RHI、RuntimeHost、Editor、Graphicsへ依存しない
- Editor、Runtime、Toolsは`Cue.Project`の公開APIを使用する
- JSON Library型、Filesystem実装型、Windows型をPublic Headerへ出さない
- ProjectをProcess Global Singletonとして保持しない

Parserは成功時に検証済み`ProjectDescriptor`の所有値を返す。返却ModelはPath Resolver、Filesystem、JSON Documentへの参照を保持しない。
Open済みProjectとWorkspaceのLifetimeはApplicationのComposition Ownerが管理する。

Parser、Serializer、Validatorは共有可変Cacheを持たず、異なる入力への並行呼び出しを許可する。同一Projectへの保存、Migration、
Workspace更新の排他制御はStorage／Session Ownerが管理する。失敗可能APIはADR-0005の`cue::Result<T>`とError分類を使用し、
Project Module内でErrorをLogして握りつぶさない。

## Consequences

### Positive

- Projectを移動または改名してもProjectIdを維持できる
- Project共有データへ個人設定とMachine Pathが混入しない
- Editor、Runtime、Toolsが同じParserとValidation結果を利用できる
- Project Format、Engine Version、Hardware Capabilityの問題を別々に診断できる
- Source AssetとRuntime Asset、共有正本と再生成可能Dataを分離できる
- Blank ProjectをScene、Script、Rendererへ依存せず生成できる

### Negative

- Project Pathだけで管理する実装よりRegistryと重複処理が増える
- AssetIdを解決するAsset Databaseが完成するまでDefault Scene参照を実体化できない
- Unknown Top-level Fieldを拒否するため、Format拡張にはschemaVersion更新または`extensions`の利用が必要になる
- Runtime Assets、Generated、Cache、SavedのCleanupとRecovery Policyを後続Issueで実装する必要がある
- Project複製時は明示的にProjectIdを再発行するWorkflowが必要になる

## Enforcement

- Project Descriptorの唯一のParser／Serializer／Validatorを`Cue.Project`に置く
- Project Pathまたは表示名をProjectIdの代替として比較するCodeをReviewで拒否する
- Project共有DescriptorへWorkspace FieldまたはHardware Snapshotを追加しない
- Default Sceneと将来のAsset参照をPathだけで保存しない
- Source AssetsをRuntimeが直接読む依存を追加しない
- Schema変更はMigrationとTestを伴うADRまたはIssueで記録する
- Project Root外Path、Root Role重複、未来Schema、未知必須FieldをTestで拒否する
- Project移動、Duplicate ProjectId、Missing Path、Workspace非共有をProcess Testで検証する

## Follow-up

- Issue #135: Project Path、Filesystem、Atomic Storage契約
- Issue #136: Platform非依存IO契約とWindows実装
- Issue #137: Version付きProject Descriptor Model、Parser、Serializer、Validator
- Issue #138: Blank Project TemplateとAtomic Generator
- Issue #139: Recent Project RegistryとUser Workspace Storage
- Issue #140: Engine VersionとRequired Capability互換性判定
- Asset Pipeline Research: AssetId、Source／Runtime Asset Mapping、Import／Cook、Runtime Package
- Project Clone Research: 明示的ProjectId再発行と参照整合性

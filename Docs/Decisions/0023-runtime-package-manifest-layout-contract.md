# ADR-0023: Runtime Package Manifest, Minimal Scene Identity, and Atomic Publication Contract

- Status: Accepted
- Date: 2026-09-09
- Decision Owners: CueEngine Project

## Context

M16では、Blank Projectの作成、Scene編集、Play、Game Module Buildに続き、同じProjectをStandalone Runtimeとして
Package化して起動できる最初の制作Loopを完成させる。

ADR-0013はProject共有DataとRoot Roleを分離し、RuntimeがSource Assetsを直接読むFallbackを禁止した。
ADR-0017はAuthoring Sceneの`SceneAssetId`とRuntime WorldのSession-local Identityを分離した。
ADR-0022はProject固有Codeを一つのGame Module Artifactとして公開し、M16 PublisherがShared Read Leaseを保持して
現在の成功VersionをPackageへCopyする契約を決定した。

現在は一般Asset Database、Import／Cook、Runtime Asset Dependency Graphが存在しない。その完成を待つとStandalone起動を
検証できず、反対にM16で一般`AssetId`、CAS、差分Cookまで先取りすると、Texture、Mesh、Audio等の未確定要件を永続形式へ固定する。
本ADRはStartup SceneとGame Moduleだけで成立する最小Package境界を決定し、一般Asset Pipelineの設計を行わない。

## Legacy Reference

### Legacy Problem

旧CueEngineも、Editorで選択したProjectとSceneをEngineとは別の実行物として起動し、配布可能なFile集合へまとめる必要があった。

### Legacy Approach

旧実装ではProject設定内のScene Path、Source Tree、Build出力を直接参照する箇所があり、Editor環境とStandalone環境の境界が
明確ではなかった。PathがIdentityとLocatorを兼ね、必要Fileの完全なInventory、Hash、Version、失敗時の旧出力保全も
一つの契約として固定されていなかった。

### Legacy Strengths

- Projectから起動Sceneを選択する基本Workflowが存在した
- Engine ExecutableとProject固有Codeを組み合わせる制作Loopを試せた
- Editorから実行する利用者体験を早い段階で確認できた

### Legacy Problems

- Current Directory、Project Source、Machine固有Build出力への暗黙依存を検出しにくかった
- Scene Pathが恒久Identityになり、RenameやPackage移動へ弱かった
- Packageに必要なFileと不要なFileの境界が明文化されていなかった
- Copy途中のDirectoryや新旧Fileが最終Outputとして観測され得た
- Source SceneとRuntime DataのFormat、所有権、変換方向が分離されていなかった

### New Design

旧Source Code、File Layout、Serializer、Path規則はコピー、移植、改名、部分抽出しない。現在のRebuild契約から、
Version付きManifest、検証済みInventory、実行File相対のPackage Root、一方向Runtime Data PublisherをFirst-party Codeとして設計する。

## Reference Engine Comparison

| Engine | 参考にする点 | CueEngineでそのまま採用しない点 |
| --- | --- | --- |
| Unreal Engine | Cook済みContentとExecutableをStagingし、Authoring Sourceから実行物を分離する | Package、UObject、Asset Registry、Platform Cook全体をM16へ導入しない |
| Unity | PlayerがProjectのAuthoring DirectoryではなくBuild済みDataから起動する | Library、GUID Database、Scene Build List全体を先取りしない |
| Godot | Executableから相対的にProject Dataを発見し、自己完結した配布物を作る | PCK Container、Resource Importer、`res://`全体を模倣しない |
| SOL-AVES | Source、変換処理、Runtime向けDataの分離と反復可能な生成を重視する | 一般Asset Pipeline、分散処理、CAS、増分変換をM16の前提にしない |

CueEngineは、Authoring SourceとRuntime Inputを分離すること、必要Fileを明示的に列挙すること、同じ入力から検証可能な出力を
作ることを取り入れる。一方、M16では一つのStartup Scene、一つのGame Module、一つのRuntimeHostへ限定する。

Sources:

- [Unreal Engine Packaging Your Project](https://dev.epicgames.com/documentation/en-us/unreal-engine/packaging-your-project)
- [Unity Building and running a WebGL project](https://docs.unity3d.com/Manual/webgl-building.html)
- [Godot Exporting projects](https://docs.godotengine.org/en/stable/tutorials/export/exporting_projects.html)
- [CEDiL: SOL-AVESの高性能なランタイムを構成するアーキテクチャ](https://cedil.cesa.or.jp/cedil_sessions/view/3297)

## Current Requirements

- RuntimeはPackage外のProject Source、Generated、Saved、User Workspace、Current Directoryを参照しない
- Startup Sceneの永続Identityと現在のSource Locatorを分離する
- 一般AssetId、Asset Database、Import／Cookを先取りしない
- Source SceneをRuntime PackageへそのままCopyしない
- Game Module、RuntimeHost、Runtime DataのSizeとHashをManifestで固定する
- Packageは移動後も同じ内容で起動できる
- Publish前の失敗とCancelでは最終Destinationを変更しない
- 既存Destinationへ暗黙Merge、上書き、部分更新しない
- 未知Version、欠損、不一致では推測FallbackせずFail-closedにする
- Debug、Development、ReleaseのFileを混在させない

## Decision

### Scope and Source of Truth

M16 PackageはProject所有の入力について自己完結する次の五つを一つのDirectoryへ配置する。OSとToolchain Runtimeの実行前提は
後述のRuntime Dependency Inventoryに従う。

1. EngineがBuildした共通`CueRuntimeHost.exe`
2. ADR-0022の現在の成功`CueGameModule.dll`
3. 対応する`CueGameModule.metadata.json`
4. Project共有設定から生成した最小Runtime Project Data
5. Startup Scene Sourceから生成した最小Runtime Scene Data

Runtimeの正本はPackage Root直下の`CuePackage.json`と、そこに列挙されたFileだけである。
`CueProject.json`、`.cuescene`、`Generated/Artifacts/.../Current.json`、Editor Session、Recent RegistryはPackageへ含めず、
Runtimeから参照しない。Packageは再生成可能な配布出力であり、Authoring SourceまたはGame Build Artifactの正本にならない。

### Scene Identity and Project Descriptor

M16では一般`AssetId`型を導入しない。Startup SceneはADR-0017の`SceneAssetId`で識別する。
`SceneAssetId`がUUID v4であることを、将来の一般Asset IdentityのBit幅、名前、Database Key、Source／Runtime Mappingの決定として
扱わない。一般Asset Databaseを導入する場合は、`SceneAssetId`との明示的MappingまたはMigrationを別ADRで決定する。

Project Descriptor schema version 2は、既存Member名`defaultScene`を維持し、`null`または次のObjectを受理する。

```json
"defaultScene": {
    "sceneAssetId": "12345678-1234-4abc-8def-1234567890ab",
    "sourceLocator": "Scenes/Default.cuescene"
}
```

`sceneAssetId`はIdentity、`sourceLocator`はDescriptorの`roots.sourceAssets`からの現在位置である。
`sourceLocator`はProject Root相対でも絶対Pathでもなく、`sourceAssets` Root境界付き相対Locatorとする。
Scene Open、Save、Package時に、Locator先File内の`sceneAssetId`がReferenceと一致しなければ拒否する。
Scene Renameまたは移動ではLocatorだけを明示更新し、Identityを再発行しない。

schema version 1から2へのMigrationは`defaultScene: null`をそのまま保持する。MigrationがScene Fileを作成したり、既存Fileから
暗黙に一つを選んだり、Identityを推測したりしない。既存Projectは明示的にStartup Sceneを選択してからPackageする。
schema version 2のBlank Project Generatorは新しい`SceneAssetId`を一度生成し、Default SceneをAtomicに作成した後、同じIdentityを
Descriptorへ保存する。Project生成全体の失敗では最終Project Directoryを公開しない。

`defaultScene: null`は編集可能なProjectとして有効だが、M16 Package Requestでは`MissingStartupScene`として失敗する。

### Immutable Publisher Inputs

Package Operationは開始時にProject DescriptorとStartup Sceneをそれぞれ一度だけ読込み、Parser、Resource Limit、Identityの一致を
検証して所有するImmutable Snapshotへ変換する。Editorの未保存変更を暗黙にPackageへ混ぜない。Editor WorkflowはDirty Documentを
明示SaveするかPackageをCancelし、Headless Publisherは保存済みSourceだけを入力にする。

Game ModuleはADR-0022のArtifact Storeから取得する。PublisherはProject IdentityとConfigurationにBindingされたShared Read Leaseを
取得してから`Current.json`を一度読み、参照Versionの全FileをSize／Hash再検証し、Package StagingへのCopyとCopy後検証が終わるまで
Leaseを保持する。Leaseは発行元Store、Project Root、Configurationと異なるPublisherへ渡せない。

Artifact Shared Read LeaseはPackage CandidateへのGame Module Copy完了後に解放し、Package Directoryの最終Publish Leaseと同時保持しない。
このLock順序によりBuild Artifact CleanupとPackage Publishの循環待ちを作らない。

### Minimal Runtime Data

M16は一般Asset Cookではなく、二つのVersion付きCanonical JSONを生成する。

| File | Role | Minimum Data |
| --- | --- | --- |
| `Data/CueProject.runtime.json` | Runtime Project Data | schema version、ProjectId、Engine Compatibility、Required Capability、Startup SceneAssetId |
| `Data/Scenes/<scene-asset-id>.cueruntime.json` | Runtime Scene Data | schema version、SceneAssetId、ObjectId、Hierarchy、Active、Core Transform、実体化可能なComponent Data |

Runtime Project Data schema version 1の完全なWire Objectを次に固定する。UTF-8、BOMなし、LF、末尾改行ありとし、
Writerは例示順でMemberを出力する。ReaderもM16ではこのCanonical Member順だけを受理し、未知Member、欠落、重複、末尾Dataを拒否する。

```json
{"schemaVersion":1,"projectId":"12345678-1234-4abc-8def-1234567890ab","engineCompatibility":{"minimum":"1.0.0","maximumExclusive":"2.0.0"},"requiredCapabilities":[],"startupSceneAssetId":"22345678-1234-4abc-8def-1234567890ab"}
```

- `schemaVersion`はJSON整数`1`だけを受理する
- `projectId`と`startupSceneAssetId`はlowercase UUID v4とし、nilを拒否する
- `engineCompatibility.minimum`はCanonical `major.minor.patch`文字列とする
- `engineCompatibility.maximumExclusive`は、minimumより大きいCanonical Version文字列または`null`とする
- `requiredCapabilities`は将来予約であり、version 1では空Arrayだけを受理する

Runtime Scene Data schema version 1の完全なWire Objectを次に固定する。空白、Encoding、Member検証はRuntime Project Dataと同じとする。

```json
{"schemaVersion":1,"sceneAssetId":"22345678-1234-4abc-8def-1234567890ab","objects":[{"objectId":"32345678-1234-4abc-8def-1234567890ab","parentObjectId":null,"active":true,"transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},"components":[{"componentInstanceId":"42345678-1234-4abc-8def-1234567890ab","typeId":"52345678-1234-4abc-8def-1234567890ab","schemaVersion":1,"fields":[{"fieldId":1,"kind":"boolean","value":true}]}]}]}
```

- `sceneAssetId`、`objectId`、`componentInstanceId`、`typeId`はlowercase UUID v4とし、nilを拒否する
- `objects`は`ObjectId`のByte辞書順、`components`は`ComponentInstanceId`順、`fields`は符号なし`fieldId`昇順とする
- `objectId`はScene内、`componentInstanceId`はScene内、`fieldId`は同一Component内で一意とする。各配列は同値を許さない
  strict ascending orderとし、重複をReader／Writerとも`UnsupportedRuntimeSceneData`で拒否する
- `parentObjectId`は同じ`objects`内の別Object IDまたは`null`とし、循環、自己Parent、欠損Parentを拒否する
- `active`はJSON booleanとする
- `translation`、`rotation`、`scale`はそれぞれ3、4、3個の、有限IEEE 754 binary32へround-trip可能なJSON numberとする。
  Writerはlocale非依存の最短round-trip表現を使用し、Readerはbinary32のoverflow、NaN、Infinityを拒否する
- `rotation`はbinary32へ変換した4成分から`cue::math::length`で求めた長さを`1.0F`と比較し、絶対Tolerance
  `0.00001F`、相対Tolerance `0.00001F`の`cue::math::is_unit_rotation`を満たす単位Quaternionだけを受理する。
  条件を満たさない値はReader／Writerとも`UnsupportedRuntimeSceneData`で拒否し、暗黙に正規化しない
- `schemaVersion`と`fieldId`は`1`以上`4294967295`以下のJSON整数とし、それぞれ`uint32_t`のComponent Schema Version、
  Field Identityを表す。範囲外、符号、小数表現を拒否する
- `kind`は`boolean`、`signedInteger`、`unsignedInteger`、`floatingPoint`、`string`、`assetReference`のいずれかとし、`value`のJSON型を一致させる
- `signedInteger`は`-9223372036854775808`以上`9223372036854775807`以下のJSON整数、`unsignedInteger`は`0`以上
  `18446744073709551615`以下のJSON整数とし、範囲外、小数表現を拒否する。`floatingPoint`は有限IEEE 754 binary64へ
  round-trip可能なJSON numberだけを許可し、overflow、NaN、Infinityを拒否する
- `assetReference`はM16では解決可能なRuntime RegistryとPackage Inventoryが存在しないため、値にかかわらず
  `UnsupportedRuntimeSceneData`で拒否する。一般Asset DatabaseのIdentity契約は確定しない
- RuntimeHost v1が登録していないComponent Type、Schema Version、Field、Asset Referenceを含む場合、WriterまたはReaderは省略せず`UnsupportedRuntimeSceneData`で拒否する

Runtime Data WriterはRaw Source Byte列をCopyせず、検証済みProject Modelと`SceneDocumentSnapshot`から新しいWire表現を生成する。
Runtime SceneはAuthoring FileのExtension、Editor状態、未知Extension、Undo履歴、Locator、保存用Migration情報を含めない。
`ObjectId`は診断と`SceneInstance` Mappingのため維持するが、Runtime `EntityHandle`を保存しない。

Runtimeが意味を解釈できないComponent、Field、Schema VersionをPublisherが黙って省略しない。Authoring Sourceでは未知DataをLosslessに
保持したまま、Package Operationを`UnsupportedRuntimeSceneData`で失敗させる。Runtime DataからAuthoring Sceneへ戻すReader、
Import、Round-trip Serializerは提供しない。

同じCanonical Project Snapshot、Scene Snapshot、Engine Version、ConfigurationからはByte単位で同じRuntime Dataを生成する。
Timestamp、Machine Path、User名、Operation ID、Container列挙順、HashMap順をRuntime Dataへ保存しない。

### Package Layout

初期Layoutを次に固定する。

```text
<package-root>/
  CuePackage.json
  CueRuntimeHost.exe
  Game/
    CueGameModule.dll
    CueGameModule.metadata.json
  Runtime/
    <required app-local runtime dependencies>
  Data/
    CueProject.runtime.json
    Scenes/
      <scene-asset-id>.cueruntime.json
```

`Runtime` Directoryは空なら作成しない。PDB、Build Log、CMake Binary Tree、Import Library、Editor、ImGui、Source Header、Source Asset、
Diagnostic BundleはStandalone起動に必要なRuntime DependencyではないためPackageへ含めない。

Package Rootの名前と親Directoryは呼び出し側のLocatorであり、Manifest Identityへ含めない。
`CueRuntimeHost.exe`は自身のExecutable Pathの親DirectoryをPackage Rootとし、Current Directory、Environment Variable、Registry、
Editor Process、Project PathからRootを推測しない。M16の正式起動ではManifest File名Overrideを提供しない。

### Package Manifest Wire Format

`CuePackage.json` schema version 1はUTF-8、BOMなし、LF、末尾改行ありのCanonical JSONとする。
Top-levelの必須Memberと順序を次に固定する。

```json
{
    "schemaVersion": 1,
    "projectId": "12345678-1234-4abc-8def-1234567890ab",
    "engineVersion": "0.1.0",
    "configuration": "Development",
    "startupScene": {
        "sceneAssetId": "12345678-1234-4abc-8def-1234567890ab",
        "runtimeDataPath": "Data/Scenes/12345678-1234-4abc-8def-1234567890ab.cueruntime.json"
    },
    "files": [
        {
            "role": "runtimeHost",
            "path": "CueRuntimeHost.exe",
            "sizeBytes": 1,
            "sha256": "0000000000000000000000000000000000000000000000000000000000000000"
        }
    ]
}
```

ReaderはMember順と意味を持たない空白には依存しないが、必須Memberの欠落、重複、未知Member、型不一致、末尾Dataを拒否する。
`schemaVersion`はJSON整数`1`だけを受理し、未来Versionを推測読込またはRuntime内Migrationしない。

`configuration`は`Debug`、`Development`、`Release`のいずれかとする。`engineVersion`はADR-0013のCanonical
`major.minor.patch`表現を使用する。`projectId`、`sceneAssetId`はlowercase UUID v4で、nilを拒否する。

`files`の各Entryは次の`role`を使用する。

| Role | Count | Fixed Path or Rule |
| --- | --- | --- |
| `runtimeHost` | exactly 1 | `CueRuntimeHost.exe` |
| `gameModule` | exactly 1 | `Game/CueGameModule.dll` |
| `gameModuleMetadata` | exactly 1 | `Game/CueGameModule.metadata.json` |
| `projectRuntimeData` | exactly 1 | `Data/CueProject.runtime.json` |
| `startupSceneRuntimeData` | exactly 1 | `startupScene.runtimeDataPath`と一致 |
| `runtimeDependency` | zero or more | `Runtime`直下 |

Entryは`path`のUTF-8 Byte昇順で整列する。同一Pathの重複、ASCII case-insensitive Alias、同一必須Roleの重複を拒否する。
`sizeBytes`はCopy後Fileの未変換Byte数、`sha256`は同じByte列に対する64文字lowercase SHA-256とする。
Manifest自身は自己参照Hashを避けるため`files`へ含めない。

Game Module MetadataのProjectId、Configuration、Architecture、ABI、Toolset、Runtime Libraryと、Manifest、RuntimeHostの期待値を
DLL Load前に一致させる。Runtime Project DataのProjectId／Startup SceneAssetId、Runtime Scene DataのSceneAssetIdをManifestと
一致させる。Runtime Scene Data version 1はProjectIdを重複保存しない。

### Resource Limits

schema version 1はParseまたはMemory確保前に可能な限り次を適用する。

| Resource | Limit |
| --- | ---: |
| Manifest File | 1 MiB |
| JSON nesting | 16 levels |
| JSON string after UTF-8 decoding | 64 KiB |
| File entries | 256 |
| Relative package path | 1,024 UTF-8 bytes |
| Path segments | 32 |
| One packaged file | 8 GiB |
| Sum of listed files | 16 GiB |
| Runtime Project Data | 1 MiB |
| Runtime Scene Data | 64 MiB |
| Runtime Scene objects | 1,000,000 |
| Components per object | 4,096 |

Size加算、Object数、Component数はOverflowを検査する。上限超過を部分読込み、切捨て、警告付き成功に変換しない。
上限変更はManifestまたはRuntime Dataの対応Schema Versionと互換性を先に判断する。

### Relative Path and Filesystem Boundary

Manifest Pathは`/`区切りのPackage Root相対Pathとする。空Path、空Segment、`.`、`..`、先頭または末尾`/`、`\\`、Drive、UNC、
URI、NUL、Control Character、Windows予約Device名、末尾`.`または空白を拒否する。

Package ReaderとPublisherは全SegmentをASCII case-insensitiveで比較し、重複Aliasを拒否する。Rootと各EntryをWindowsの
Extended-length Native Pathとして開き、Package RootからFileまでの全既存SegmentでReparse Pointを追跡せず拒否する。
EntryはRegular Fileだけを受理し、Directory、Device、Named PipeをFileとして扱わない。

Publisherは入力Fileを開いたHandleからSizeとHashを測定し、検証した同じByte列をStagingへCopyする。Path検証後に別Pathを
再解決して読み直すTOCTOU Fallbackを設けない。RuntimeはManifestに列挙されない場所から同名Fileを探索しない。

### Runtime Dependency Inventory

Runtime Dependency Collectorは`CueRuntimeHost.exe`とGame ModuleのBuild Metadata、およびM16で明示的に登録したApp-local Runtime Fileを
入力にする。Project Directory、`PATH`全体、Windows System Directory、Visual Studio Installation、vcpkg Install Treeを再帰探索しない。

OSが提供するSystem DLLと、下記のMSVC Runtime前提はPackage Entryにしない。Game ModuleのApp-local DLLが必要な場合は、
Build時の正本PathからStagingへCopyし、`runtimeDependency`としてHash／Sizeを記録する。M16ではWindows DLL Loaderが再帰探索しない
ことと検索境界を一致させるため、`runtimeDependency`は`Runtime/`直下の一File Nameだけを許可し、子Directoryを拒否する。
未知または不足したDependencyを起動時のOS Search Pathへ委ねず、Publish前に失敗する。Debugと非DebugのRuntime Libraryを同じManifestへ
混在させない。

現行BuildはCMake／MSVC既定のDynamic Runtimeを使用するため、`CueRuntimeHost.exe`のLoad-time ImportはWindows System DLLに加え、
使用Compilerに対応するMicrosoft Visual C++ Runtimeを許可する。Development／Release Packageの実行Hostには対応するx64
Visual C++ Redistributable、Debug Packageの実行Hostには対応するVisual Studio C++ Debug Runtimeを前提とし、どちらもWindows
System DirectoryへInstallされた正本だけを使用する。`CueRuntimeHost.exe`はMSVC Linkerの`/DEPENDENTLOADFLAG:0x800`を必須とし、
Process起動時のLoad-time Importを`LOAD_LIBRARY_SEARCH_SYSTEM32`へ限定する。Package Root、Current Directory、`PATH`、User Directoryを
Host起動前の依存解決へ含めない。これらはToolchainの実行前提でありPackageへCopyせず、Debug Packageを配布物として扱わない。
M16のStandaloneはProject SourceやBuild Treeからの独立を意味し、OS／Toolchain RuntimeまでDirectory内へ複製するInstaller契約は
含まない。Production配布でのStatic RuntimeまたはRedistributable Installer方針は別ADRで決定する。

M16 PublisherのHost Import検証は、Windows System DLLと上記MSVC Runtimeの既知Import名だけを許可する。Host自身がそれ以外の
App-local DLLを直接Importしている場合、Windows LoaderはManifest検証や`Runtime` Directory登録より前に解決を要求するため、
M16 Publisherはその構成をPackageせず失敗させる。
RuntimeHostはPackage処理の最初に`SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 |
LOAD_LIBRARY_SEARCH_USER_DIRS)`を成功させ、Application Directory、Current Directory、`PATH`をProcessの既定DLL検索対象から除外する。
Game ModuleをLoadする前に追加できるApp-local Dependencyだけを`Runtime/`へ配置し、全Entry検証後にそのDirectoryを`AddDllDirectory`で
Process-localなUser Directoryへ登録する。Game Module自体は検証済み絶対Pathを
`LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS)`で開き、
`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`を指定しない。これにより、Game Moduleの隣接`Game/`、Package Root、Current Directory、`PATH`を
依存DLLの候補へ含めない。将来RuntimeHostへApp-local直接Dependencyが必要になった場合は、Executable隣接配置の信頼境界または
静的Bootstrapを別ADRで決定する。

`Runtime/`をDLL検索Directoryへ登録する直前に、RuntimeHostはDirectoryを非再帰で列挙し、通常Fileだけで構成されること、Reparse
Pointや子Directoryがないこと、各File NameのASCII case-insensitive集合がManifestの`runtimeDependency` Entry集合と完全一致することを
検証する。未列挙File、欠損Entry、Alias、列挙失敗が一つでもあればDirectoryを登録せず起動を拒否する。Manifestに
`runtimeDependency`がない場合は`Runtime/`が存在しないか空であることを要求する。これにより、Package公開後に追加された未検証DLLを
Windows Loaderの候補へ含めない。

新しい第三者LibraryをRuntime Dependencyへ追加またはVersion更新する場合は、AGENTS.mdの承認、vcpkg Manifest、License、Notice契約を
別Issueで満たす。本ADRは新しい外部Library導入を承認しない。

### Compatibility and Startup Order

RuntimeHostはPackage Rootを確定した後、次の順序でFail-closedに起動する。

1. `CuePackage.json`のSize、UTF-8、Schema、Resource Limitを検証する
2. 全EntryのPath、Role、Count、Size、Hashを検証する
3. HostのEngine Version、Architecture、ConfigurationをManifestと照合する
4. Game Module MetadataをADR-0022の順序で検証する
5. Runtime Project DataとStartup Scene Runtime DataをParseし、IdentityとCompatibilityを照合する
6. Game ModuleをLoadし、Schema、Component、System Factoryを一時Registrationへ登録する
7. 登録済みの不変System Factory集合からRuntime Application Project Scopeを構築する
8. Startup Sceneを新しいRuntime Worldへ実体化する
9. Runtime Application SessionをStartし、Loopへ入る

失敗時は完了済みStepだけを逆順で終了し、部分World、System State、Module Handle、DLL Handleを残さない。
Game ModuleのStopと全State破棄が完了する前にDLLをUnloadしない。Owner Thread契約はADR-0021とADR-0022を維持する。

Package RuntimeはSchema、Manifest、Runtime Dataを暗黙Migrationしない。互換性のないPackageは対応Engineで再生成する。
Required Capabilityの取得失敗はUnsupportedへ捏造せず、既存Compatibility分類に従って起動拒否理由を返す。

### Staging and Atomic Publish

Package Requestは呼び出し側が選んだ最終Destination Directoryを受ける。Destinationの絶対Host PathはOperation入力であり、
ManifestまたはRuntime Dataへ保存しない。

Publisherは最終DestinationのSiblingにOperation-owned Staging Directoryを作成する。StagingとDestinationが同じVolumeであることを
確認し、Cross-volume Copyまたは`copy + delete`をAtomic Renameと報告しない。

次の順序で公開する。

1. Destinationが存在しないことを確認する
2. 一意なSibling StagingをOperationが作成する
3. Runtime Dataを生成し、Game Artifact、RuntimeHost、DependencyをCopyする
4. 各Fileを閉じる前にWriteと`FlushFileBuffers`の成功を確認する。失敗時はStagingだけをRollbackする
5. Manifestを最後にStagingへ書き、同様にFile内容をFlushする
6. Staging内の全EntryをManifestから再読込し、Size、Hash、Identity、Compatibilityを検証する
7. Destination不存在を再確認する
8. ADR-0014のWrite-through境界を満たす置換なしの同一Volume RenameでStaging DirectoryをDestinationへ一度だけ公開する
9. Rename APIの失敗時は固定済みStaging IdentityがSource、Destinationのどちらに存在するか事後確認し、公開状態を推測しない
10. 公開済みDestinationからManifestと全Entryを再検証し、Publish Outcomeを確定する

既存DestinationがFile、Directory、Reparse Pointのいずれでも、暗黙Merge、削除、Rename、上書きを行わず`DestinationExists`で失敗する。
別の最終名を選ぶ操作は呼び出し側が明示する。Operation IDまたはTimestampをPackage Contentへ保存しない。

Cancelは最終Rename直前まで受理し、StagingだけをCleanupする。Rename成功後のCancelは公開済みPackageを巻き戻さず、Publish Outcomeを
優先する。CleanupはOperationが作成した正確なStaging Rootだけを対象にし、Destination、Project Source、Build Artifact、別Operationを
再帰削除しない。

Publish OutcomeはADR-0014と同じ分類を使用する。

- `Committed`: 全FileのFlush、Write-through Rename、Destination再読込による完全性確認に成功し、Package成功として記録できる
- `NotPublished`: Destinationは公開されず、以前の成功Package記録を維持する
- `PublishedButDurabilityUnknown`: Renameの耐久性を確認できない、またはRename成功後のManifest／Entry再検証に失敗して、
  Destinationが可視でも安全な成功Packageと確定できない。成功として自動Runせず、確認できた可視状態を診断へ記録する。
  通常の再検証失敗も公開後である以上このOutcomeへ含め、`NotPublished`へ分類したり自動Rollbackしたりしない

最後に成功したPackage LocatorとManifest SummaryはEditor SessionまたはUser Workspace側の再生成可能な状態として保持できるが、
Project Descriptor、Source Scene、Package ManifestへMachine絶対Pathを書き戻さない。

### Diagnostics and Security

Package Operation ResultはStage、Outcome、Destination、Manifest Summary、主要Identity、Error Contextを返す。
Credential、Environment全体、Source本文、Machine絶対PathをPackageへ保存しない。Editor Logまたは診断Exportへ絶対Pathを出す場合は、
ADR-0022のRedaction契約を適用する。

Hashは偶発的破損と入力一致を検出するIntegrity情報であり、署名または信頼境界ではない。M16はCode Signing、Authenticode Policy、
敵対的Package Sandboxを決定しない。DLLは全Manifest検証と互換性検査の後だけLoadする。

## Consequences

### Positive

- Current DirectoryとProject Sourceから独立したStandalone起動を検証できる
- Startup SceneのStable Identityを維持しながらLocator変更を許容できる
- 一般Asset Pipelineを待たずに最初のGame制作Loopを完成できる
- ManifestのSize／Hashと固定Roleにより不足、混在、破損をLoad前に拒否できる
- Sibling Stagingと置換なしRenameにより部分Packageと既存Outputの破壊を避けられる
- Canonical Runtime DataとManifestにより再生成とRelocationを自動比較できる

### Trade-offs

- Source SceneとRuntime Sceneの二つのSerializer／Validatorを保守する必要がある
- 一般AssetIdを導入しないためM16 PackageはStartup Scene以外のAsset参照を扱えない
- Destinationを置換しないため、同じ名前への再Packageは新しいDestination選択が必要になる
- JSON Runtime Dataは最終的な高性能Runtime Asset形式ではなく、Parse CostとFile Sizeが残る
- 全FileのHash再検証はPackage作成時間と起動時間を増やす
- App-local Dependencyの明示InventoryをBuild構成ごとに維持する必要がある

### Mitigations

- M16 Runtime Dataを最小Sceneへ限定し、一般Asset Cookと同じAPI名を使用しない
- Manifest／Runtime DataのParserとWriterをUIから分離してHeadless Testする
- HashとParse時間をM16 Gateで測定し、最適化は測定結果を伴う後続Issueにする
- Package入出力をImmutable SnapshotとOperation-owned Stagingに限定する
- 将来のBinary FormatはVersion付き明示変換として追加し、schema version 1の意味を変更しない

## Rejected Alternatives

### `CueProject.json`と`.cuescene`をPackageへそのままCopyする

Authoring用Extension、未知Data、Source Locator、Migration契約をRuntimeへ持ち込み、Source／Runtime分離に反するため採用しない。

### Startup SceneをPackage相対Pathだけで識別する

Renameで恒久参照が変わり、Descriptor、Manifest、Scene内容のIdentity一致を検証できないため採用しない。

### M16で一般`AssetId`とAsset Databaseを確定する

Texture、Mesh、Audio、Dependency、Import設定の要件なしに永続IdentityとDatabaseを固定するため採用しない。

### RuntimeがManifest欠損時にProject RootまたはCurrent Directoryを探索する

開発Machineでは動作して配布先で失敗する構成を成功にし、Manifest外Fileを実行するため採用しない。

### 最終DestinationへFileを順番にCopyする

途中失敗やCancelで部分Packageが最終Outputとして観測され、旧成功Outputと混在するため採用しない。

### 既存Destinationへ上書きまたはDirectory Mergeする

Windowsで非空Directory全体の置換を一つのAtomic Renameとして扱えず、新旧File混在とRollback範囲拡大を招くため採用しない。

### Cross-volume Copy後のSource削除をAtomic Publishと呼ぶ

Copy途中の可視状態と電源断境界がRenameと異なり、旧出力保全を証明できないため採用しない。

### Package ManifestへTimestamp、Machine Path、Operation IDを保存する

同じ入力からの再生成結果が変わり、Package移動と再現性の検証を妨げるため採用しない。

### PackageごとにRuntimeHostを再Compileする

ADR-0022の共通HostとGame Module境界を崩し、Engine SourceとProject Sourceを再び一つのBuildへ結合するため採用しない。

## Validation Contract

M16では次を検証する。

- Project Descriptor version 2がnullまたはIdentityとSource Locatorを持つ`defaultScene`だけを受理する
- version 1から2へのMigrationがSource Fileを変更せず、失敗時に元Descriptorを維持する
- Blank ProjectがDescriptorと一致するDefault SceneをAtomicに生成する
- Scene移動後も`SceneAssetId`を維持し、Locatorだけを更新できる
- DescriptorとScene Fileの`SceneAssetId`不一致をPackage前に拒否する
- Raw `.cuescene` Byte列がPackageへ含まれない
- 未対応Component、Field、Schema Versionを黙って省略せずPackageを失敗させる
- 同じSnapshotからRuntime Project／Scene DataをByte単位で決定的に生成する
- Manifest version 1の必須Member、Role、Path、Size、Hash、Resource LimitをWriterとReaderで一致させる
- 絶対Path、Drive、UNC、Traversal、予約Device名、Case Alias、Reparse Pointを拒否する
- RuntimeHost、Game Module、Metadata、Runtime Dataの欠損、重複、Size／Hash不一致をDLL Load前に拒否する
- RuntimeHostのLoad-time Importが`/DEPENDENTLOADFLAG:0x800`でSystem Directoryへ限定され、Game Module依存解決が検証済み
  `Runtime/`とSystem Directory以外を検索しない
- Debug、Development、ReleaseのArtifactとRuntime Dependencyが混在しない
- Artifact Shared Read Lease中に参照VersionをCleanupできず、Copy後にLeaseを解放する
- Publish前失敗とCancelがDestinationを作成せず、Operation-owned StagingだけをCleanupする
- 既存DestinationをMerge、上書き、削除しない
- Cross-volume DestinationをAtomic Publishとして扱わない
- `PublishedButDurabilityUnknown`を自動Run可能な成功にしない
- Executable相対でPackage Rootを解決し、Current Directoryを変更しても起動できる
- Package移動後にManifestからGame ModuleをLoad、Start、Update、Stop、Unloadできる
- Project SourceとWorkspaceを参照不能にしたProcess Testでも起動できる
- 同じ入力から再生成したManifestとRuntime DataのHashが一致する
- PackageにAsset Import／Cook、ECS改良、Game Rendering、Sound、Effect、Physicsを追加していない

## Implementation Sequence

1. #229でProject Descriptor version 2、Startup Scene Reference、Blank Default Scene、Migrationを実装する
2. #230でCanonical Runtime Project／Scene Data Publisherを実装する
3. #231でPackage ManifestとRuntime Dependency Inventoryを実装する
4. #232でSibling Staging、Atomic Publish、Rollback、Outcomeを実装する
5. #233でExecutable相対Discovery、Manifest検証、Game Module、Startup SceneをRuntimeHostへ接続する
6. #234でBuild、Package、Run、Cancel、StopをEditor Application ServiceとUIへ接続する
7. #235で3構成のRelocation、Reproducibility、Failure Injectionを検証する
8. #236でBlank Game Templateと利用手順を現行UIへ一致させる
9. #237でM12からM16の最初の利用可能な制作Loopを完了判定する

## Follow-up

- 一般Asset Pipeline ResearchでAssetId、Source／Runtime Mapping、Dependency Graph、Import／Cookを決定する
- Startup Scene以外のRuntime Asset参照は一般Asset Pipelineの契約後に追加する
- Binary Runtime Scene、Compression、Incremental Package、CASは測定と要件を伴う別Issueで決定する
- Code Signing、Installer、Patch、DLC、Remote DeployはM16の対象外とする
- M16完了後、ユーザー指示に従い、冗長性、誤り、Performance阻害を実測と差分根拠に基づいて別監査する

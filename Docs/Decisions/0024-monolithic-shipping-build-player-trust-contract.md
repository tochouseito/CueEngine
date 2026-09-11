# ADR-0024: Monolithic Shipping Build and Player Trust Contract

- Status: Accepted
- Date: 2026-09-11
- Decision Owners: CueEngine Project

## Context

M15はProject固有C++ Sourceを`CueGameModule.dll`へBuildし、固定C ABIを介して共通の
`CueRuntimeHost.exe`へ接続する契約を定めた。M16は、そのRuntimeHost、Game Module、Runtime Dataを
Manifestで列挙し、移動可能なStandalone PackageとしてAtomicに公開する契約を定めた。

この構成はEditorからのBuild、診断、Module単位の失敗分離に適している。一方、プレイヤーへ公開する製品では、
Project Code DLLを差し替え可能な単独Fileとして配布する必要がなく、開発用Metadata、Loader、Module用Directoryも
製品の攻撃面と運用項目を増やす。製品Buildは開発者のIterationよりも、配布Inventoryの小ささ、誤配置耐性、
実行前検証、Publisher Identity、更新可能性を優先する必要がある。

本ADRは、開発時のModular経路を残したまま、Release構成へPlayer向けMonolithic Shipping Productを追加する。
Build Target、ProjectとEngineのLink境界、Game Module ABI再利用、RuntimeHost分割、Artifact Identity、Package Manifest v2、
PE Hardening、署名境界、Editor Workflow、Compatibilityを決定する。

本ADRはADR-0022の「Static Linkだけを正式経路にしない」という判断を維持する。開発用の正式経路は引き続きDLLである。
同時に、ADR-0022の「ProjectごとのRuntimeHost再LinkをM15/M16では採用しない」と、ADR-0023の
「PackageごとにRuntimeHostを再Compileしない」というScope限定の不採用判断を、M17のPlayer向け製品に限って更新する。
ADR-0022のABI v1とADR-0023のManifest v1の意味は変更しない。

## Current Requirements

- Debug／Development／Releaseは最適化と診断のConfigurationとして維持する
- 開発・診断では`CueRuntimeHost.exe + CueGameModule.dll`を維持する
- Player向け製品ではEngine RuntimeとGame Codeを一つのExecutableへStatic Linkする
- Shippingを第四のConfigurationにせず、Build Targetとして表現する
- Game Module ABI v1のSchema、System登録、Lifecycle、Error契約をDynamic／Staticで共用する
- Static経路ではGame Codeの`LoadLibraryExW`、`GetProcAddress`、`FreeLibrary`を使用しない
- Player PackageへGame DLL、Import Library、Static Library、PDB、Source、Build Logを含めない
- Manifest v1 Modular Packageを壊さず、Monolithic PackageをVersion付き形式で表現する
- Release ProductのPE Security設定とImportをPackage公開前に検査する
- Hashを署名またはPublisher Trustとして扱わない
- Private KeyまたはPFXをRepository、Project、Packageへ保存しない
- 新しい第三者Libraryを導入しない
- Editor Play、Asset Pipeline、Renderer、Sound、Effect、Physics、ECSを変更しない

## Reference Comparison

| Reference | 参考にする点 | CueEngineで採用しない点 |
| --- | --- | --- |
| Unreal Engine | Development用Modular TargetとShipping用Monolithic Targetを用途で分ける考え方 | Unreal Build Tool、Build.cs、Module Graph、UObject、Pak、署名実装を移植しない |
| Unity | Player BuildをEditor実行環境から分離し、配布成果物をBuild Operationとして扱う考え方 | Managed Runtime、IL2CPP、Unity Player設定形式を導入しない |
| Godot | EditorとExported Projectの責務を分け、Export前にPresetと成果物を検証する考え方 | Export Template、PCK、GDExtension形式を移植しない |
| SOL-AVES | 開発時のIteration経路と製品Runtimeの性能・安全境界を別に最適化する考え方 | Runtime、Asset Pipeline、Task Graph、独自ToolのSourceまたは構造を取り込まない |
| Legacy CueEngine | Editor実行と製品実行の成果物を区別する必要性 | 旧Build Script、旧DLL Loader、旧Runtime構成をコピーまたは移植しない |
| TheatriaEngine | CMake TargetとしてGame成果物を明示する考え方 | Machine固有Pathと通常Build／Reloadの混在を採用しない |

参考Engineは設計比較にのみ使用する。公開Source、Sample、記事からCodeをコピー、移植、改名、部分抽出しない。

Security設定の仕様確認には次の公式資料を使用する。

- [Microsoft: `/guard` (Enable Control Flow Guard)](https://learn.microsoft.com/en-us/cpp/build/reference/guard-enable-control-flow-guard)
- [Microsoft: `/CETCOMPAT` (CET Shadow Stack compatible)](https://learn.microsoft.com/en-us/cpp/build/reference/cetcompat)
- [Microsoft: `/DEPENDENTLOADFLAG` (Set default dependent load flags)](https://learn.microsoft.com/en-us/cpp/build/reference/dependentloadflag)
- [Microsoft: Verifying the Signature of a PE File](https://learn.microsoft.com/en-us/windows/win32/seccrypto/example-c-program--verifying-the-signature-of-a-pe-file)

## Decision

### Delivery Models

CueEngineは次の二つを正式な異なるDelivery Modelとして扱う。

```text
Modular Development
  CueRuntimeHost.exe
    `-- CueGameModule.dll

Monolithic Shipping
  CueGameProduct.exe
    |-- Engine Runtime static objects
    `-- Game Module static objects
```

Modular DevelopmentはEditorでの短いIteration、Module検証、診断、将来の明示的Reload Researchに使用する。
Monolithic ShippingはPlayerへ公開するRelease製品に使用する。片方を他方のFallbackとして暗黙選択しない。

初期の製品File名は固定の`CueGameProduct.exe`とする。Project Display Nameは任意Unicodeを含み得るため、
File名、CMake Target名、Export Symbolの正本にしない。製品名の安全なCustom Nameは別のVersion付き契約で追加できる。

### Configuration and Build Target Axes

ConfigurationとBuild Targetを直交させる。

| Build Target | Debug | Development | Release |
| --- | --- | --- | --- |
| `GameModule` | Supported | Supported | Supported |
| `ShippingProduct` | Rejected | Rejected | Supported |

`Release`はOptimization、Debug Information、Runtime Library等のCompiler／Linker設定を示す。
`ShippingProduct`はLinkage、Artifact、Package、Trust Policyを示す。`Shipping` Configurationは追加しない。

Build PlanはTargetを必須Identityとする。ShippingProductでは`minimumTrustMode`と`publisherKeyId`も必須Build入力とし、
Workspace Key、Binary Tree、Lock、Candidate、Artifact Version、`Current.json`をConfiguration、Target、Trust Identityで分離する。
初期Pathは次を正本とする。

```text
Generated/Build/<target>/<workspace-key>
Generated/Build/Locks/<target>/<workspace-key>.lock
Generated/Build/Candidates/<target>/<operation-id>
Generated/Artifacts/<target>/<configuration>/<variant-key>/Versions/<artifact-id>
Generated/Artifacts/<target>/<configuration>/<variant-key>/Current.json
```

GameModuleの`variant-key`は`modular`、UnsignedLocal ShippingProductは`unsigned-local`、PublisherSigned ShippingProductは
`publisher-<publisherKeyIdの先頭16文字>`とする。短縮値はDirectory分離にだけ使用し、完全な`publisherKeyId`をBuild Plan、
Workspace Key互換入力、Candidate Metadata、Artifact Metadata、`Current.json`選択条件へ保存して完全一致を要求する。
`minimumTrustMode`は`UnsignedLocal`または`PublisherSigned`とし、Workspace Key互換入力へ含める。

旧M15 Layoutの既存GameModule ArtifactはReader Compatibilityの対象とするが、新規WriterはTargetを含むLayoutだけへ書く。
旧Layoutから新Layoutへ移動、削除、上書きを自動実行しない。

Build Profile schema version 1は`target = GameModule`だけを表す既存形式として読取りを維持する。
schema version 2は`schemaVersion`、`configuration`、`target`、`minimumTrustMode`、`publisherKeyId`を各一つ要求する。
GameModuleでは後二つをJSON null、ShippingProductではReleaseと有効な組合せだけを許可する。
UnsignedLocalは`minimumTrustMode = UnsignedLocal`かつ`publisherKeyId = null`、PublisherSignedは
`minimumTrustMode = PublisherSigned`かつ64文字lowercase SHA-256の`publisherKeyId`を要求する。
v1 Fileを読んだだけで書き換えず、Userが保存した時点でv2 Writerを使用する。未知Version、未知Target、無効な組合せは拒否する。

### Project and Engine Build Boundary

生成Projectは次のTargetを持つ。

- `CueGameModule`: 開発用SHARED Library
- `CueGameModule.Static`: 製品Link入力となるSTATIC Library
- `CueGameProduct`: Release専用Executable

`CueGameModule`と`CueGameModule.Static`は同じUser所有Game SourceをCompileし、同じABI Query実装を使用する。
一つのTargetのObject Fileを異なるRuntime LibraryまたはCompile Definitionで無条件共有しない。

`CueGameProduct`はBuild Planの`minimumTrustMode`と`publisherKeyId`をRead-only Dataへ埋め込む。
UnsignedLocal Productは`minimumTrustMode = UnsignedLocal`とKeyなし、PublisherSigned Productは
`minimumTrustMode = PublisherSigned`と固定`publisherKeyId`を持つ別Build Identityとする。
PublisherSigned Productは`UnsignedLocal` Manifestを必ず拒否し、Manifestの書換えだけで署名必須Policyを降格できない。
UnsignedLocal Productは`PublisherSigned` Manifestを受理せず、署名済みProductの代用にならない。

M17のMonolithic Productは、Project Configure時にOperation入力として渡された`CUE_ENGINE_ROOT`から、
Engine-owned Shipping Link BridgeをCMake `add_subdirectory`で追加し、必要なFirst-party Runtime TargetをProject Binary Tree内でBuildする。
共有Projectの`CMakeLists.txt`と`CMakePresets.json`へEngineの絶対Pathを書かない。Engine SourceをProject RootへCopyせず、
Game SourceをEngine Source Treeへ生成しない。Build PlanはEngine Commit、Build Policy Version、Toolset、Architectureを記録する。

Engine Binary SDKのInstall／Export契約はM17では導入しない。現状のLibrary群にはInstall Interface、Binary Compatibility、
Runtime Library、Compile Definitionの公開契約がなく、未定義の`.lib`集合をProjectへ渡す方が危険なためである。
将来Binary SDKを追加する場合はVersion、Toolchain、Configuration、ABI、License、Symbol、Patch配布を別ADRで決定する。

Player PackageへEngineまたはGameの`.lib`を含めない。Static LibraryはBuild Tree内のLink入力であり、Player Runtime Artifactではない。

### Game Module Static ABI Entry

`CueGameModuleApiV1`、`cue_game_module_query`、Result Code、登録順、Module／System state、破棄Callbackの契約は
ADR-0022のABI v1を維持する。構造体Layout、Version値、Calling ConventionをMonolithic専用に変更しない。

ABI Headerへ`CUE_GAME_MODULE_STATIC`を追加する。Windowsでは次のVisibilityを使用する。

- `CUE_GAME_MODULE_BUILD`: DLL Build時の`__declspec(dllexport)`
- 定義なし: DLL Consumer時の`__declspec(dllimport)`
- `CUE_GAME_MODULE_STATIC`: Static Producer／Consumer時にimport／export属性なし

同時に`CUE_GAME_MODULE_BUILD`と`CUE_GAME_MODULE_STATIC`が定義された場合はCompile Errorとする。
ABI文書中の「DLL所有」は、共通契約では「Game Module所有」と読み替える。Dynamic ProviderはDLLのLoad Lifetime、
Static ProviderはProcess LifetimeのCodeを持つが、Module HandleとSystem StateはどちらもGame Moduleの破棄Callbackで破棄する。

Static ProviderはLink済み`cue_game_module_query`を直接呼ぶ。Game Code取得に`LoadLibraryExW`、`GetProcAddress`、
`FreeLibrary`、Environment Variable、Current Directory探索、Fallback Module名を使わない。

### RuntimeHost Core and Providers

RuntimeHost処理を次のOwnershipへ分離する。

```text
Cue.RuntimeHost.Core
  |-- Manifest and runtime data validation
  |-- Game Module ABI registration adapter
  |-- RuntimeApplicationSession startup and shutdown
  |-- Window and main loop coordination
  `-- common diagnostics

Cue.RuntimeHost.Dynamic.Windows
  `-- verified DLL load, identity pinning, query, unload

CueRuntimeHost.exe
  `-- Dynamic Provider

CueGameProduct.exe
  `-- Static Provider + CueGameModule.Static
```

Dynamic Provider固有SourceとそのImportはShipping ProductのLink closureへ含めない。単に未使用Branchへ置くだけではなく、
別Targetまたは別Object Fileで分離する。

Providerは検証済み`CueGameModuleApiV1`を共通Registration Adapterへ渡す。Adapter以降のSchema、Component、System Factory、
Project Scope、Session、Startup Scene、Start／Update／Stop順は同一とする。初期化途中失敗では完了済みStepを逆順にRollbackする。
Dynamic Providerは全State破棄後にDLLをUnloadする。Static ProviderはUnload操作を持たない。

### Shipping Artifact Contract

ShippingProductの成功ArtifactはGameModule Artifactと異なるKindおよびStoreに公開する。最低限次を記録する。

- schema version
- artifact kind `ShippingProduct`
- Engine Version、Engine Commit、Engine Source Treeのclean／dirty状態
- Project Identity
- Architecture `x64`
- Configuration `Release`
- Build Policy Version
- `minimumTrustMode`と完全な`publisherKeyId`またはnull
- Product EXEの相対File名、Size、SHA-256
- 開発側PDBの存在、Size、Hash、Locatorを含まないSymbol Identity
- PE Machine、Security Flags、Import Inventory、Validation結果
- Engine Source、Game Source、CMake入力、Compiler入力のProvenance

`PublisherSigned` Artifactは、Engine Source Treeが記録CommitのTreeと一致し、追跡済み変更とBuild入力になる未追跡Fileが
存在しない場合だけ生成できる。`UnsignedLocal`はdirty Engine Sourceを許可できるが、dirty状態をMetadataへ記録し、
Public Readyへ昇格しない。

Artifact Storeへ公開するProduct EXEは最終Byte列とする。UnsignedLocalでは未署名Candidateを検証して公開する。
PublisherSignedではOperation-owned Candidate内のEXEへAuthenticode署名とTimestampを適用し、署名後のPE、Trust、Size、Hashを
再検証してから不変Artifactとして公開する。Artifact Store内のEXEへ後から署名、追記、置換を行わない。
Package PublisherはArtifactをShared Read Leaseで固定してByte単位にCopyし、Artifact MetadataのSize／Hashと一致させる。
ManifestのHashはCopy先の同じ署名済みByte列から計算する。Manifest CMS署名はPackage Staging所有のManifestへ適用し、
Shipping ArtifactのEXEまたはMetadataを変更しない。

Game ProjectはGit Repositoryであることを要求しない。Publisherは`Source/Game`以下の通常File、Project Rootの
`CMakeLists.txt`、`CMakePresets.json`、`CueProject.json`をRoot相対PathのUTF-8 Byte順に列挙し、Path、Size、SHA-256から
Canonical Source Inventory Hashを生成する。Reparse Point、Root外参照、列挙中のIdentity変化を拒否する。

`PublisherSigned` BuildはSource Treeを直接Compiler入力にしない。Build開始時に、記録Commitから復元したEngineの追跡済み
Build入力と、共有なしHandleで一度だけ読み取ったGame Source／Project Build入力をOperation-owned Source Snapshotへ複製する。
Snapshot生成中は各入力のFile Identity、Size、HashをHandleから検証し、Pathの再Open結果へ依存しない。CMake ConfigureとBuildは
Snapshot内のPathだけをSource入力として使用し、元のEngine RootまたはProject RootをSource／Include Pathへ含めない。
SnapshotはBuild Operation以外から書込み可能な共有を持たず、生成完了後に封印し、Build前後でInventory Hashを検証する。
Snapshotの変更、元Sourceへの参照、Inventory不一致を検出した場合は署名もArtifact公開も行わない。

Toolchain、Windows SDK、vcpkg Install TreeはSnapshot外の信頼済みBuild Environment入力とする。VersionとManifest／Baseline Hashに加え、
実際に解決したCompiler、Linker、Header、LibraryのIdentityをProvenanceへ記録する。同じUser権限の悪意あるProcessがBuild Environmentや
Operation-owned Snapshotを書き換えられる環境は信頼済み署名環境ではない。Public Distribution用Buildは、Access ControlとProcess分離を
備えた専用Build Agent上で行い、開発者Desktop上の署名成功だけをPublic ReadyのEvidenceにしない。

`UnsignedLocal` Buildは従来どおりSource TreeからBuildできるが、開始時とBuild完了後に同じInventory Hashを再計算し、
Build中の入力変更を拒否する。この二回のHash確認はLocal診断用であり、PublisherSigned Provenanceの代用にしない。

ProvenanceはEngine Commitとclean状態、Game Source Inventory Hash、CMake Version／Generator、MSVC Compiler Version、
Windows SDK Version、Architecture、Configuration、Build Policy Version、vcpkg Manifest／Baseline Hashを記録する。
Machine絶対Path、User名、Environment全体、Credentialは記録しない。

Shipping PublisherはMetadataをEngine構成時の定数だけから組み立てない。Project Binary Treeの`CMakeCache.txt`、
`CMakeFiles/<CMakeVersion>/CMakeCXXCompiler.cmake`、生成済み`CueGameProduct.vcxproj`を上限付きで読み、実際に選択された
CMake実行File／Version／Generator／Visual Studio Instance／`CUE_ENGINE_ROOT`、C++ Compiler／Version／Architecture、`PlatformToolset`、
Windows SDK VersionをBuild PlanおよびEngine構成時の信頼済みIdentityと照合する。Compiler実体のSHA-256を含む照合済みの値だけを
Metadataへ記録し、絶対Pathは記録しない。不一致、重複値、欠落、未知ArchitectureはArtifact公開前に拒否する。

Visual Studioの既定minor Toolset更新で再利用中のBinary Treeが別Compilerへ切り替わらないよう、Engine構成時に選択した
MSVC Toolset Directory VersionをBuild Runnerの必須入力とする。ConfigureはCMakeへ`-T version=<version>`を渡し、Buildは
MSBuild Global Property `/p:VCToolsVersion=<version>`を渡して同じVersionを固定する。Publisherは
`CMAKE_GENERATOR_TOOLSET`、`VCToolsVersion`、CMake Compiler Path／Version、Compiler実体のFile Version／SHA-256を相互照合する。
この契約導入時はEngine Build Policy Versionを2へ更新し、固定前のBinary Treeを互換再利用しない。

Artifact選択用`Current.json`のschema version 1は、M15のGameModule用旧形式として意味と読取互換性を維持する。
version 1は`schemaVersion`、`artifactId`、`configuration`、`files`だけを持ち、File Entryに用途を持たない。
version 2は`schemaVersion`、`artifactId`、`configuration`、`target`、`minimumTrustMode`、`publisherKeyId`、`files`を必須とし、
各File Entryへ`purpose`を持つ。新規Publisherは常にversion 2を書き、Readerはversion 1をGameModuleにだけ許可する。
Readerは旧Manifestを自動書換えせず、未知／重複／余分／欠落Memberを拒否し、読取後にFile用途を推測しない。

この移行のため`BuildArtifactInventory`は完全な`BuildProfile`を値として所有し、公開`profile()`でTarget、Configuration、
Trust Identityを返す。既存利用側向けの`configuration()`は互換Accessorとして`profile()`内のConfigurationを返す。
これによりGameModuleのversion 1読取境界を限定したまま、ShippingProductのversion 2を同じReader Lease契約で扱う。

PDBはArtifact Storeの開発側Symbol領域へ保持できるが、Package Publisherの入力Roleにしない。
Candidate検証またはPublishが失敗した場合、以前の成功`Current.json`を変更しない。取消は`Current.json`のAtomic Replace直前まで
受理する。Candidateを不変VersionへRenameした後に取消された場合は、以前の`Current.json`を保持し、公開済みだが未選択のVersionを
後続Cleanup対象として残す。`Current.json`の置換が可視になった後は、呼出側の取消より可視状態の検証とDurability結果を優先する。

### Package Manifest Version 2

`CuePackage.json` schema version 1はModular Package専用の既存形式として意味を維持する。
schema version 2は`executionModel`を必須とし、M17 Writerは`monolithic`だけを生成する。

schema version 2はUTF-8、BOMなし、LF、末尾改行ありのCanonical JSONとする。WriterのTop-level Member順、
Object内Member順、`files`整列順を次の例どおりに固定する。ReaderはMember順と意味を持たない空白には依存しないが、
必須Memberの欠落、重複、未知Member、型不一致、末尾Dataを拒否する。

```json
{
  "schemaVersion": 2,
  "projectId": "12345678-1234-4abc-8def-1234567890ab",
  "engineVersion": "0.1.0",
  "architecture": "x64",
  "configuration": "Release",
  "executionModel": "monolithic",
  "startupScene": {
    "sceneAssetId": "22345678-1234-4abc-8def-1234567890ab",
    "runtimeDataPath": "Data/Scenes/22345678-1234-4abc-8def-1234567890ab.cueruntime.json"
  },
  "applicationExecutable": "CueGameProduct.exe",
  "trust": {
    "mode": "PublisherSigned",
    "publisherKeyId": "0000000000000000000000000000000000000000000000000000000000000000",
    "manifestSignaturePath": "CuePackage.signature.p7s"
  },
  "files": [
    { "role": "applicationExecutable", "path": "CueGameProduct.exe", "sizeBytes": 1, "sha256": "0000000000000000000000000000000000000000000000000000000000000000" },
    { "role": "projectRuntimeData", "path": "Data/CueProject.runtime.json", "sizeBytes": 1, "sha256": "0000000000000000000000000000000000000000000000000000000000000000" },
    { "role": "startupSceneRuntimeData", "path": "Data/Scenes/22345678-1234-4abc-8def-1234567890ab.cueruntime.json", "sizeBytes": 1, "sha256": "0000000000000000000000000000000000000000000000000000000000000000" }
  ]
}
```

Top-level Memberと型を次に固定する。

| Member | Type | Contract |
| --- | --- | --- |
| `schemaVersion` | JSON integer | `2`だけを受理する |
| `projectId` | string | lowercase UUID v4、nilを拒否する |
| `engineVersion` | string | ADR-0013のCanonical `major.minor.patch` |
| `architecture` | string | `x64`だけを受理する |
| `configuration` | string | `Release`だけを受理する |
| `executionModel` | string | `monolithic`だけを受理する |
| `startupScene` | object | `sceneAssetId`と`runtimeDataPath`を各一つ要求する |
| `applicationExecutable` | string | `CueGameProduct.exe`だけを受理する |
| `trust` | object | `mode`、`publisherKeyId`、`manifestSignaturePath`を各一つ要求する |
| `files` | array | 3 Entryを要求し、Root相対PathのUTF-8 Byte順とする |

`startupScene.sceneAssetId`はlowercase UUID v4でnilを拒否し、Runtime Project Dataの`startupSceneAssetId`および
Runtime Scene Dataの`sceneAssetId`と一致させる。`startupScene.runtimeDataPath`は対応する
`startupSceneRuntimeData` EntryのPathと一致させる。

`trust.mode`は`UnsignedLocal`または`PublisherSigned`とする。`UnsignedLocal`では`publisherKeyId`と
`manifestSignaturePath`をJSON nullとする。`PublisherSigned`では`publisherKeyId`を署名Certificate Public Keyの
DER SubjectPublicKeyInfoに対する64文字lowercase SHA-256、`manifestSignaturePath`を固定値
`CuePackage.signature.p7s`とする。他の組合せを拒否する。

`files` Entryは`role`、`path`、`sizeBytes`、`sha256`を各一つ要求し、未知Memberを拒否する。
Monolithic Packageは次のRoleを各一件要求し、それ以外を拒否する。

| Role | Count | Fixed Path or Rule |
| --- | --- | --- |
| `applicationExecutable` | exactly 1 | `CueGameProduct.exe`、top-level `applicationExecutable`と一致 |
| `projectRuntimeData` | exactly 1 | `Data/CueProject.runtime.json` |
| `startupSceneRuntimeData` | exactly 1 | `startupScene.runtimeDataPath`と一致 |

`gameModule`、`gameModuleMetadata`、`runtimeHost`、`runtimeDependency`を禁止する。追加Runtime Data Roleは後続の
Asset Pipeline契約まで導入しない。EntryのPath、Size、Hash、整列、ASCII case-insensitive Alias、Relative Path、
Reparse Point、Root境界の契約はADR-0023 schema version 1と同じとする。`sizeBytes`は0以上8 GiB以下の指数表現なし
JSON整数、`sha256`は対象Byte列の64文字lowercase SHA-256とする。

Manifest File上限1 MiB、JSON nesting 16、UTF-8 decode後の一String 64 KiB、File Entry 3、相対Path1,024 UTF-8 byte、
Path Segment 32、1 File 8 GiB、全File合計16 GiB、Runtime Project Data 1 MiB、Runtime Scene Data 64 MiB、
Runtime Scene Object 1,000,000をschema version 2のResource Limitとする。Product PEはOverlayを含め512 MiB以下でなければ
PE専用検証を開始せず拒否する。加算と件数のOverflowを検査する。

`CuePackage.signature.p7s`はManifest自身の自己参照と循環署名を避けるため`files`へ含めない。
`PublisherSigned`ではPackage Root直下に通常Fileとして一件だけ存在することを要求し、Size上限を256 KiBとする。
`UnsignedLocal`では同名Fileの存在を拒否する。Package Root以下を再帰列挙し、Manifest、任意Signature、`files`の
列挙対象以外の通常Fileを拒否する。列挙されたFileの親として必要な通常Directoryだけを許可し、Reparse Point、
空の余剰Directory、未知Entryを拒否する。

ProductのLoad-time ImportはManifest検証より前にWindows Loaderが処理する。このためM17のPackage RootへApp-local DLLを配置せず、
Product EXEの直接ImportをVersion付きAllowlistに含まれるWindows System DLLとMSVC Runtimeだけに限定する。
未知ImportまたはApp-local Dependencyを必要とするProductはPublish前に拒否する。

Manifest v2はv1へFallbackせず、Runtime時にv1からv2へMigrationしない。Executableは自身のFile名と同じDirectoryから
Manifestを解決し、Current Directory、Project Root、Build Tree、Environment Variableを探索しない。

### C/C++ Runtime Policy

M17はDevelopment／Releaseで現在のDynamic MSVC Runtime `/MD`を維持する。Visual C++ RuntimeのSecurity Updateを
OSへInstallされたRedistributableから受けられ、Engine更新なしに修正できることをPlayer Security上優先する。
`/MT`は単一File配布を増やす利点があるが、CRT脆弱性修正のたびに全Gameを再Build／再配布する責任が生じるため採用しない。

MSVC Runtime DLLをPackageへCopyしない。対応x64 RedistributableがWindows System DirectoryへInstall済みであることを
実行前提とする。InstallerがRedistributableを導入する手順は別Issueとする。

### PE Hardening and Import Policy

Release Shipping ProductはCompilerとLinkerの両方でSecurity設定を有効にし、最終PEで結果を検証する。
少なくとも次をPolicyとして固定する。

- ASLRとHigh Entropy VA
- DEP／NX compatibility
- Control Flow GuardのCompile／Link設定とLoad Configuration
- CET compatibilityをToolchainがSupportする場合に必須化し、未対応を成功へ偽装しない
- Stack security check
- `/DEPENDENTLOADFLAG:0x800`
- x64 Machine、Release Configuration
- 既知System DLLとMSVC Runtimeだけの直接Import
- Game Module DLL名または未知App-local DLL Importの拒否

Build Optionを指定した事実だけで完了せず、最終EXEのPE Header、Load Configuration、Import TableをFirst-party Validatorで読む。
Toolchain VersionがFlagをSupportしない場合はPolicy Versionを変えず無視せず、Shipping Publishを失敗させるか、
ADR更新を伴う明示的Compatibility判断を行う。

### Executable and Content Signing Boundary

Monolithic化とSHA-256 Inventoryは攻撃面と偶発破損を減らすが、Publisher Identityまたは敵対的改ざん防止を提供しない。
公開可能なShipping Packageは次の信頼境界を満たす。

1. Artifact Publisherが最終`CueGameProduct.exe`をLinkし、PE HardeningとImport Policyを検証する
2. PublisherSignedではOperation-owned Candidate内で外部SignerがAuthenticode署名とTimestampを付与する
3. 署名後のEXEを再検証し、最終Byte列として不変Shipping Artifactへ公開する
4. Package Publisherが不変ArtifactをShared Read LeaseでStagingへCopyする
5. Copyした署名後EXE Byte列のSizeとSHA-256を含むManifest v2を生成する
6. 外部SignerがCanonical Manifest Byte列へDetached Signatureを付与する
7. Publisher Identity、Certificate Chain、Timestamp、Manifest Signature、InventoryをStaging内で再検証する
8. ADR-0023と同等のFlush、同一Volume Rename、公開後再検証を行う

SignerはWindows Certificate Storeまたは外部Signing Serviceを利用できるAdapter境界とし、Private KeyをCueEngineへ返さない。
証明書Thumbprint、許可Publisher、Timestamp Authority等はMachine／Organization固有設定として扱い、共有Project Sourceへ保存しない。
PFX、Password、Token、Private KeyをLog、Artifact Metadata、Manifest、Project、Repositoryへ保存しない。

Manifest Detached SignatureはDER encoded CMS／PKCS#7 SignedDataとし、detached contentを使用して
`CuePackage.json`のBOMなしUTF-8 Byte列全体を署名する。Digest AlgorithmはSHA-256だけを受理する。
Signer Public Key AlgorithmはRSA 2048 bit以上のPKCS#1 v1.5 SHA-256、またはECDSA P-256 SHA-256を受理する。
Signerは一件だけを要求する。Signed AttributeはCMS content type、message digest、signing timeだけを許可し、
Unsigned AttributeはRFC 3161 signature timestamp tokenだけを許可する。未知Attribute、弱いDigest、DetachedでないContentを拒否する。

Product Buildは許可Publisherの`publisherKeyId`を非Secret Build入力として受け取り、ProductのRead-only Dataへ埋め込む。
PublisherはProduct Authenticode SignerとManifest CMS SignerのDER SubjectPublicKeyInfo Hashが、埋込値およびManifestの
`publisherKeyId`とすべて一致することを検証する。Player Runtimeは自身へ埋め込まれた値とManifest値を比較し、
CMS Signatureを同じPublic Keyに対して暗号学的に検証する。Machine-local設定だけをPlayer Runtimeの信頼起点にしない。

AuthenticodeのCertificate Chain、Code Signing EKU、Timestamp、RevocationはPackage公開時にWindows Trust Policyで検証する。
期限切れCertificateは、有効なRFC 3161 Timestampが署名時刻をCertificate有効期間内と証明する場合だけ受理する。
Public Ready検証ではOnline Revocation確認を要求し、NetworkまたはRevocation Serviceへ到達できず結果を確定できない場合は
Fail-closedで公開しない。Player RuntimeはOffline実行を妨げないためChain／RevocationをNetwork再確認せず、埋込Public Keyとの一致、
CMS Signature、Manifest Inventoryを検証する。OS、署名済みInstaller／Launcher、App Control等がProduct EXE自体の
外部Trust Anchorとなる。

`PublicDistributionReady`には、実際の配布・起動経路が許可Publisherの署名を外部Trust Anchorとして強制することを必須とする。
例えば、OSが信頼する署名済みInstallerからAccess ControlされたInstall Rootへ配置し、Installer／LauncherまたはApp Controlが
Product Authenticode Signerを検証してから起動する。Packageを直接配布してUser書込み可能なDirectoryから任意実行できる状態、
あるいは検証がBuild時だけで起動時に強制されない状態は、AuthenticodeとCMSが有効でも`PublicDistributionReady`ではない。

Local Developmentでは、明示的な`UnsignedLocal` Trust Modeを選択してRelease Monolithic Packageを作成・実行できる。
その成果物はUI、Operation Result、Metadataで`NotPublishable`と表示し、公開可能成功へ昇格しない。
Test用Self-signed Certificateは署名機構のTestだけに使用し、Public Trust達成のEvidenceにしない。

M17は二つの完了状態を区別する。

- `MonolithicLocalReady`: Release Product、UnsignedLocal Package、Relocation、Tamper、起動、終了を検証済み
- `PublicDistributionReady`: 信頼された実運用CertificateでAuthenticode、Timestamp、Online Revocation、CMS署名、Publisher一致を検証し、
  許可Publisherを強制する外部Trust Anchor付き配布・起動経路を実機で検証済み

M17 Completion GateとMilestone Closeに必須なのは`MonolithicLocalReady`である。実運用Certificateが提供されない場合は
`PublicDistributionReady = false`をEvidenceとUIへ残し、「公開署名済み製品」「公開準備完了」と報告しない。
Test用Self-signed Certificateは機構のTestには使用できるが、`PublicDistributionReady`をtrueにしない。

### Atomic Publication and Runtime Verification

Manifest v2 PublisherはADR-0023のSibling Staging、同一Volume、Flush、置換なしAtomic Rename、公開後再検証、
`Committed`／`NotPublished`／`PublishedButDurabilityUnknown`を維持する。既存DestinationをMerge、削除、上書きしない。

RuntimeはExecutable相対RootからManifest v2を読み、Resource Limit、Schema、Execution Model、Project、Engine、Architecture、
Configuration、Role、Path、Size、Hash、Content Signature PolicyをFail-closedで検証する。Manifestと各Runtime DataはReparse Pointを
拒否してHandleで開き、最終PathとFile Identityを確認する。検証からSession読込み完了までWrite／Delete共有なしのHandleを保持し、
Size／HashとParser入力を同じHandleから取得する。ParserとSessionへは検証済みByte Snapshotまたは保持中Handleだけを渡し、
検証後にPathを再Openしない。これにより検証と使用の間の置換を拒否する。検証完了前にRuntime Dataを使用しない。

Executable自身のHashは、実行中の同じProcessが信頼の起点として自己申告するだけでは完全な敵対的置換を防げない。
OSのAuthenticode Policy、Installer／Launcher、App Control等の外部Trust Anchorと組み合わせる必要がある。
この制約を診断と文書へ明記する。

Game Save、User Settings、Log、Crash Data等のMutable DataはPackage Rootへ書かず、User Data領域へ保存する。
M17は新しいSave Systemを実装しないが、Package Rootへの書込みを正当化しない。

### Editor Workflow

Editorは既存のGameModule Build、Standalone Package／Run、F5 Playを維持し、別ActionとしてMonolithic Shipping Workflowを追加する。

```text
Build Shipping Product
  -> Validate Shipping Artifact
  -> Package Monolithic Product
  -> Validate Trust Policy
  -> Run Packaged Product
```

Shipping ActionはRelease固定とし、Debug／Developmentを選択できない。Build中、未保存Document、Stale Artifact、
Package Validation失敗、Signing Policy未成立を明確に表示する。`UnsignedLocal`はPublic Readyと同じ表示にしない。

Runは最終Packageの`CueGameProduct.exe`だけを起動し、Build Tree、Project Source、Current Directoryの別EXEへFallbackしない。
Stop、Exit Code、Process Capture、Job Object CleanupはM16契約を再利用する。

F5 PlayをRuntimeHost ProcessまたはShipping Productへ置き換えない。未保存Scene転送、Hot Reload、Editor Process分離は別Researchとする。

### Compatibility and Versioning

- Game Module ABIはversion 1を維持する
- Build Profile v1 Readerを維持し、v2 Writerを追加する
- GameModule Metadata v1はModular専用として維持する
- Shipping Artifact Metadataは別Kind／Versionを持つ
- Package Manifest v1はModular専用として維持する
- Package Manifest v2はMonolithic専用として追加する
- Runtime Data schemaはM16のVersionを維持する
- 未知Versionを新しいVersionとして推測して読まない
- v1 Modular Packageをv2 MonolithicへRuntime Migrationしない

### Diagnostics and Sensitive Data

Operation ResultはBuild Target、Configuration、Artifact Kind、Execution Model、Trust Mode、Validation Stage、Outcomeを含む。
絶対PathをUser向け表示に使えるが、Package、Canonical Manifest、共有Project Fileへ保存しない。Log Exportは既存Redaction契約を維持する。

Certificate Subject、Issuer、Thumbprint、Timestamp結果は診断へ記録できる。Password、Token、PFX Path、Private Key Handle、
署名Service Credential、Environment全体は記録しない。未知の署名失敗を「unsigned」と単純化せず、検証不能と署名なしを区別する。

## Consequences

### Positive

- Player PackageからProject Code DLLとそのLoader／Metadataを除外できる
- Development DLL経路を維持し、製品SecurityのためにIteration速度を犠牲にしない
- ConfigurationとDelivery Modelを分離し、Release DLLによる診断も継続できる
- Dynamic／Staticで同じ登録とLifecycle契約を共有できる
- Manifest v1を壊さずMonolithic Layoutを追加できる
- PE Hardening、Import、署名状態を機械的Gateにできる
- Static LibraryやPDBをPlayerへ配布しない境界が明確になる

### Trade-offs

- ProjectごとにEngine Runtimeを再Compile／再Linkするため、Shipping Build時間とArtifact Sizeが増える
- RuntimeHost共通処理とDynamic LoaderのSource／Target分離が必要になる
- Build Profile、Artifact、Packageに新しいVersionとTarget軸が増える
- Dynamic／Static二経路の回帰Testが必要になる
- `/MD`によりVisual C++ RedistributableのInstall前提は残る
- AuthenticodeとContent Signatureは証明書および外部Trust AnchorなしにPublic Trustを完成できない
- Monolithic EXEでも管理者権限を持つPlayerによるPatchを完全には防げない

### Mitigations

- ShippingProductをRelease専用にし、日常IterationではGameModule Targetを使用する
- 共通Registration／Lifecycle TestをProvider Parameterized Testとして実行する
- Source-based Link BridgeをVersion付きの小さいCMake境界に限定する
- v1を変更せず、新Writerだけをv2へ追加する
- Shipping ArtifactとPackageを不変Inventoryとして再検証する
- Public TrustとUnsigned Local BuildをUI、Metadata、Gateで分離する
- Build Time、Link Time、Binary Size、Startup Timeを同一条件でBaseline記録する

## Rejected Alternatives

### すべてのConfigurationをMonolithicへ変更する

開発時のBuild／Link時間を増やし、DLL単位の検証と診断経路を失うため採用しない。

### `Shipping`を第四のConfigurationとして追加する

最適化設定と成果物形態を一つの軸へ混在させ、Release DLLとRelease Productを表現できなくするため採用しない。

### Game Module ABIをStatic専用C++ Interfaceへ置き換える

Dynamic／Staticで登録順、所有権、Error契約が分岐し、検証範囲を増やすため採用しない。

### Manifest v1からGame Module Roleを省略可能にする

既存Versionの必須条件を変更し、古いReader／Writerと同じVersionで異なる意味を持つため採用しない。

### Game DLLをEXEへ埋め込み、起動時に展開してLoadする

最終的に書込み可能なDLLとLoaderを残し、Static Linkの攻撃面削減とInventory単純化を達成しないため採用しない。

### Engine `.lib`を未VersionのBinary SDKとしてProjectへ直接渡す

Install Interface、Toolset、Runtime Library、Compile Definition、ABIのConsumer契約が未決定で、偶然Linkできる状態を
正式APIとして固定するため採用しない。

### MSVC Runtimeを`/MT`で静的Linkする

File数は減るが、CRTのSecurity Updateを得るたびにGame Productの再Build／再配布が必要になるためM17では採用しない。

### Package RootへMSVC Runtime DLLを同梱する

OSより先にApp-local CopyをLoadさせる配置と更新責任を増やすため採用しない。正式RedistributableをSystemへInstallする。

### Hashだけで公開Packageを信頼する

攻撃者がFileとManifestを同時に置換でき、Publisher Identityを証明しないため採用しない。

### CueEngineがPFXまたはPrivate KeyをProjectへ保存する

Source管理、Log、Artifact、共有MachineへのCredential漏えいRiskを生むため採用しない。

### M17でF5 PlayをShipping Productへ統合する

未保存Scene、Process間転送、Reload、Editor診断の問題をShipping Buildへ混在させるため採用しない。

## Validation Contract

M17では最低限次を検証する。

- Build Profile v1 GameModuleを従来どおり読める
- Build Profile v2が二Targetを表現し、不正Configuration／Target組合せを拒否する
- Workspace、Lock、Candidate、ArtifactがTargetおよびTrust Identity単位で衝突しない
- Debug／Development／Release GameModule Buildが成功する
- Release ShippingProduct Buildが成功し、Debug／Development指定を拒否する
- Dynamic／Static ProviderでABI登録順、Module／System Lifecycle、Rollback結果が一致する
- Static ProductへDynamic Game Module Loader SourceまたはImportがLinkされない
- Game Module ABI v1のC11／C++20 CompileとLayoutが回帰しない
- Project共有FileへMachine絶対Engine Pathを書かない
- Shipping Artifact失敗時に旧成功Artifactを保全する
- PublisherSigned Artifactは署名後Byte列を不変公開し、Package時に変更しない
- PublisherSigned BuildをOperation-owned Source Snapshotだけから行い、元Source参照とSnapshot変更を拒否する
- UnsignedLocal BuildではEngine clean状態とGame Source InventoryをBuild前後で一致させる
- Manifest v1 Modular Packageを従来どおり検証・起動できる
- Manifest v2 Monolithic Packageが三つの必須Roleを各一件要求する
- v2へGame Module、Metadata、RuntimeHost Roleを混在させると拒否する
- Product PackageへDLL、LIB、PDB、Source、Build Logを含めない
- Product PEがx64、Release、ASLR、DEP、CFG、CET Policyを満たす
- Product ImportがVersion付きAllowlist内にあり、Game Moduleまたは未知App-local DLLを含まない
- `/DEPENDENTLOADFLAG:0x800`を最終PEで確認する
- EXE、Manifest、Runtime DataのSize／Hash不一致を検出する
- Runtime Dataを検証したHandleまたはByte Snapshotから使用し、検証後のPath再Openによる置換を許さない
- Signatureなし、異なるPublisher、壊れたSignature、期限またはChain不正をPublic Readyにしない
- 外部Trust Anchorが許可Publisherを実際の配布・起動経路で強制しない場合はPublic Readyにしない
- Unsigned Local BuildをPublic Readyと表示しない
- PublisherSigned ProductがUnsignedLocal Manifestを拒否し、Manifest変更でTrust Modeを降格できない
- UnsignedLocal ProductとPublisherSigned Productが異なるBuild／Artifact Identityを持つ
- Destination既存、Cross-volume、Flush、Rename、再検証失敗でAtomic Publish契約を維持する
- Packageを別Directoryへ移動し、Project Source、Editor、Engine Build Treeなしで起動できる
- Current DirectoryをPackage外へ変更しても同じProductを起動する
- Standard Userで起動し、Package RootへMutable Dataを書かない
- EditorからBuild、Package、Validate、Run、Stopを一貫して実行できる
- 既存F5 Play、Modular Standalone、Project Hub Workflowが回帰しない
- Build Time、Link Time、Binary Size、Startup Timeを条件付きBaselineとして記録する
- 実WindowでProduct起動と正常終了を確認する

## Implementation Sequence

1. #299でShippingProduct Target、Build Profile v2、Target別Workspace／Artifact Identityを追加する
2. #300でRuntimeHost CoreとDynamic／Static Query Providerを分離する
3. #301でDual Game Module Target、Engine Shipping Link Bridge、CueGameProduct Targetを生成する
4. #302でShipping Product Artifact PublisherとSymbol保管境界を実装する
5. #303でManifest v2とMonolithic Package Publisherを実装する
6. #304でPE Hardening、Import Validator、署名可能なTrust Policyを実装する
7. #305でEditor Shipping Build／Package／Run Workflowを統合する
8. #306でRelocation、Tamper、Security、実WindowEnd-to-Endを検証する
9. #307でM17 Completion Gateを再検証する

## Follow-up

- Binary Engine SDK、Installed CMake Package、Plugin SDKはABIとDistribution要件を伴う別Researchとする
- Installer、Updater、Visual C++ Redistributable導入、MSIXは配布方式の別Milestoneとする
- Production Certificate取得、HSM／Cloud Signing、Timestamp SLA、鍵RotationはOrganization運用として別途決定する
- DRM、Anti-cheat、Server AuthorityはGame要件が確定した後の別Scopeとする
- Runtime Plugin、Mod、Hot ReloadはMonolithic Shippingの暗黙例外にせず、明示的Trust Modelを持つ別ADRとする
- Asset Pipeline／Cook後はManifest v2を上書きせず、新しいRuntime Asset RoleとVersionを先行Researchで決定する

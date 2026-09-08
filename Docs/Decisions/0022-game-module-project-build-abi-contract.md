# ADR-0022: Game Module, Project Build, and ABI Contract

- Status: Accepted
- Date: 2026-09-08
- Decision Owners: CueEngine Project

## Context

M15では、Project固有のC++ SourceをCMakeでBuildし、再利用可能なGame Module Artifactとして扱う。
EditorはBuildを開始、Cancel、再実行し、LogとArtifactを表示するが、Build ToolやNative Processを直接操作しない。

ADR-0003はC++20、MSVC、Debug／Development／ReleaseのBuild構成を決定した。
ADR-0013はProject共有Data、Machine固有Workspace、Generated、Savedを分離した。
ADR-0015はCompiler型名と登録順から独立したSchema Identityを決定した。
ADR-0021はProject Scopeが不変System Factory集合を持ち、各Runtime Application SessionがSystem Instanceを所有することを決定した。

現行の`Cue.RuntimeHost`はEngine Source Tree内で構築される単一Executableであり、Project固有Sourceを入力に持たない。
現行ECSのComponent Storageは`World::register_component<T>`でC++型へCompile時にBindingされる。
この状態でProject SourceをEngine Targetへ直接追加すると、ProjectごとにEngine Hostを再Linkし、Engine SourceとGame Sourceの
責務、生成物、失敗時の保全境界が混在する。一方、C++ Class、STL Container、Exception、所有PointerをそのままDLLへ公開すると、
Compiler、Runtime Library、Configuration、Allocation Ownerの差が未定義動作または互換性事故になる。

本ADRは、Game Module Target、Engineとの依存方向、最小ABI、登録順とLifetime、Project SourceとBuild Outputの配置、
3構成の互換性、Editor Build Serviceの責務、失敗Artifactの扱いを決定する。
Game Moduleの実装、Hot Reload、Scripting、Asset Build／Cook、Runtime Packaging、ECS Storage改良は決定しない。

## Prior and Legacy Reference

### Current Rebuild Contracts

- CMakeを唯一の正式Build定義とし、生成されたIDE Projectを正本にしない
- RuntimeはEditor、ImGui、Platform固有型へ依存しない
- `SchemaRegistryBuilder`はCoreとProjectの定義を集約した後に一度だけSealする
- `RuntimeSystemRegistry`はSystem ID、Phase、Order、Dependency、登録順から実行順を固定する
- 一つのRuntime Application SessionがSystem Instanceを一意所有し、Startの逆順でStopする
- Project ScopeのFactoryはSession-local Mutable Stateを所有しない
- Project Root外のPathを暗黙に読書きせず、GeneratedとSavedを共有Sourceから区別する

本ADRはこれらを置換せず、Project DLLをProject Scopeへ接続する境界を追加する。

### Legacy CueEngine

旧CueEngineはScript DLLをBuildし、C ABIと関数Pointer Tableを介してEngine機能へ接続することで、
Engine本体を再BuildせずProject Codeを読込む問題を解いていた。DLL内で生成したObjectをDLL側関数で破棄し、
ABI Versionを検査する考え方も持っていた。

一方、ABIは多くのComponent、Object、Gameplay操作を一つのVersionへ追加し続け、変更影響と互換性検証の範囲が大きくなった。
Editor Build、Staging、PDB、Reload、Runtime APIが近接し、通常BuildとHot Reloadの所有境界も理解しにくかった。

新CueEngineでは単一Exportと明示Version、C互換値、同一Module内の生成／破棄という原則だけを現在要件から再設計する。
旧ABIの型、関数、Version値、Data Layout、Loader、Build Scriptはコピー、移植、改名、部分抽出しない。

検証は新規HeaderのC／C++単体Compile、誤Version／Configuration／Architectureの拒否、登録順、
SessionごとのInstance分離、Load／Unload順、Artifact保全Testで行う。

### TheatriaEngine

TheatriaEngineはTarget種別をBuild定義から生成し、DLLを別名へStagingしてNative Loaderで読込む短いIteration経路を持つ。
ただし、Machine固有PathをBuild定義へ含める構成と、通常起動、Editor状態、Hot Reloadを同時に扱うLoaderは、
再現可能なProject Buildと最小M15 Scopeには適さない。

CMakeからTargetを構成する点だけを参考にし、Generator、Loader、Path、Reload実装は使用しない。

## Current Requirements

- Project固有C++ SourceをEngine Sourceと物理的、論理的に分離する
- Projectを一つのGame Module DLLへBuildし、M16の汎用RuntimeHostから接続可能にする
- CMakeを唯一の正式Build定義とし、Visual Studio Solutionを生成物として扱う
- Game ModuleからEditor、ImGui、Project Hub、Platform Windows、RHI、D3D12へ依存しない
- RuntimeからGame Module実装へCompile時依存しない
- DLL境界にSTL、C++ Virtual Interface、Exception、所有Pointer、Allocatorを公開しない
- Schema、Component宣言、Systemの登録入口と順序を固定する
- Debug／Development／Release、Architecture、ABI Versionの不一致をLoad前または登録前に拒否する
- Project Display NameをTarget名、File名、Export Symbol、C++ Identifierの正本にしない
- Build失敗、Cancel、Validation失敗で以前の成功Artifactを破壊しない
- M15でHot Reload、Asset Build、Runtime Packaging、任意Project Component Storageを導入しない

## Reference Comparison

| Reference | 参考にする点 | CueEngineで採用しない点 |
| --- | --- | --- |
| Unreal Engine | ModuleごとのSource、Public／Private境界、Build RuleからのTarget構成、Monolithic／Modularの選択 | `UObject`、Reflection、Build.cs、Module API Macro、Engine全体のModule Graphを移植しない |
| Unity | Native Plug-inを単純なC InterfaceとC linkageで接続する原則 | Managed Runtime、P/Invoke、Unity固有Rendering Plug-in Eventを導入しない |
| Godot | 単一Entry Symbol、C Interface、Version付きAPI、Configuration／Platform別Library指定 | GDExtension API、Variant、Object Binding、`.gdextension`形式を移植しない |
| SOL-AVES | Global相当とWorld相当のLifetimeを分け、依存と破棄順を明示する考え方 | Service Container、型Lookup、Job System、Updater Graph、ECS変更をM15へ導入しない |
| Legacy CueEngine | Version検査、C ABI、DLL側生成物をDLL側で破棄する原則 | 肥大化したGameplay ABI、Hot Reload、旧型、旧Loader、旧Build Scriptを使用しない |
| TheatriaEngine | Project Codeを独立TargetとしてBuildする短い導線 | Machine固有Path、通常BuildとHot Reloadの混在、既存Source生成実装を使用しない |

References:

- [Unreal Engine Modules](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-modules)
- [Unreal Build Tool](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-build-tool-in-unreal-engine)
- [Unreal Engine Module API Specifiers](https://dev.epicgames.com/documentation/en-us/unreal-engine/module-api-specifiers-in-unreal-engine)
- [Unity Native plug-ins](https://docs.unity3d.com/2023.2/Documentation/Manual/NativePlugins.html)
- [Godot What is GDExtension?](https://docs.godotengine.org/en/stable/engine_details/engine_api/gdextension/what_is_gdextension.html)
- [Godot `.gdextension` file](https://docs.godotengine.org/en/stable/engine_details/engine_api/gdextension/gdextension_file.html)
- [Godot GDExtension interface JSON](https://docs.godotengine.org/en/stable/engine_details/engine_api/gdextension/gdextension_interface_json_file.html)
- [Microsoft C Run-Time Library selection](https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library)
- [Microsoft Linker Tools Error LNK2038](https://learn.microsoft.com/en-us/cpp/error-messages/tool-errors/linker-tools-error-lnk2038)
- [CEDiL: 「SOL-AVES」の高性能なランタイムを構成するアーキテクチャ](https://cedil.cesa.or.jp/cedil_sessions/view/3297)
- [TheatriaEngine](https://github.com/tochouseito/TheatriaEngine)

参考EngineはModule化とIterationを改善する一方、DLL数、Export管理、互換性検証、Tooling Complexityを増やす。
CueEngineはM16のStandalone Packageが必要とする一つのProject Moduleだけを導入し、汎用Plugin Systemへ拡張しない。

## Decision

### Delivery Model

Project固有CodeはWindows x64のShared Library Target `CueGameModule`としてBuildする。
Artifact名は固定の`CueGameModule.dll`とし、Project Display NameをFile名またはC++ Symbolへ変換しない。
PDB等の付随ArtifactはConfigurationとToolchainが生成する別FileとしてInventoryへ記録する。

EngineはProjectごとに`Cue.RuntimeHost`を再Compileしない。M15はGame ModuleをBuild Artifactとして生成、検証、記録するまでとし、
M16のPackage Publisherが汎用`CueRuntimeHost.exe`、Game Module、Runtime Data、Manifestを一つのPackageへ配置する。
M16のRuntimeHostはManifestに列挙されたGame ModuleだけをProcess開始時に読込み、停止完了後にUnloadする。

M15とM16は一Project、一Game Module、一RuntimeHost Processを基本形とする。複数Game DLL、Editor Plugin、Runtime Plugin、
動的追加、Hot Reload、同一Process内のProject切替は導入しない。

Static Linkだけを正式経路にしない。Static LinkではProjectごとにRuntimeHost Executableを再Link、複製する必要があり、
M16の共通Host、Package Manifest、Game Module Artifactという境界を維持できないためである。

### Target and Dependency Direction

依存方向は次のとおりとする。

```text
Project Source
  `-- CueGameModule.dll
        `-- Cue.GameModule.Abi headers

CueRuntimeHost.exe
  |-- Cue.GameModule.Loader.Windows
  |-- Cue.GameModule.Adapter
  `-- Cue.Runtime -> Cue.GameCore / Cue.Scene / Cue.Schema / Cue.Input / Cue.Foundation

Cue.Editor.Tool
  `-- Cue.Build -> Cue.Platform.Process.Windows / Cue.Project / Cue.Foundation
```

`Cue.GameModule.Abi`はC互換Headerだけを公開し、EngineのC++ LibraryへLinkしない。
Game Moduleは`Cue.Runtime`、`Cue.GameCore`、`Cue.Schema`のC++ HeaderまたはBinaryへ直接依存しない。
Host側のFirst-party AdapterだけがC ABI値を既存の`TypeDescriptor`、`RuntimeSystemDescriptor`、
`RuntimeSystem`へ変換する。Runtime ModuleはLoader、DLL Handle、Editor Build Serviceを参照しない。

Windows DLL読込みはPlatform Adapterへ置く。`HMODULE`、`LoadLibraryW`、`GetProcAddress`、Native Error Codeを
Portable ABI Header、Runtime、Editor Coreへ公開しない。

### Project Source and Build Layout

Project Root内の役割を次のように定める。

| Path | Role | Source of Truth | Overwrite Policy |
| --- | --- | --- | --- |
| `Source/Game` | User所有のFirst-party Game Source | Yes | 既存FileをGeneratorが上書きしない |
| `Source/Game/CMakeLists.txt` | Game Module Target定義 | Yes | 初回生成後はUser所有とする |
| `CMakeLists.txt` | Project BuildのRoot定義 | Yes | 初回生成後はUser所有とする |
| `CMakePresets.json` | 3構成の共有Preset定義 | Yes | Schemaを検証し、既存Fileを上書きしない |
| `Generated/Build/<workspace-key>` | CMake Binary Tree、生成IDE Project、Compiler中間物 | No | 互換なToolchain入力では再利用する |
| `Generated/Build/Candidates/<operation-id>` | 成功Processから収集した未公開Artifact | No | 検証後にPublishまたは破棄できる |
| `Saved/Build/Operations/<operation-id>` | Build Log、Plan、Environment、Result Snapshot | No | 診断Retention Policyで管理する |
| `Generated/Artifacts/<configuration>` | M16 Publisher入力となる検証済み成功Artifact Snapshot | No | 新しい成功ArtifactのPublish時だけ置換する |

`Assets/Source`はAsset Authoring用であり、C++ Sourceを置かない。`Assets/Runtime`はAsset Pipeline出力用であり、
Game DLL、PDB、Build Logを置かない。Machine固有Engine Source／Binary LocationはCMake引数またはUser Workspace設定から渡し、
共有`CMakeLists.txt`、`CMakePresets.json`、`CueProject.json`へ絶対Pathを書込まない。

`workspace-key`はGenerator、Architecture、Toolset、Engine Build Policyの互換入力から決定的に作る。
同じKeyのCMake Binary TreeはIncremental Buildへ再利用し、入力が変わった場合は別KeyへConfigureする。
Operation IDをBinary TreeのIdentityにしないため、通常の再Buildで全Objectを毎回作り直さない。

Generatorは空Project生成時に全共有SourceとBuild定義をStaging Rootへ構築し、検証後だけProject Rootとして公開する。
既存Projectへの不足File追加はCreate-onlyとし、File単位の存在と内容Hashを検査してから実行する。
既存User Source、CMake定義、Presetを自動Migrationまたは再生成名目で上書きしない。

### ABI Surface

Game Module ABI v1は一つの固定Export Symbol `cue_game_module_query`から開始する。
ExportはHostが要求するABI VersionとHostが確保した出力構造体を受け取り、
互換な`CueGameModuleApiV1` Function Tableまたは安定Error Codeを返す。

ABI HeaderはC11とC++20の両方からInclude可能にし、次の規則を満たす。

- `extern "C"`はC++ Compile時だけ適用する
- 整数は`uint8_t`、`uint32_t`、`uint64_t`、`int32_t`、`int64_t`等の固定幅型を使用する
- `bool`、`wchar_t`、`size_t`、Compiler依存Enum幅、Reference、Template、RTTI、Virtual Classを使用しない
- 文字列はUTF-8の`const char*`と`uint64_t`長を持つ借用Viewとし、NUL終端を要求しない
- UUID型Identityは16 byte値として渡し、Compiler型名、Pointer値、登録順をIdentityにしない
- 可変長入力はPointerと`uint64_t`件数を持つ借用Arrayとし、呼出終了までだけ有効とする
- 全公開構造体は`structSize`と対象Versionを持ち、Hostは不足Sizeを拒否し、未知Tailを読まない
- Function Tableの予約Fieldは0またはNullを要求し、追加はVersionまたはSizeでNegotiationする
- `std::string`、`std::vector`、`std::span`、`std::unique_ptr`、`Result`、`Error`、`std::function`を公開しない
- C++ Exception、SEH、`longjmp`を境界越しのError通知に使用しない
- Calling ConventionとSymbol visibilityはMacroで一箇所に固定する

Module API TableはDLLが所有し、HostはDLL Load中だけ借用する。HostはTableの値を検証済みFactoryへCopyできるが、
Function PointerをDLL Unload後に呼ばない。v1はGame ModuleからHost機能を任意に呼ぶHost API Tableを公開しない。
後続のGameplay APIは必要機能だけを列挙したVersion付きTableとして追加し、既存v1構造体の予約領域へ暗黙追加しない。

生成、登録、Start、Update、Stop等の失敗可能なABI関数は安定した数値Resultを返す。
System StateとModule Handleの破棄Callbackだけは戻り値を持たない失敗不能操作とし、同じ入力へ一度だけ呼ぶ。
破棄Callbackは所有物を完全に解放して正常復帰するか、契約違反を検出したModule自身がProcessをFail-fast終了する。
Exception、部分解放状態、再試行要求をHostへ返してはならず、Hostは破棄失敗を回復可能Errorへ変換してDLLをUnloadしない。
詳細診断はUTF-8の借用Viewとして同じ呼出中だけ返し、Hostが即時Copyする。
Game Module内のAllocationはGame Moduleが解放し、Host内のAllocationはHostが解放する。片側で生成したObject、Buffer、
String、Array、File Handleを他方の`free`、`delete`、Destructorで破棄しない。

### Registration Entries

`CueGameModuleApiV1`は次の責務を持つCallbackを固定順で提供する。

1. Project ScopeのModule Handleを生成する
2. Schema TypeとTombstoneを登録する
3. Component宣言を登録する
4. Runtime System Factory定義を登録する
5. Project ScopeのModule Handleを破棄する

Schema登録は`TypeId`、診断名、連続Schema Version、Field ID、Field診断名、Reserved Field IDをC互換Descriptorで渡す。
HostはCallback中に全値をFirst-party所有値へCopyし、既存`SchemaRegistryBuilder`へ追加する。
登録後に借用Pointerを保持しない。Core Schemaを先に登録し、Game SchemaとTombstoneをModule登録順で受理した後、
全衝突を検査してSchema Registryを一度だけSealする。

Component登録はStable Type IDとComponent用途の宣言をSchema登録へ関連付ける。
M15 ABI v1はComponentのC++ Object Layout、Constructor、Move、Destructor、生Pointer、Storage Pointer、Query Viewを公開しない。
現行`World::register_component<T>`はCompile時C++型Bindingであるため、任意Project Componentを動的Storageへ登録する機能は
ECS Storage／ABIの別Researchを先行させる。M15では宣言の重複、対応Schema欠損、Stable ID衝突を診断できるところまでとし、
Component StorageやSerializationを追加しない。

System登録はStable UTF-8 ID、`PreUpdate`／`Update`／`PostUpdate`、`int32_t` Order、Dependency ID Array、
System State生成／破棄、Start／Update／Stop Callbackを渡す。HostはDescriptorを即時Copyし、CallbackとModule Handleを保持する
First-party FactoryをProject Scopeへ構築する。FactoryはSessionごとにHost側`RuntimeSystem` Adapterを一つ生成する。
AdapterがDLL側System Stateを一意所有し、Adapterの破棄処理が失敗不能なDLL側破棄Callbackの一回実行を包含する。
System StateをAdapterと別のOwnerへ置かず、Adapterより先に独立破棄しない。

Game System ABI v1のStart／StopはSystem Stateだけを受け、UpdateはFrame Indexと符号付き64-bit NanosecondのTiming値だけを値で受ける。
`World`、`RuntimeWorld`、`StructuralCommandBuffer`、Entity Pointer、Component Pointer、Service Locatorを公開しない。
Start／Update／Stopを通した任意ECS操作とGameplay APIはM16 Gateの条件ではなく、後続ResearchでVersion付きHost APIとして追加する。
これによりM15でECS設計を変更せず、ABI v1を旧版の広大なGameplay APIへ拡大しない。

### Registration Order and Lifetime

Standalone ProcessとEditor Processは一つのProjectだけをGame Moduleへ接続する。順序は次のとおりとする。

1. ManifestまたはBuild Artifact MetadataからPath、Hash、Size、Architecture、Configuration、ABI VersionをLoad前に検証する
2. Windows Loader AdapterがDLLをLoadし、固定Entry Symbolだけを解決する
3. EntryへHost ABI Versionを渡し、Module自己報告値、Module API Table、Project Identityを登録前に検証する
4. Project ScopeのModule Handleを生成する
5. Engine Core Schemaを`SchemaRegistryBuilder`へ登録する
6. Game ModuleのSchema、Tombstone、Component宣言を登録する
7. 全SchemaとComponent宣言を検証し、Schema RegistryをSealする
8. Game ModuleのSystem Factory定義を登録順でProject ScopeへCopyする
9. Runtime Application SessionごとにFactoryからSystem StateとHost Adapterを生成する
10. 既存規則どおりRegistryをSealし、Phase、Order、登録順でStart／Update／逆順Stopする
11. 全SessionのStop完了後に各Adapterを破棄し、その処理内で所有System Stateを破棄した後、Factoryを破棄する
12. Project ScopeのModule HandleをDLL側Callbackで破棄し、最後にDLLをUnloadする

Schema Registry Seal失敗またはSystem Factory登録失敗ではSessionを開始しない。途中まで生成したHost所有値とModule Handleを
逆順で破棄し、DLLをUnloadする。System Start失敗以降はADR-0021のSession RollbackとCleanup契約へ従う。

Module DLLはModule Handle、Factory、Adapter、System State、Callbackのいずれかが生存する間Unloadしない。
Game Moduleを接続するComposition RootのThreadをProject Scope Owner Threadとする。DLL Load、Query、Module Handle生成、
Schema／Component／Factory登録、Module Handle破棄、DLL UnloadはこのThreadだけで行う。Game Module ABI v1を使用する
Runtime Application Sessionは同じProject Scope Owner Threadで生成、更新、停止、破棄し、System Lifecycle Callbackも同Threadだけで呼ぶ。
Game Moduleを使用しないSessionにはこの追加制約を適用せず、ADR-0021のOwner Thread契約に従う。

ModuleはCallback Context、借用String、借用Array、HostのOpaque ContextをCallback終了後に保持しない。
M15／M16ではBackground ThreadをGame Module ABIから開始しない。`DllMain`とC++静的初期化では、Thread生成、File IO、
Engine Callback、Module外Resource取得、永続Mutable状態の公開を行わない。外部Metadata不一致はDLL Load前に拒否するが、
Module自己報告値だけの不一致はDLL初期化後、Module Handle生成と登録の前に拒否し、即座にUnloadする。

### Toolchain, Runtime Library, and Configuration Compatibility

初期対応Matrixを次に固定する。

| Property | Supported Contract |
| --- | --- |
| Host OS | Windows |
| Architecture | x64 |
| Language | C++20 for Project Source、C11-compatible public ABI |
| Compiler family | Engineが記録した対応MSVC Toolset |
| Debug | Debug Host + Debug Game Module、MSVC Debug DLL Runtime |
| Development | Development Host + Development Game Module、MSVC DLL Runtime |
| Release | Release Host + Release Game Module、MSVC DLL Runtime |

Debug／Development／Releaseを相互に混在させない。Game Module Artifact Metadataは少なくともABI Version、Project ID、
Engine Compatibility、Configuration、Architecture、Compiler family、MSVC Toolset Identityを持つ。
LoaderはDLL Entryを呼ぶ前にManifest Metadataを検査し、Entry呼出後にもModule報告値との一致を検査する。

C ABIでAllocator所有権を分離しても、異なるConfigurationやToolsetの組合せを暗黙に互換とは扱わない。
Generated Project CMakeはEngineが公開するBuild Policy Targetを使用し、DebugではDebug DLL Runtime、
Development／ReleaseではDLL Runtimeを選択する。`_MSC_VER`、Runtime Library、Iterator Debug Level等の不一致を
Link時またはArtifact Validationで診断し、LNK2038を場当たり的なCompiler Option無効化で回避しない。

Toolchain Versionの許容Rangeは#220のEnvironment ValidationでEngine Build Metadataから決定する。
本ADRは特定Visual Studio Install Path、Windows SDK Patch Version、CMake Install Pathを共有Projectへ固定しない。

### Editor Build Service Boundary

Editor UIは`Cue.Build`のApplication Serviceへ型付きBuild Requestを渡すだけとする。
ServiceはToolchain検証、Plan生成、Process実行、Stage遷移、Log、Cancel、Artifact Publishを所有する。

```text
ImGui Build UI
  -> Build Request
  -> Game Build Service
       -> Toolchain Validation
       -> Immutable Build Plan
       -> CMake Runner
       -> Child Process Service
       -> Artifact Validation / Publish
  <- State Snapshot / Log Event / Result / Artifact Inventory
```

UIはCMake、MSBuild、`CreateProcessW`、Job Object、Filesystem Publishを直接呼ばない。
Build ServiceはEditorDocument、Selection、ImGui Context、RuntimeWorldを所有せず、Game ModuleをLoadまたは実行しない。
M15ではBuild完了後のHot Reload、Editor Play自動再起動、Runtime Session差替えを行わない。

Build RequestはProject Root、Configuration、Target、Operation IDを検証し、不変Build Planへ変換する。
Process RunnerへShell Command文字列ではなくExecutable PathとArgument Vectorを渡す。
Machine固有EnvironmentはAllowlistで構成し、Credentialや無関係なEnvironmentをPlan、Log、診断Bundleへ保存しない。

### Artifact Publication and Failure Contract

各Build Operationは互換KeyのBinary Treeを再利用し、独立したLog、Result、Candidate Artifact領域を持つ。
ConfigureまたはBuild開始時に以前の成功Artifactを削除、切詰め、上書きしない。

CMake Process成功後、要求Targetの出力をOperation固有Candidate領域へCopyし、そのSnapshotだけを検証する。
Binary Tree内の中間出力は成功Artifactの正本にせず、失敗Buildで更新されても公開済みSnapshotへ影響させない。

CandidateはProcess成功だけで公開せず、次をすべて検証する。

- 要求Configuration、Architecture、Targetと一致する
- 必須DLLが存在し、通常FileでRoot境界内にある
- Sizeが0ではなく、Hashを取得できる
- ABI、Project、Engine Compatibility、Toolset Metadataが一致する
- Artifact InventoryのFile名、相対Path、Size、Hashが確定している

検証成功後、StorageのAtomic Replace契約で`Generated/Artifacts/<configuration>`の
`Latest Successful Artifact` Snapshotを更新する。このSnapshotは再生成可能なM16 Publisher入力であり、
RuntimeがProjectのGenerated Rootから直接Loadする契約ではない。
失敗、Cancel、Timeout、Editor終了、Metadata不一致ではCandidateを成功として公開せず、以前の成功SnapshotとInventoryを保持する。
失敗OperationのLog、Plan、Environment Report、Stage Resultは`Saved/Build/Operations/<operation-id>`へ診断可能な範囲で残す。

Artifact Retention、古いBinary Tree削除、Diagnostic Bundle上限は後続Issueで決めるが、User SourceとLatest Successful Artifactを
Cleanup対象へ含めない。CleanupはBuild成功条件にせず、失敗しても成功Artifactの正本を失わせない。

### Error and Diagnostic Contract

Game Module ABIは安定数値ResultとUTF-8診断だけを返す。HostはModule Errorを`Cue.Foundation`のErrorへ変換し、
ABI Version、Project ID、Configuration、Stage、System ID等の値をContextへCopyする。DLL内部Pointer、Native Handle、
Exception Object、Call Stack ObjectをErrorへ保存しない。

ABI CallbackからExceptionまたはSEHが境界外へ出た場合はContract違反として扱う。通常の回復可能Errorへ変換して継続せず、
既存Fatal PolicyでProcessを停止する。Build失敗はRuntime Fatalではなく、Build Operationの失敗として既存成功Artifactを保持する。

診断にはSource本体、Credential、Environment全体を既定で含めない。絶対User PathはUIで必要な範囲だけ保持し、
Export時は#226のRedaction契約へ従う。

## Consequences

### Positive

- Engine SourceをProjectごとに再Buildせず、一つのGame Module Artifactとして扱える
- Runtime、Editor、Project Codeの依存方向が一方向になる
- DLL境界のAllocator、STL、Exception、Compiler固有C++ ABI事故を限定できる
- Build失敗と以前の成功Artifactを明確に分離できる
- M16がManifestからRuntimeHost、Game Module、Runtime Dataを決定的に起動できる
- Hot Reloadと大規模Gameplay ABIをM15から分離できる

### Trade-offs

- C ABI Descriptor、Host Adapter、Metadata、Compatibility検証が必要になる
- Static Linkだけの構成よりTargetとArtifactが一つ増える
- C++型を直接渡せないため、Game APIはVersion付きHost Functionとして個別設計する必要がある
- M15 ABI v1だけでは任意Project ComponentをRuntime ECSへ格納、Query、編集できない
- ABIを変更するたびにVersionと互換性Testが必要になる

### Mitigations

- ABI v1を登録とLifecycleの最小面積へ限定し、予約APIを先回りして追加しない
- Public ABI HeaderをCとC++の双方で単体Compileする
- Module内生成／Module内破棄、Host内生成／Host内破棄をTestで固定する
- Runtime System AdapterとLoaderをFirst-party Targetへ隔離する
- Gameplay APIと任意Component Storageは実要件を伴うResearch Issueから追加する
- M15 GateでAsset Pipeline、Hot Reload、ECS変更が差分へ含まれないことを確認する

## Rejected Alternatives

### ProjectごとのRuntimeHostへGame SourceをStatic Linkする

DLL ABIは不要になるが、ProjectごとにEngine Hostを再Linkし、共通RuntimeHost ArtifactとM16 Package Manifestの境界が崩れる。
Build時間と配布物の重複も増えるため、M15の正式経路には採用しない。

### C++ Virtual InterfaceとSTLをDLLへ公開する

Compiler、Runtime Library、Iterator Debug Level、Exception、Allocator、Object Layoutの一致を暗黙前提にする。
所有権と破棄責務を局所的に証明できないため採用しない。

### Game ModuleをEditor Processへ直接LoadしてHot Reloadする

Build、Editor、Session、DLL、Thread、PDB、失敗RollbackのLifetimeを同時に固定する必要がある。
M15のBuild機能完成を遅らせるため採用しない。

### ABI v1へECS、Renderer、Sound、Effect、Physicsの全操作を入れる

未確定SubsystemとData LayoutをABIへ固定し、旧CueEngineと同様にVersion更新範囲を肥大化させる。
ユーザー方針にも反するため採用しない。

### Engine C++ LibraryをGame DLLへ直接Linkする

既存C++型、Template、所有権がDLL境界へ漏れ、Engine内部RefactorをABI変更にする。
Game ModuleはC互換ABI Headerだけへ依存させるため採用しない。

### 任意Service ContainerをGame Moduleへ公開する

依存、Lifetime、Thread、利用可能APIを実行時Lookupまで隠し、ADR-0021の明示Compositionに反するため採用しない。

### 失敗Build開始時に前回Outputを消去する

CancelまたはCompiler Errorで最後に動作したArtifactを失う。Operation単位のCandidateとAtomic Publishを使用するため採用しない。

## Validation Contract

M15で次を検証する。

- Blank Project Generatorが`Source/Game`とCMake正本をProject Root内へ生成する
- 既存User Source、CMake定義、Presetを上書きしない
- 生成ProjectをDebug／Development／ReleaseでConfigure、Buildできる
- C11とC++20のTranslation UnitがABI Headerを単体Includeできる
- ABI Public HeaderがSTL、Exception、C++ Class、Windows型を公開しない
- 誤った外部MetadataのProject ID、Architecture、Configuration、ToolsetをDLL Load前に拒否する
- Module自己報告のABI Version、Project ID、Configuration不一致をModule Handle生成と登録の前に拒否する
- Schema、Component宣言、System Factoryを固定順で登録し、失敗時に逆順Cleanupする
- 二つのSessionが別のDLL側System Stateを持ち、片方の停止が他方へ影響しない
- System StateとModule Handleを生成したDLL側Callbackで一度だけ破棄する
- Adapter破棄がSystem State破棄を包含し、失敗不能破棄Callback以外の経路で解放しない
- Callback、Factory、System State生存中にDLLをUnloadしない
- Project ScopeとGame Module使用Sessionの全Callbackを一つのOwner Threadへ限定する
- UIなしでBuild Request、Plan、Cancel、Retry、Artifact Publishを検証できる
- CancelとTimeoutを区別し、Process TreeとHandleを残さない
- 失敗Build後も以前の成功Artifact HashとInventoryが変わらない
- Runtime、Game Module、Build CoreからEditor／ImGuiへの逆依存がない
- M15差分にAsset Pipeline、Hot Reload、ECS Storage改良が含まれない

M16でManifestに列挙されたGame ModuleのHash、Size、Compatibilityを検証し、Package移動後もCurrent Directoryへ依存せず
Load、Startup、Stop、UnloadできることをProcess Testで追加確認する。

## Implementation Sequence

1. #219でGame Source、CMake Workspace Generator、ABI v1 Header、最小Game Entryを実装する
2. #220でCMake、Visual Studio、MSVC、Windows SDK、Engine Build Metadataを検出、検証する
3. #221でArgument Vector、Log Capture、Cancel、Timeout、Process Tree終了を持つWindows Child Process境界を実装する
4. #222でBuild Request、Immutable Plan、Debug／Development／Release Profileを実装する
5. #223でCMake Configure／Build RunnerとStage Resultを実装する
6. #224でBuild Operation、Log、Artifact Inventory、Latest Successful Artifactを所有するServiceを実装する
7. #225でEditor Build UI、Console、Cancel、Retry、Artifact表示を実装する
8. #226でPath Redactionと上限を持つBuild Diagnostic Bundleを実装する
9. #227で3構成Build、Process／Cancellation Test、手動Editor Workflow、Artifact保全を完了判定する

## Follow-up

- #219から#227で本契約のBuild側を実装、統合、検証する
- #233でPackage ManifestからGame ModuleをLoadし、登録、Session実行、Stop、Unloadを接続する
- 任意Project ComponentのStorage、Query、SerializationはECS改良を再開する際のResearch Issueで決定する
- GameplayからWorldを操作するHost APIは具体的な最小Game要件を伴う別Research IssueでVersion化する
- Scripting、Hot Reload、Game Rendering、Sound、Effect、Physics、Asset Pipelineは本ADRで先行実装しない

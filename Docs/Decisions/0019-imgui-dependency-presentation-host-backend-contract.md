# ADR-0019: Dear ImGui Dependency, Presentation Host, and Backend Contract

- Status: Accepted
- Date: 2026-09-04
- Last amended: 2026-09-17 (#327)
- Decision Owners: CueEngine Project
- Approval: User authorized Dear ImGui through vcpkg on 2026-09-04

## Context

M12 の #162、#163、#165 は、Project Hub と最小 Editor を実動 ImGui UI から操作し、Keyboard、Cancel、
Error 表示、手動 Workflow を検証することを要求する。ADR-0018 は ImGui を Presentation Adapter に限定したが、
Dear ImGui の取得方法、License、Version 固定、Platform／Renderer Backend、Tool Host の所有権は決定していない。

現在の Repository には Dear ImGui Source、Package Manager Manifest、Submodule、`FetchContent` 定義がない。
Userは第三者CodeをEngine所有Sourceへ混在させず、Licenseを順守して`ThirdParty`配下へ分離し、外部Libraryを
vcpkgで導入する方針を指定した。新規外部Libraryは導入前に毎回Userの明示承認を必要とし、Dear ImGuiはM12での導入が
明示承認された。

本 ADR は承認済みDear ImGuiの取得、License、Version固定、Presentation Host、Backend、所有権境界を決定する。

## Verified Facts

- Dear ImGui は MIT License で公開され、Copyright Notice と Permission Notice の同梱を要求する
- Dear ImGui は Core、Platform Backend、Renderer Backend を分離する
- Windows では公式 Win32 Platform Backend と DirectX 12 Renderer Backend が提供される
- Platform Backend は Input、Cursor、Timing、Windowing を担当する
- Renderer Backend はFont TextureとDraw DataのRenderer接続を担当する
- 公式 Documentation は Custom Backend より公式 Backend の利用を初期選択として推奨する
- Dear ImGui 自体は CueEngine 用の正式 CMake Target を提供しないため、CueEngine 側に限定された Adapter Target が必要となる
- vcpkgはManifest Modeを多くのUserに推奨し、Manifestごとに分離されたInstall Treeを使用する
- vcpkgの`builtin-baseline`はRegistry Commitを固定し、Dependency Versionの再現性を提供する
- #327で固定したvcpkg builtin portはDear ImGui `1.92.9`と`docking-experimental`／`win32-binding`／
  `dx12-binding` Featureを提供し、`docking-experimental`はupstreamの`v1.92.9b-docking`を取得する

確認元:

- <https://github.com/ocornut/imgui/blob/master/LICENSE.txt>
- <https://github.com/ocornut/imgui/blob/master/docs/BACKENDS.md>
- <https://github.com/ocornut/imgui/wiki/Getting-Started>
- <https://github.com/ocornut/imgui/releases>
- <https://learn.microsoft.com/vcpkg/concepts/manifest-mode>
- <https://learn.microsoft.com/vcpkg/users/examples/versioning.getting-started>
- <https://github.com/microsoft/vcpkg/tree/master/ports/imgui>

## Decision Drivers

- User承認のない外部Libraryを導入しない
- Clean Checkout から同じ Source Revision を取得できる
- Configure 時の暗黙 Network Access を避ける
- External Source を First-party Source と混在させない
- UI Adapter を Runtime、Cue.RHI、D3D12 Native API から隔離する
- Tool Host 固有の Window、ImGui Context、Backend、GPU Resource の寿命を一意にする
- M12 の手動 UI Workflow と Headless Test を両立する
- Runtime Renderer、Viewport、Multi-Viewportを先取りせず、User承認済みのDocking API利用可能化だけをM21で行う

## Options

### Option A: 現行の第三者 Code 禁止を維持し、First-party UI Toolkit を実装する

第三者依存は増えないが、#162 と #163 の ImGui 要件を満たさず、Input、Text Editing、Layout、Clipping、Font、
Accessibility、Renderer Backend を M12 内で新規設計する必要がある。Project Hub と基本 Editor 操作を早期に完成させる目的に
対して Scope が大きすぎるため推奨しない。

### Option B: 公式RepositoryをPinしたGit Submoduleとして取得する

External Source の正本、Commit Identity、License を分離でき、CueEngine Source へ Code をコピー、改名、部分抽出せずに利用できる。
Configure は既に取得済みの Pin 済み Sourceだけを使用し、Network AccessやBranch追従を行わない。初回取得には明示的な
Submodule 初期化が必要となる。

Versionは固定できるが、Userが指定したvcpkg限定方針に反するため採用しない。

### Option C: CMake FetchContent でDear ImGuiを取得する

Configure が Network と外部 Host 状態へ依存し、Source取得とBuild定義が暗黙に混在する。Offline Build、失敗診断、
Supply Chain Review が弱くなるため採用しない。

### Option D: vcpkg Manifest で Dear ImGui を取得する

ManifestとRegistry BaselineでDependency Graphを宣言し、Project専用Install Treeへ分離できる。Package Manager、Registry、
Port DefinitionもBuild Inputになるため、それらをVersion PinとReview対象へ含める。Userがvcpkgでの外部Library導入を指定したため
採用する。

### Option E: Dear ImGui SourceをRepositoryへCopyまたはVendorする

Engine Sourceとの分離は可能だが、vcpkgを唯一の導入経路とするPolicyに反し、更新時の差分とProvenanceも曖昧になるため採用しない。

### Option F: Machineへ事前InstallされたBinaryを検索する

ABI、Compiler、Configuration、Version、License、Clean Checkout 再現性を保証できないため採用しない。

## Decision

Option Dを採用し、Dear ImGuiをvcpkg Manifest Modeで導入する。

- Dependency Control PlaneはRepository Rootの`ThirdParty`配下に置く
- `ThirdParty/vcpkg.json`へDear ImGui Core、`docking-experimental`、`win32-binding`、`dx12-binding`だけを宣言する
- `ThirdParty/vcpkg-configuration.json`で公式vcpkg Registryと40文字のBaseline Commitを固定する
- `ThirdParty/vcpkg-tool.json`で公式vcpkg Repository、Tool Commit
  `386d7c478221b7ee0c97bfe6ea61dcf65121d564`、Tool Release `2026-07-27`、Windows x64 Tool Version
  `2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8`、実行Binary SHA-256
  `13b8175e99a884c5ad34249218754b45541a1a63f216e92603aee57a285ac741`、Tool Source SHA-512
  `e2e256879343662da5b18994559559faa04691987bc1025fc067d9ca944d3ba495bad759e4906bb06952572df1a2671e0fc63281c494fe7299078bcd18c16cde`
  を固定する
- #327以降は確認済みbuiltin portのDear ImGui `1.92.9`とupstream `v1.92.9b-docking`を使用し、
  Registry BaselineとManifest FeatureでVersionを固定する
- `ThirdParty/vcpkg_installed`をProject専用Install Rootとし、生成物としてGit管理対象外にする
- `ThirdParty/.tools/vcpkg`は明示Dependency Restoreだけが作成できるPin済みTool Checkoutとし、Git管理対象外にする
- `ThirdParty/THIRD_PARTY_NOTICES.md`と`ThirdParty/Licenses/DearImGui-LICENSE.txt`をGit管理し、配布物にも含める
- `examples/`、Demo Application、第三者Extension、Multi-Viewportは対象にしない
- 第三者Sourceは変更、Copy、Patch、Rename、部分抽出しない
- `Engine`配下には第三者Source、Header、Binary、License Copyを配置しない
- Dependency Restoreは専用Script／CI Stepとして明示実行し、通常のCMake Configure中の暗黙Network取得は無効にする
- Dependency Restoreは`ThirdParty/.tools/vcpkg`の管理Checkoutだけを実行元とし、外部`VCPKG_ROOT`を使用しない
- Restore前に管理Checkoutの追跡対象WorktreeがCleanであること、HEAD Commit、
  `scripts/vcpkg-tool-metadata.txt`のRelease／Source Hash、`vcpkg.exe version`、実行Binary SHA-256を
  `vcpkg-tool.json`と照合する
- 初回または実行Binary不一致時はPin済みClean Checkoutの`bootstrap-vcpkg.bat -disableMetrics`から再生成し、
  再照合に失敗した場合はInstallを開始せず失敗する
- Machine固有の絶対PathをRepositoryへ記録しない
- Updateは専用Research／Maintenance IssueでUser承認を得て、Baseline、Version、License、API差分、3構成Buildを再検証する
- ImGui Version番号を`Cue.ImGui.Core`のCompile Version Tokenにも固定し、更新時は全利用Targetを再Compileさせる
- Dear ImGui以外の外部LibraryをManifestへ追加する場合は、その変更前にUserの明示承認を得る

## Target and Dependency Boundary

```text
Cue.ProjectHub.ImGui ------> Cue.ProjectHub
          |----------------> Cue.ImGui.Core

Cue.Editor.ImGui ----------> Cue.EditorCore
          |----------------> Cue.ImGui.Core

Cue.ToolHost.WindowsD3D12 -> Cue.Platform.Windows
          |----------------> Cue.ImGui.Backend.Win32D3D12
          |----------------> D3D12 / DXGI private composition

Cue.ProjectHub.Tool ------> Cue.ToolHost.WindowsD3D12
          |----------------> Cue.ProjectHub.ImGui
          |----------------> Cue.ProjectHub.Windows
          `----------------> Cue.IO.Windows

Cue.ProjectHub.Windows ---> Cue.ProjectHub
          |----------------> Cue.IO.Windows
          `----------------> Cue.Foundation.Windows

Cue.Editor.Tool ----------> Cue.ToolHost.WindowsD3D12
          |----------------> Cue.Editor.ImGui
          |----------------> Cue.EditorCore
          `----------------> Cue.IO.Windows

Cue.ImGui.Backend.Win32D3D12 -> Cue.ImGui.Core

Cue.ImGui.Core ------------> vcpkg imgui::imgui
```

`Cue.ProjectHub`、`Cue.EditorCore`、`Cue.Scene`、`Cue.Project`、Runtime Module は Dear ImGuiへ依存しない。
`Cue.ProjectHub.ImGui` と `Cue.Editor.ImGui` は `Cue.RHI`、D3D12 Header、Native Device、Descriptor Heapを参照しない。

`Cue.ToolHost.WindowsD3D12` は M12 Tool UI 共通のHost Runtime Targetとする。Window、Message Pump、ImGui Context、
Frame開始／終了、Tool用D3D12 Device、Queue、Swap Chain、Descriptor Heap、公式Backendの初期化／終了順を所有する。
Tool Host の Native ObjectをPresentation AdapterまたはApplication Serviceの公開APIへ出さない。
Tool用Swap Chain生成はADR-0006をAmendする専用Adapterで、短命な`NativeWindowView::value()`を
`CreateSwapChainForHwnd`相当の呼出中だけ使用する。Move-onlyな`WindowsToolSwapChainBinding`はRaw Native値を保持または公開せず、
Windowより先にSwap Chainと共に破棄する。Runtime RHIまたはGame Swap Chainへこの例外を拡張しない。

`Cue.ProjectHub.Tool`と`Cue.Editor.Tool`を最終Executable Composition Rootとする。各Rootは
`Cue.ToolHost.WindowsD3D12`、対応するApplication Service、Presentation Adapter、Presentation Stateを一意所有し、
HostのFrame Callback内でAdapterを駆動する。共通Host TargetはProject HubまたはEditor固有型へ依存しない。

`Cue.Editor.Tool`は`Cue.IO.Windows`を介して、起動要求から再検証したProject Descriptorが示すSource Assets RootとSaved Rootを
Windows Filesystem Rootとして生成する。両Rootと`ScenePersistenceServices`は`EditorController`および
`ProjectWorkspaceSession`より長く一意所有し、Presentation AdapterへFilesystemまたはNative Handleを公開しない。
Project Rootは既存DirectoryとしてBindingし、Source Assets Rootは既存の通常Directoryだけを許可する。Source Assetsが欠損、
File、Reparse Pointの場合はSessionを公開せず、それぞれ`NotFound`、`TypeMismatch`、`UnsupportedEntry`で起動を失敗させる。
Saved Rootが欠損している場合だけ、Binding済みProject Rootの`create_directories()`でDescriptorのProject相対Rootを作成してから
Saved RootをBindingする。File／Reparse Point衝突は`TypeMismatch`／`UnsupportedEntry`とし、既存Entryを置換しない。
途中で失敗した場合は生成済みFilesystem Objectと部分Sessionだけを破棄する。作成済みSaved Directoryは、競合Processの利用や
作成後の内容を安全に判別できないため削除せず、空Directoryを残すことを許容する。再試行は同じDirectoryを冪等に再利用する。

`Cue.ProjectHub.Windows`は`ProjectHubPlatform`の本番Windows Adapter Factoryを提供し、Locator正規化、Project Locator合成、
Windows Filesystem Root生成、UUID発行を実装する。`Cue.ProjectHub.Tool`はこのAdapterと`Cue.IO.Windows`からWorkspace
`FilesystemRoot`を生成して、どちらも`ProjectHubService`より長く一意所有する。Presentation Adapterはこれらへ直接依存しない。

Workspace RootはADR-0014をAmendする`create_windows_known_folder_filesystem_root()`へ
`LocalApplicationData`、`CueEngine/Workspace`、`CreateOrOpen`を渡して生成する。初回起動時だけ不足Directoryを安全に作成し、
File／Reparse Point衝突、Known Folder解決失敗、作成失敗はNative Context付きRecoverable ErrorとしてUIへ伝える。

M12 では Runtime Renderer、Game Swap Chain、Viewport Render Target、Cue.RHI 公開APIを Tool UI のために変更しない。
Tool用D3D12 ResourceとGame Renderer Resourceの共有は対象外とする。

`Cue.ImGui.Core`はCMake上で`imgui::imgui`を`PUBLIC`または`INTERFACE`依存として公開し、Presentation Adapterへ
Core HeaderとLink Symbolを供給する。`Cue.ImGui.Backend.Win32D3D12`は同じTargetの公式Win32／DX12 Backend APIを
First-party Host Adapterから呼ぶ。Presentation AdapterはBackend Targetへ依存しない。

公式Win32 Backendが初期化時のNative Window値を内部保持するため、ADR-0006をAmendし、
`Cue.ImGui.Backend.Win32D3D12`の`WindowsBackendWindowBinding`だけにこの非所有保持を許可する。BindingはWindowより短命で、
Backend Shutdownを完了してから破棄される。Tool HostはBinding破棄後にだけWindowの`destroy()`を開始し、First-party Codeは
Native値を保存またはPresentation／Application／RHIへ公開しない。

## Win32 Message Delivery Contract

公式Win32 BackendへInputを渡すため、#162で`Cue.Platform.Windows`にWindows固有のOpt-in Message Sink境界を追加する。
Platform非依存の`Cue.Platform` API、`WindowEvent`、Runtime ModuleへWin32型を追加しない。

- Sinkのattach／detachはWindow Owner Threadだけで行い、一つのWindowに一つだけ非所有Sinkを関連付ける
- SinkはNative Window、Message ID、`wParam`、`lParam`を`const void*`、固定幅Integer、`std::uintptr_t`、
  `std::intptr_t`のWindows固有Value Viewとして受け、Headerから`windows.h`を公開しない
- SinkはHandled FlagとNative Result値を返し、Host Adapterが`ImGui_ImplWin32_WndProcHandler`の結果へ変換する
- `WM_CLOSE`、`WM_DESTROY`、`WM_SIZE`などPlatformが所有するLifecycle Messageは既存状態更新を優先し、
  Sinkがその処理を抑止できない
- Keyboard、Mouse、Text、FocusなどPlatformが所有しないMessageはSinkを呼び、Handledなら`DefWindowProcW`へ渡さない
- Sink CallbackはWindow Procedure内で同期実行し、例外を送出せず、Message引数とNative Windowを保存しない
- SinkはImGui ContextとWin32 Backendより後にattachし、Backend ShutdownとContext破棄より前にdetachする
- Window破棄または初期化RollbackはSink関連付けを解除し、Sinkより後にWindowを破棄する
- `NativeWindowView`のSubclass禁止は維持し、Tool Hostによる`SetWindowLongPtrW`置換を許可しない

attach／detachは診断可能な`Result<void>`を返し、回復可能な失敗は`Cue.Platform.Windows` Error Categoryで表す。
Owner Thread外からの呼出し、Sink Callback実行中の再入attach／detach、許可されないLifecycle Stateからの呼出しは、
ADR-0005とADR-0006に従うProgramming Contract違反とする。Debug／DevelopmentではAssertして終了し、Releaseでは
呼出し自体を契約外とする。attachは`Created`／`Visible`、detachは`Created`／`Visible`／`CloseRequested`だけで許可する。
Windows Windowではない対象は`InvalidWindowKind`を返す。

関連付けの状態遷移は次の契約とする。

| 操作 | 現在状態 | 結果 |
| --- | --- | --- |
| attach(A) | 未関連付け、許可State | Aを関連付けて成功する |
| attach(A) | Aを関連付け済み | 冪等に成功し、状態を変更しない |
| attach(B) | Aを関連付け済み | `MessageSinkAlreadyAttached`を返し、Aを維持する |
| detach(A) | Aを関連付け済み | 関連付けを解除して成功する |
| detach(A) | 未関連付け | 冪等に成功し、状態を変更しない |
| detach(B) | Aを関連付け済み | `MessageSinkMismatch`を返し、Aを維持する |

回復可能な失敗ではCallbackを呼ばず、既存関連付けとWindow Lifecycle状態を変更しない。Window破棄は、以後Callbackが
発生しない状態へ遷移してから関連付けを自動解除する。ただし`Destroyed`でのdetachは`destroy()`以外のNative操作であるため
冪等成功にせず、ADR-0006どおりProgramming Contract違反とする。Tool Hostの正常終了経路はWindow破棄前、かつBackend Shutdownと
Context破棄より前に明示detachする。安定Error Code値は#162の実装時に既存Platform Error規約へ追加し、上記Categoryと
状態不変条件をTestで固定する。

この境界はTool UIのNative Input配送に限定し、一般Runtime Input System、IME抽象、Drag and DropはM12対象外とする。

## Ownership and Lifetime

起動順は次とする。

1. Foundation Diagnostics
2. Windows Window と Message Pump
3. Tool用D3D12 Device、Queue、Swap Chain、Descriptor Resource
4. Dear ImGui Context
5. Win32 Platform Backend
6. DirectX 12 Renderer Backend
7. 最終Executable Rootが所有するProject HubまたはEditor Application Service
8. 同Rootが所有するPresentation Adapter State

正常終了は新しいFrame受付を停止し、最後にExecuteしたUI Drawより後ろへTerminal Fence SignalをQueueへ投入してから、
Production既定5,000 msの有限Waitで完了を確認する。GPU完了確認後にだけMessage Sinkをdetachし、DirectX 12 Renderer Backend、
Win32 Platform Backend、ImGui Context、Frame Resource、Descriptor Resource、Swap Chain／Binding、Fence／Event、Queue、Device、
Windowの順に終了する。`ImGui_ImplDX12_Shutdown`相当を含むRenderer Backend ShutdownまたはGPU Resource解放をFence完了確認より
先に行わない。TimeoutをGPU完了として扱わず、Device Removalも確認できない場合はResourceを解放せずFatal終端する。
ImGui Contextは一つのTool Hostが一意所有し、Global SingletonまたはRuntime Serviceへ登録しない。

Tool HostはQueue-globalな`nextFenceValue`を1から開始し、`UINT64_MAX`をDevice Removal検出用として予約する。
UI DrawをExecuteする各SubmitとTerminal Signalは、全ての既発行値より大きい新規未使用値をSignal前に予約し、
Signal成功／失敗にかかわらず巻き戻しまたは再利用を行わない。Frame Resourceの再利用値と最後のSubmit値は、対応する
Executeより後ろへ並ぶこの予約値だけから更新する。Signal失敗後にGPU完了を証明できるのは、その失敗したSignal用に予約した
未使用値へFence Completed Valueが到達した場合だけとし、過去の完了済み値を判定へ使用しない。Completed Valueは大小比較より先に
`UINT64_MAX`か判定し、このSentinelなら予約値への到達として扱わず必ずDevice Removal経路へ移る。Sentinel以外の場合だけ
予約値と比較する。Tool Hostは最後に成功したSignal値を`lastSignaledFence`として保持する。次の予約値が`UINT64_MAX`へ
達する場合は新しいExecuteを開始せず、Terminal Signalも発行しない。`lastSignaledFence`が0なら未提出として直ちに安全な
終了順へ進み、1以上ならその値をProduction既定5,000 msで待つ。この値は直前までの全Execute後に成功したSignalなので、
完了時だけRenderer Backend Shutdown以降の終了順へ進む。待機中の`UINT64_MAX`はDevice Removal経路、完了もRemovalも
証明できない失敗はResourceを解放しないFatal経路とし、Fence値のWrapまたはSentinelのSignalを行わない。

Fence枯渇を検出した時点で`Cue.ToolHost/ToolHostError::FenceValueExhausted`をPrimary Errorとして保存し、未描画Frameを
正常成功へ変換しない。未提出またはDrain成功ではこのErrorを返してProcessを非0で終了する。Wait Error後に既存値の完了を
証明できた場合もFence枯渇をPrimaryに維持し、Wait ErrorをSecondary Contextとする。Drain中にDevice Removalを確認した場合は
Wait Errorを`FenceValueExhausted`へSecondary Contextとして集約してから、その集約ErrorをImmediate Causeとして
`ToolHostError::DeviceRemoved`へ再分類し、Removal ReasonをNative Errorとして待機なし制御解放へ進む。
完了もRemovalも証明できない場合はWait／Removal確認Errorを`FenceValueExhausted`へ発生順のSecondary Contextとして集約し、
その集約ErrorをImmediate Causeに`ToolHostError::GpuCompletionUnavailable`へ再分類してResource解放前にFatal Dispatchする。

M12 Tool Hostは2個のSwap Chain Bufferと2個のFrame Resource Slotを固定で所有し、Dear ImGui DX12 Backendの
`NumFramesInFlight`も2とする。Frame SlotはSwap Chain Indexではなく単調なFrame Sequenceの`mod 2`で選び、同じ順序で
BackendのFrame用Vertex／Index Bufferを使用する。Back Bufferは毎Frame `GetCurrentBackBufferIndex()`で別に選択する。
各SlotはCommand Allocatorと、そのSlotを最後に使用したSubmitの`reuseFenceValue`を所有する。

Frame開始時はImGui DX12 BackendのNew Frame、Allocator Reset、Command List Reset、Frame用Buffer更新より前に選択Slotを検査する。
`reuseFenceValue`が0なら未使用として進み、1以上ならCompleted Valueの`UINT64_MAX` Sentinelを大小比較より先に判定する。
Sentinel以外で対象値へ未到達ならProduction既定5,000 msの有限Fence Waitを行い、復帰後もSentinelと対象値を再検査する。
完了時だけSlot Resourceを再利用し、Device Removalは待機なし制御解放、完了もRemovalも証明できない失敗は
`ToolHostError::GpuCompletionUnavailable`のFatal経路へ移ってAllocatorまたはImGui Frame Bufferを変更しない。
有限WaitがTimeout、`WAIT_FAILED`、予期しない結果になった場合はFrame受付を停止し、再検査でRemovalを最優先する。
Removal未確認で対象Slotの`reuseFenceValue`完了を証明できても、現在FrameのAllocator／Command List／ImGui BufferをResetまたは
更新しない。この値は2 Frame前のSubmitまでしか覆わない可能性があるため、GPU Resourceを直ちに解放せず、Frame受付停止時点の
`lastSignaledFence`を全提出WorkのDrain対象として固定する。`lastSignaledFence`が対象値以下で既に完了済みなら追加Waitを省略し、
それより大きく未完了なら、失敗したWait Eventを再利用せず新規の一時Eventを生成して`SetEventOnCompletion`相当を登録し、
Production既定5,000 msの有限Waitを行う。再検査ではCompleted Valueの`UINT64_MAX`を大小比較より先に判定し、Device Removalを
完了より優先する。`lastSignaledFence`まで完了した場合だけGPU Resourceを安全な順序で解放し、最初のWait ErrorをPrimary、
追加Drain Errorを発生順のSecondary DiagnosticsとしてProcessを非0終了する。Removalなら既定の`DeviceRemoved`へ昇格し、
完了もRemovalも証明できない場合は集約済みWait ErrorをImmediate Causeに`GpuCompletionUnavailable`としてResource解放前に
Fatal Dispatchする。Wait Eventを置換して同一Sessionを継続することは行わない。

UI DrawはCommand List Execute、Present、Queue Signalの順に行う。Presentの非Removal失敗ではPresent ErrorをPrimaryとして保存し、
Frame受付を停止した上で、実行済みWorkを覆う新規予約値のQueue Signalを試みる。Signal成功時だけ予約値をSlotの
`reuseFenceValue`と`lastSignaledFence`へ保存し、その値を有限DrainしてからGPU Resourceを安全な順序で解放し、Present Errorを
返してProcessを非0終了する。補完Signal失敗後に予約値の完了だけを証明できた場合はPresent ErrorをPrimary、Signal／Wait Errorを
発生順のSecondary Diagnosticsとして安全終了する。Device Removalは`DeviceRemoved`へ昇格し、完了もRemovalも証明できなければ
集約済みPresent／Signal／Wait ErrorをImmediate Causeに`GpuCompletionUnavailable`としてResource解放前にFatal Dispatchする。
Present成功時は通常Signal成功後だけ予約値をSlotの`reuseFenceValue`と`lastSignaledFence`へ保存して次Frameへ進む。

Windowの`Minimized`またはClient Sizeが0の間はUI Frame提出と`ResizeBuffers`を行わない。有効な`Resized`／`Restored` Eventでは
新しいFrame受付を一時停止し、`lastSignaledFence`が0なら未提出、1以上なら上記Sentinel優先の5,000 ms有限Waitで全提出Workを
Drainする。GPU完了後に全Back Buffer参照を解放し、`ResizeBuffers`、全Back Buffer再取得、RTV再生成の順で処理してから
Frame受付を再開する。成功後は全Slotの完了済み`reuseFenceValue`を0へ戻す。
Wait Timeout／Wait失敗は完了とRemovalを再検査し、Removalなら待機なし制御解放、どちらも証明できなければResourceを解放せず
Fatal終端する。`ResizeBuffers`またはBack Buffer／RTV再生成の非Removal失敗は新しいFrameを受け付けず、GPU完了済みResourceを
Best-effortで解放して`ToolHostError::SwapChainResizeFailed`を返し、Tool Sessionを非0で終了する。

一つのUI FrameはOwner Threadだけで処理する。Windows Message、ImGui Frame、Semantic Intent適用、Application Service Mutation、
ViewModel再取得、Draw Data提出を同じThreadで順序付ける。Background ThreadからImGui APIまたはProjectHubServiceを直接呼ばない。

## Presentation Contract

ImGui Adapterは表示用ViewとPresentation Stateだけを読み、User操作をSemantic Intentへ変換する。

- ViewのStable IdentityはProjectId、ObjectId、ComponentInstanceIdを使用する
- ImGui ID、Row Index、Pointer、Pathだけを長期Identityにしない
- Text Buffer、Focus、Popup、選択中Template、確認DialogはPresentation Stateが所有する
- Filesystem、Descriptor、Serializer、RecentProjectRegistryを直接操作しない
- Service操作後は成功／失敗にかかわらず公開Viewを再取得する
- Error CodeとContextをPresentation側の安定Mappingで日本語表示する
- Progressは実Operation Stateだけを表示し、同期処理を偽の非同期Progressとして表現しない

## Error and Diagnostics Contract

- External Dependency未復元は専用Dependency Checkで失敗させ、通常のCMake ConfigureからDownloadを自動開始しない
- Backend初期化失敗は部分Hostを逆順に破棄し、Process Exit CodeとFoundation Errorへ記録する
- Device RemovalをFenceの`GetCompletedValue()`、Queue Signal、Fence Wait、Present、Resizeの失敗または
  `ID3D12Device::GetDeviceRemovedReason()`から確認した時点で新しいFrameとQueue操作を停止する
- Device Removal確認後はFenceをSignalまたはWaitせず、Removal Reasonを記録してDREDを一度だけBest-effortで採取する。
  DRED採取失敗は制御解放を妨げず、Message Sink detach、DirectX 12 Renderer Backend、Win32 Platform Backend、ImGui Context、
  Command Resource、Back Buffer、RTV／SRV Heap、Swap Chain／Binding、Fence／Event、Queue、Device、Windowの順に待機なしで解放し、
  `ToolHostError::DeviceRemoved`をPrimary、Removal ReasonをNative Errorとする。Device Removal検出前のErrorが0件ならCauseなし、
  1件ならそのErrorをImmediate Causeとして保持する。複数ある場合は最初のErrorをPrimaryに維持して後続Errorを
  `append_secondary_diagnostics()`で発生順に集約し、そのAggregate全体を再分類時の一つのImmediate Causeとして保持する。
  独立してDevice Removal確認後に発生するDRED／Cleanup ErrorはCause Chainへ加えず、
  `append_secondary_diagnostics()`で発生順のSecondary Contextとして保持する。Device Removalでは先行Error維持より本規則を優先する
- Fence Signal／Wait／待機Primitiveが失敗した場合は、Fence完了とDevice Removalの再検査結果を先に集める。
  Device Removalを確認できればFence完了も同時に観測していてもRemoval経路を最優先し、Removal未確認でFence完了だけを
  証明できた場合に限り安全なResource解放を最後まで行って先行Errorを返す。どちらも確認できなければGPUが参照し得る
  Native Resourceを解放せず、ErrorとContextを一度Log／FlushしてFatal HandlerでProcessを終端する
- 自動Device再生成と同一Process内RecoveryはM12対象外とする
- UI AdapterはApplication ServiceのErrorを握りつぶさず、安定Categoryと操作対象を日本語Messageへ変換する
- `DurabilityUnknown`は成功表示に変換せず、公開済み可能性と再確認手順を表示する
- `open_project()`拒否後の一覧Refreshが`DurabilityUnknown`になった場合は
  `OpenRejectedViewDurabilityUnknown`を返し、Immediate Causeへ元のOpen拒否Categoryを保持する

## Validation Contract

Headless TestはDear ImGuiのPixel出力に依存せず、次を検証する。

- `IMGUI_HAS_DOCK`、固定Version、`ImGuiConfigFlags_DockingEnable`、`ImGui::DockSpace`の実Link Symbol
- ViewModelから表示Row状態へのMapping
- Semantic Intentの生成と無効操作抑止
- Keyboard Activate、Focus移動、Escape Cancel
- Create／Register／Open／Pin／Remove確認
- Missing／Broken／Compatibility／DurabilityUnknownの日本語Message
- Service Mutation後に旧Viewを再利用しないこと

Backend Integration TestはWindowとTool D3D12 Resourceを生成し、自動Close可能なSmoke ModeでFrameを提出する。
Fault Injection TestはTerminal Signal／Wait成功、Timeout、Device Removal、Signal失敗後の予約値完了を分離し、GPU完了前に
Renderer Backend Shutdownが呼ばれないこと、Fence値を再利用しないこと、Device Removalだけが待機なし解放へ進むこと、
完了もRemovalも証明不能な経路がResource解放前にFatal Dispatchすることを検証する。
Fence枯渇は`nextFenceValue`と`lastSignaledFence`を注入し、未提出0値の即時Cleanup、1以上のSignalなしDrain、Timeout、
Device Removal、非Wrap／非Sentinel Signal、Primary ErrorとProcess非0終了を個別に検証する。
Signal／Wait／Present／Resize、`GetCompletedValue()`の`UINT64_MAX`、`GetDeviceRemovedReason()`直接確認の各Device Removal経路は、
Fence完了との同時成立、先行Error、DRED失敗、複数Cleanup Errorを注入する。`ToolHostError::DeviceRemoved`が常にPrimary、
Removal ReasonがNative Errorになることを検証する。検出前Error 0件ではCauseなし、1件ではそのError、複数では最初のErrorを
Primaryとして後続を発生順のSecondary Diagnosticsへ集約した一つのAggregateがImmediate Causeとなることを個別に検証する。
DRED／Cleanup ErrorはDeviceRemoved Errorの発生順Secondary Contextになり、独立ErrorがCause Chainへ直接入らないことも検証する。
複数FrameのFault Injectionは同じSlotへ戻る前の未完了Fence、Sentinel、有限Wait成功／Timeout／Wait失敗／Removalを注入し、
完了前にAllocator Reset、ImGui Frame Buffer更新、Command List Resetを行わないことと、成功Signal値だけをSlotへ保存することを検証する。
Wait失敗後に対象Slotの完了を観測するRaceでは、別Slotのより大きい`lastSignaledFence`を未完了状態で注入する。SlotをReset／再利用せず、
失敗したEventとは別の一時Eventで全提出Workを有限Drainし、その完了後だけResourceを解放してWait ErrorをPrimaryに非0終了することを
検証する。追加DrainのEvent生成／登録／Wait失敗、Timeout、Device Removal、`UINT64_MAX`、完了とRemovalの同時成立も個別に注入し、
完了もRemovalも証明できない場合はResourceを解放せずFatal Dispatchすることを検証する。
Presentの非Removal失敗は補完Signal成功、Signal失敗後の完了、Device Removal、安全性不明を注入し、Frame受付停止、
Present Error維持またはDeviceRemoved／GpuCompletionUnavailableへの規定どおりの昇格、Drain後の非0終了を検証する。
Resize Integration Testは提出済みFrameをDrainした後にだけBack Bufferを解放し、`ResizeBuffers`とRTV再生成を行ってFrame提出を
再開できることを確認する。最小化／0 Size抑止、Timeout、Device Removal、`ResizeBuffers`／再生成失敗も個別に検証する。
Manual TestはProject作成、既存Project登録、Pin、一覧除外、Keyboard操作、Cancel、Editor Launch要求、正常Closeを確認する。

Pixel完全一致、Theme、Font Raster差分、Docking、Multi-ViewportはM12 Gateに含めない。

## Consequences

### Positive

- 実動ImGui UIと外部Code非混在を両立できる
- Registry Baseline、Port Version、LicenseをReview可能な形で固定できる
- vcpkg Tool自体のRevisionも開発機とCIで照合できる
- Tool UIがRuntime RendererとRHI公開APIを拡張せずに成立する
- Project HubとEditor CoreをHeadlessに維持できる
- 後続のFiles、Play、Build UIが同じHost境界を再利用できる

### Trade-offs

- vcpkg本体、Registry、Port DefinitionがBuild Inputに増える
- Clean Checkout後に明示的なDependency Restoreが必要になる
- Tool専用D3D12 ResourceはRuntime RHIと実装責務が一部重複する
- Upstream Update、License Notice、Supply Chain Reviewの継続運用が必要になる
- Docking Branchはupstreamで安定化前のため、Version更新時にAPI／ABI差分の再検証が必要になる
- Presentation Adapter、Host、Backendを分離するためTarget数が増える

### Mitigations

- 承認対象、vcpkg Tool Commit、Registry、Feature、Version、更新手順を固定する
- External TargetへWarningとInclude境界を限定し、First-party Warningを抑止しない
- Tool Hostの重複をM12最小Scopeに限定し、Runtime Rendererへ逆流させない
- Dependency取得、Hash、License、3構成BuildをCI Gateへ追加する
- Compile Version TokenとDockSpace実Link契約Testで、既存Build Treeに古いHeader／Libraryが残る不整合を検出する

## Rejected Shortcuts

### Mock SurfaceだけをImGui UIとして完了扱いにする

Headless Testには有用だが実動Window、Input、Backend、手動Workflowを満たさないため採用しない。

### Project Hub AdapterからD3D12 Deviceを直接所有する

UI責務とGPU Resource Lifetimeが混在し、#162の依存GateとADR-0018へ反するため採用しない。

### RuntimeHostまたはGame Swap ChainへProject Hubを埋め込む

Tool起動にRuntime WorldとGame Rendererを要求し、RuntimeからEditorへの依存を作るため採用しない。

## Approval Record

2026-09-04にUserは、第三者Codeを`Engine`から分離して`ThirdParty`配下でLicenseに従い管理すること、今後の外部Libraryは
導入前に毎回確認すること、導入手段をvcpkgへ限定すること、Dear ImGuiをM12へ導入することを明示承認した。

2026-09-17にUserは、Dear ImGuiをDockingを含む最新版へ更新することを明示承認した。#327では
`v1.92.9b-docking`の利用可能化だけを行い、DockSpace配置、Layout永続化、Multi-Viewportは後続Issueへ分離する。

## Follow-up

- #162でDependency Pin、External Target、Tool Host、Project Hub ImGui Adapterを最小実装する
- #163で同じHostへHierarchy／Inspector Adapterを追加する
- #164でProject HubとEditor Process Workflowを統合する
- #165でClean Checkout、3構成、Headless、Backend Smoke、手動UI Workflowを検証する
- #327でDear ImGui `v1.92.9b-docking`のPin、増分Build契約、Docking Compile／Link契約を検証する
- EditorのDockSpace配置とLayout永続化はM21の別Issueで設計・実装する

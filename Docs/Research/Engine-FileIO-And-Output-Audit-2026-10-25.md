# Engine の File I/O・ログ・診断出力の一覧

2026-10-25、`m06/issue144/gpu-waits-and-telemetry` の現在の Source と未 Commit の調査コードを静的確認した記録。

対象は First-party `Engine/Source`、Engine が使う ImGui の経路、手動計測 Probe、Capture / 解析 Script、直近の比較 Harness。未呼出しの第三者 API 全般や、OS / Driver 内部の実 I/O を網羅した実行 Trace ではない。

Source の変更、Build、アプリ起動は行っていない。本レポートだけを追加した。

## 1. 通常起動時の保存先

Repository の `CueEditorHost` と `CueWindowsHost` は Main で `StorageMode::Development` を明示するため、Build 構成にかかわらず以下を使う。

| 用途 | Path |
| --- | --- |
| dataRoot | `C:/Users/sinse/source/repos/CueEngine/out` |
| 通常ログ | `out/logs/CueEngine-<epoch milliseconds>-<PID>-<sequence>.log` |
| 性能設定 | `out/editor/Performance.cfg` |
| ImGui Layout | `out/editor/imgui.ini` |
| Cache Directory | `out/cache/shaders`、`out/cache/pso` |

保存先は `dataRootOverride` で変更可能。汎用 Host の既定 Product Mode は `%LOCALAPPDATA%/CueEngine/CueEngine`、Portable は実行ファイルの Directory。Application / Company の設定で Product の Path は変わる。

根拠: [StoragePaths.cpp](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/StoragePaths.cpp:56)、[Editor Main](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Main.cpp:41)、[Windows Main](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Main.cpp:24)。

`WindowsHost::initialize()` は Root、logs、cache/shaders、cache/pso の Directory 作成を試みる。File ログを無効にしてもこの Directory 作成は残る。Cache Directory があることと Cache 内容の保存実装があることは別である。

## 2. 通常アプリのファイル内容 I/O

| 場所 | 種別 | 対象・発生条件 |
| --- | --- | --- |
| [WindowsHost::initialize](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:474) | Read | 起動時、性能設定の存在を確認し、存在すれば読む |
| [load_performance_settings](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/PerformanceSettings.cpp:149) | Read | 性能設定を最大4096 Byte読む。起動時 / EditorのLoad saved操作 |
| [save_performance_settings](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/PerformanceSettings.cpp:137) | Write | Apply and save 等の明示要求。文字列をRAMで組み立て、replace_fileで保存 |
| [EditorHost::build_ui](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/EditorHost.cpp:396) | Write呼出 | 設定適用完了後、保存要求を一度処理。Main上の同期I/O |
| [ImGuiManager::create](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/ImGuiManager.cpp:219) | Read | 起動時、Layout INIが存在すれば最大4 MiB読む |
| [ImGuiManager::end_frame](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/ImGuiManager.cpp:1004) | Write呼出 | WantSaveIniSettingsが立ったFrameでsave_settingsを呼ぶ |
| [ImGuiManager::save_settings](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/ImGuiManager.cpp:1124) | Write | LayoutをSaveIniSettingsToMemoryでRAMへ整形し、Directory作成後replace_file。Context MutexはI/O前に解放 |
| [ImGuiManager::shutdown](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/ImGuiManager.cpp:1189) | Write呼出 | ReadyなContextの停止時にもLayoutを保存 |
| [DX12ShaderCompiler::compile](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12ShaderCompiler.cpp:293) | Read | Shaderコンパイル要求ごとにHLSL Sourceを読む |
| [FileIncludeHandler::LoadSource](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12ShaderCompiler.cpp:77) | Read | DXCが要求したIncludeごとに読む |
| [DX12ShaderCompiler初期化](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12ShaderCompiler.cpp:188) | DLL Load | dxcompiler.dllをLoadLibraryExW。実行ファイル隣を優先、なければBuild時解決のSDK Path |

性能設定 / Layout は毎Frame無条件で読み書きする構造ではない。ただし Layout 保存要求が立つと、実行中のMain処理で同期保存が発生する。

性能設定名を空にすれば設定の自動読込み・保存を無効化できる。ImGui の `settingsFile` を空にすればLayoutの内容I/Oを無効化できる。

Shader Root は実行ファイル隣の `EngineResources/Shader` を優先し、存在しなければ Repository の `Engine/Shader`。標準表示 Pass の [PresentToSwapChainPass](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/Passes/Public/Passes/PresentToSwapChainPass.h:55) は `Hlsl/FullscreenTriangle.hlsl` を VS / PS でコンパイルする。Graph構築などの要求時で、通常の毎Frame読込みではない。Editor既定表示はImGuiPassへ差し替える。

First-party の Shader Bytecode / 外部PDB / PSO Cache のファイル保存は見つからなかった。DebugのDXCオプションは `-Zi -Qembed_debug -Od` で、取得BytecodeにDebug情報を埋め込む。PipelineManagerの登録先はRAM内のRegistry。DriverやCompiler内部の暗黙Cache I/Oは今回未検証。

## 3. ログ・標準出力・Debugger出力

| 入口・実装 | 出力先・条件 |
| --- | --- |
| [HostLoggingConfig](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Public/WindowsHost/HostLogging.h:18) | 既定Info、File有効、Debug出力Automatic。Debug / DevelopmentではAutomaticが有効 |
| [create_host_logger](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:49) | 起動時にログFileをCreateNew。既存Fileを上書きしない |
| [report_log / report_log_error](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/Diagnostics.cpp:158) | 登録Loggerへ配送。report_errorも共通ログ経路を使う |
| [Logger::write](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Logging/Private/Logger.cpp:140) | 呼出Thread上でMutexを保持して整形・Sink配送。専用非同期ログThreadはない |
| [FileLogSink::write](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Logging/Private/FileLogSink.cpp:59) | IFile::write経由で同期WriteFile。部分書込み時はループ。毎行Flushはしない |
| [Logger::flush / shutdown](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Logging/Private/Logger.cpp:180) | 明示Flush、Fatal配送後、停止・回収時にFlush。FileLogSink::closeもFlushする |
| [WindowsDebugLogSink::write](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsDebugLogSink.cpp:27) | UTF-8をUTF-16へ変換してOutputDebugStringW。Engine自身によるFile保存ではない |
| [緊急出力](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/Diagnostics.cpp:25) | Logger未登録・再入・故障時、CUE_DEBUG_OUTPUTが有効ならOutputDebugStringW。HostのdebugOutput設定とは別 |
| [Runtime Destructor](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Runtime/Private/Runtime.cpp:71) | Cleanup失敗時にfprintf(stderr)。Hostのログ設定を経由しない。リダイレクト先がFileならFile出力になる |

通常Hostの成功ログは `initializing`、`initialized`、`performance settings applied`、`stopped`。その他のRenderer等の呼出はSetName失敗、Reset / Submit / Cleanup失敗などが中心。健全なMain / Update / Render Loopに、計測結果を毎Frame通常Fileログへ出す処理は見つからなかった。呼出箇所は後述の自動抽出一覧にも収録する。

`snprintf()`、`std::format()`、SaveIniSettingsToMemoryはRAM内の整形であり、それだけでFileやConsoleへ出力するものではない。

## 4. ImGui / D3D12 / PIXの独立した診断経路

同梱ImGui確認先: `ThirdParty/vcpkg_installed/vcpkg/blds/imgui/src/9b-docking-9ae2fb46a1.clean/`。

| 場所 | 動作 |
| --- | --- |
| [imgui.cpp:4510](C:/Users/sinse/source/repos/CueEngine/ThirdParty/vcpkg_installed/vcpkg/blds/imgui/src/9b-docking-9ae2fb46a1.clean/imgui.cpp:4510) | 既定DebugLogFlagsがEventError / OutputToTTY |
| [imgui.cpp:24054](C:/Users/sinse/source/repos/CueEngine/ThirdParty/vcpkg_installed/vcpkg/blds/imgui/src/9b-docking-9ae2fb46a1.clean/imgui.cpp:24054) | Error / Debug Event時にIMGUI_DEBUG_PRINTF。通常はprintfで、HostLoggingと独立 |
| [imgui.cpp:24059](C:/Users/sinse/source/repos/CueEngine/ThirdParty/vcpkg_installed/vcpkg/blds/imgui/src/9b-docking-9ae2fb46a1.clean/imgui.cpp:24059) | OutputToDebugger設定時にOutputDebugStringA |
| [ImGui::LogToFile](C:/Users/sinse/source/repos/CueEngine/ThirdParty/vcpkg_installed/vcpkg/blds/imgui/src/9b-docking-9ae2fb46a1.clean/imgui.cpp:16264) | 指定先またはimgui_log.txtへ追記するAPI。ただしFirst-party Engineからの呼出は見つからない |
| [ImGuiManager初期化](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/ImGuiManager.cpp:249) | io.IniFilename=nullptrでCoreの直接自動保存を無効化。Engine側のLayout保存は別に残る |
| [公式DX12 Backend](C:/Users/sinse/source/repos/CueEngine/ThirdParty/vcpkg_installed/vcpkg/blds/imgui/src/9b-docking-9ae2fb46a1.clean/backends/imgui_impl_dx12.cpp:806) | 埋込みShader文字列をD3DCompile。外部HLSL読込みではない。Device Object生成・再生成時 |
| [公式Win32 Backend](C:/Users/sinse/source/repos/CueEngine/ThirdParty/vcpkg_installed/vcpkg/blds/imgui/src/9b-docking-9ae2fb46a1.clean/backends/imgui_impl_win32.cpp:238) | XInput DLLのLoadLibraryA。DPI HelperにもDLL Loadがある |
| [DX12RenderDevice](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:62) | Debug限定でDebug Layer / GPU Validation / DRED、InfoQueue。SDK診断の出力経路。Engineによる毎FrameのFile Dumpなし |
| [DX12ResourceLeakChecker](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12ResourceLeakChecker.cpp:30) | DebugのBackend正常停止時にReportLiveObjects。SDK側診断出力。失敗時は共通LoggerのWarning |
| [DX12FrameGraphExecutor](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12FrameGraphExecutor.cpp:443) | PassごとにPIXBeginEvent / PIXEndEvent。CommandList Markerで、この関数がCapture Fileを開くわけではない |

既定FontはAddFontDefaultから埋込みデータを展開し、外部TTFを読まない。第三者APIとしてのAddFontFromFileTTFは存在するが、現在のEngineからの呼出は見つからなかった。

副次所見: settingsFileが空のsave_settingsはWantSaveIniSettingsを消さずに成功を返すため、要求発生後に毎Frame保存関数へ入る可能性がある。ただし空Pathの早期returnでFile I/Oは発生しない。今回未修正。

## 5. 実際に読み書きするPlatform層

すべて [WindowsFileSystem.cpp](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp) の同期実装。

| 処理 | 行 / Native API | 種別 |
| --- | --- | --- |
| read | 142 / ReadFile | File内容の読込み |
| write | 157 / WriteFile | File内容の書込み |
| flush | 220 / FlushFileBuffers | 明示的なFlush |
| open | 516 / CreateFileW | Open / 新規作成 / 上書き。要求指定による |
| stat / size | 287 / GetFileAttributesExW、207 / GetFileSizeEx | Metadata照会 |
| list_directory | 369 / FindFirstFileW、404 / FindNextFileW | Directory列挙 |
| create_directories | 332 / CreateDirectoryW | Directory作成 |
| remove | 426 / DeleteFileW、RemoveDirectoryW | 削除 |
| rename | 444 / MoveFileExW | 改名 / 移動 |
| copy_file | 457 / CopyFileW | 内容コピー |
| replace_file | 530、606、615、626 | 同Directoryの.cue-GUID.tmpへWrite→Flush→Close→置換Rename |

`IFileSystem::exists`はstatのMetadata照会。[read_all](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Private/FileSystem.cpp:15)はOpen / Size / Read / EOF・Size再確認 / Close、[write_all](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Private/FileSystem.cpp:71)はOpen / Write / Flush / Closeの共通処理。呼出しなしに自発的に動く機能ではない。

通常WriteFileと、Flushによる永続化要求は区別する。Loggerは毎行Flushしないが、設定のreplace_fileではFlushとWRITE_THROUGH付きRenameを行う。

## 6. 手動計測・Capture・解析ツール

これらは通常Editorの起動だけで実行されるものではない。

| 場所 | Read / Write / Outputとタイミング |
| --- | --- |
| [SpikeTimingProbe.cpp](C:/Users/sinse/source/repos/CueEngine/Engine/Tests/Performance/SpikeTimingProbe.cpp:373) | 即時ETWモードは記録中にTraceLoggingWrite。DeferredモードはRAM保存後に送出 |
| 同:448 / 491 / 1006 | GPU / CPU区間CSVは計測停止・Shutdown後に保存。開始前と終了後にstdout / エラー時stderr |
| 同:926 | Trigger時は計測停止後にfreeze.flagを書き、frozen.flagの存在を繰り返し確認 |
| [FocusExperiment.h](C:/Users/sinse/source/repos/CueEngine/Engine/Tests/Performance/FocusExperiment.h:250) | Focus履歴CSV。配置・起動・結果をstdoutへ出す |
| [EditorTimingProbe.cpp](C:/Users/sinse/source/repos/CueEngine/Engine/Tests/Performance/EditorTimingProbe.cpp:172) | CPU / Timeline / GPU readiness / Cycles CSV、Metadata JSON、Dispatch CSV等。測定・Host停止後の保存関数から出力 |
| [PresentDispatchProbe.h](C:/Users/sinse/source/repos/CueEngine/Engine/Tests/Performance/PresentDispatchProbe.h:184) | Dispatchの記録をCSVへ保存 |
| [ImGuiDescriptorLookupProbe.cpp](C:/Users/sinse/source/repos/CueEngine/Engine/Tests/Performance/ImGuiDescriptorLookupProbe.cpp:248) | 単独比較の結果CSV、エラーをstderrへ出力 |
| [FrameControllerTimingProbe.cpp](C:/Users/sinse/source/repos/CueEngine/Engine/Tests/Performance/FrameControllerTimingProbe.cpp:81) | 各条件の結果をstdout、失敗をstderr。File保存はリダイレクト先次第 |
| [M09FramePacingProbe.cpp](C:/Users/sinse/source/repos/CueEngine/Engine/Tests/Performance/M09FramePacingProbe.cpp:65) | 結果をstdout。Host logging既定を変更していないので、通常Hostのログも対象 |
| [SpikeEtwExport.cpp](C:/Users/sinse/source/repos/CueEngine/Engine/Tests/Performance/SpikeEtwExport.cpp:300) | ETLをOpenTrace / ProcessTraceで読み、CSVをfprintf。解析時の処理 |
| [capture_spike_etw.ps1](C:/Users/sinse/source/repos/CueEngine/scripts/capture_spike_etw.ps1:90) | 通常モードはxperfのKernel.etl / UserGpu.etl / Markers.etlを保存。TriggerのKernel / UserGPUはBufferingで保持し、停止前にFlush保存。設定JSON、開始終了ログ、制御Flag、Merged.etl、Probeのstdoutログも保存 |
| [RunPresentationPathStudy.ps1](C:/Users/sinse/source/repos/CueEngine/Engine/Tests/Performance/RunPresentationPathStudy.ps1:78) | PresentMonの--output_fileで実行中のPresent CSV。stdout / stderrのFile Redirect、Environment / 結果JSONも保存 |
| [RunEditorTimingStudy.ps1](C:/Users/sinse/source/repos/CueEngine/Engine/Tests/Performance/RunEditorTimingStudy.ps1:28) | Environment JSON、Probeの結果、Summary CSV等を保存 |
| scripts/analyze_*.py / validate_spike_capture.py | 保存済みCSV / Log / ETL変換結果を読み、JSON / CSV / JSONLとConsole結果を出す。解析時だけ |
| Engine/Tests/Performance/AnalyzePixPresent.py / Summarize*.py | PIX等の出力や各比較結果を読み、集計結果を保存 / stdout出力。手動解析 |
| [過去実験StudyImGuiRecorder.h](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/StudyImGuiRecorder.h:76) | 未追跡の過去実験Source。D3DCompileFromFileでShaderを読む経路。現在の通常Engine経路とは分ける |

Capture Tool自身のI/Oと、対象Engine ProcessのI/Oは別Processの場合がある。外部Toolが書くETL / CSVをEngine自身のWriteFileと混同しない。

直近の比較は以下の専用Harnessを使用した。

- `out/validation/Performance20261024/DevelopComparison/Harness/CommonProbe.cpp`: Shutdown後にCSV / stdout。RunComparison.ps1はProcess終了後にLog / JSON保存
- `out/validation/Performance20261024/HotspotStudy/Harness/Recorder.h`: 計測中は固定RAM、Shutdown後にmetrics.csvへ保存。CommonProbe.cppは同様に終了後CSV / stdout。RunStudy.ps1もProcess終了後に保存
- 両HarnessはHostのFile / Debugログを無効にし、ImGui settingsFileも空にしている。ただしLibrary独自の異常診断出力やDriver内部I/Oまでゼロと証明したものではない

## 7. テストとBuild時だけのI/O

- `Engine/Tests/Support/TemporaryFiles.h`: `%TEMP%/cue-io-<PID>-<time>`を作り、Test後に列挙・削除。失敗時OutputDebugStringA
- FileSystem / Logging / Settings / Shader等のTestはFixtureへRead / Write / Replaceして検証する。通常アプリの毎Frame動作とは別
- `scripts/test_*.py`: 一時DirectoryへCSV / JSON / LogのFixtureを作って解析関数を検証
- `scripts/codex_build.ps1` / CMake / MSBuild: Source読込み、out/build以下へProject / Object / Lib / EXE / PDB / Log等を生成
- `Engine/Source/Renderer/DX12/CMakeLists.txt`: Generated Header作成、DXC DLLとShader DirectoryのBuild後コピー
- `scripts/prepare_dependencies.ps1`: vcpkg install。out/vcpkg/downloads、out/vcpkg/binary-cache、ThirdParty/vcpkg_installed等の読書き

## 8. 一覧の解釈と残る範囲

- GPU Upload / Readback / Map、Descriptor書込みはMemory操作であり、File内容のI/Oには数えない
- 正常な毎Frame計測は主にRAM保持。実行中のFile書込みで注意するのはLayout保存、明示的な設定保存、ログ発生時、Captureモード
- File名・ソース位置の文字列整形やFolder Path解決だけでは、File内容のRead / Writeとは言わない。Metadata照会も別に表示した
- OS Loader、Driver Cache、Debugger保存、Windowsの暗黙I/Oはこの静的調査だけでは全件特定できない。Process全体のI/O量・実Path・所要時間の確認には別途実行Traceが必要
- Sourceの変更・Build・再計測は行っていないため、追加検証による新たな性能値はない

## 9. Engine/Source 内の関連API・呼出行の抽出一覧

以下は確認時点のSourceから抽出した参照。呼出元だけでなく共通入口・宣言も含む。実行頻度・有効条件は上記を参照する。

| 場所 | Source |
| --- | --- |
| [Engine/Source/EditorHost/Main.cpp:15](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Main.cpp:15) | `cue::report_log_error("CueEditorHost", a_error, cue::LogLevel::Fatal);` |
| [Engine/Source/EditorHost/Main.cpp:19](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Main.cpp:19) | `cue::report_log_error("CueEditorHost cleanup", *stopResult.try_error(), cue::LogLevel::Error);` |
| [Engine/Source/EditorHost/Main.cpp:37](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Main.cpp:37) | `cue::report_log_error("CueEditorHost storage", *root.try_error(), cue::LogLevel::Fatal);` |
| [Engine/Source/EditorHost/Main.cpp:69](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Main.cpp:69) | `cue::report_log_error("CueEditorHost", *stopResult.try_error(), cue::LogLevel::Fatal);` |
| [Engine/Source/EditorHost/Main.cpp:76](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Main.cpp:76) | `cue::report_log("CueEditorHost", "unexpected C++ exception", cue::LogLevel::Fatal);` |
| [Engine/Source/WindowsHost/Main.cpp:21](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Main.cpp:21) | `cue::report_log_error("CueWindowsHost storage", *root.try_error(), cue::LogLevel::Fatal);` |
| [Engine/Source/WindowsHost/Main.cpp:32](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Main.cpp:32) | `cue::report_log_error("CueWindowsHost", *initResult.try_error(), cue::LogLevel::Fatal);` |
| [Engine/Source/WindowsHost/Main.cpp:36](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Main.cpp:36) | `cue::report_log_error("CueWindowsHost cleanup", *stopResult.try_error(), cue::LogLevel::Error);` |
| [Engine/Source/WindowsHost/Main.cpp:47](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Main.cpp:47) | `cue::report_log_error("CueWindowsHost", *stepResult.try_error(), cue::LogLevel::Fatal);` |
| [Engine/Source/WindowsHost/Main.cpp:51](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Main.cpp:51) | `cue::report_log_error("CueWindowsHost cleanup", *stopResult.try_error(), cue::LogLevel::Error);` |
| [Engine/Source/WindowsHost/Main.cpp:65](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Main.cpp:65) | `cue::report_log_error("CueWindowsHost", *shutdownResult.try_error(), cue::LogLevel::Fatal);` |
| [Engine/Source/WindowsHost/Main.cpp:72](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Main.cpp:72) | `cue::report_log("CueWindowsHost", "unexpected C++ exception", cue::LogLevel::Fatal);` |
| [Engine/Source/EditorHost/Private/DX12ImGuiBackend.cpp:96](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/DX12ImGuiBackend.cpp:96) | `report_log_error("ImGui DX12", {ErrorCategory::Fatal, a_operation}, LogLevel::Fatal);` |
| [Engine/Source/EditorHost/Private/DX12ImGuiBackend.cpp:295](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/DX12ImGuiBackend.cpp:295) | `report_log_error("ImGui DX12 cleanup", *result.try_error(), LogLevel::Fatal);` |
| [Engine/Source/Runtime/Private/Runtime.cpp:71](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Runtime/Private/Runtime.cpp:71) | `std::fprintf(stderr, "Runtime cleanup: %s (nativeCode=%lld)\n",` |
| [Engine/Source/EditorHost/Private/EditorHost.cpp:247](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/EditorHost.cpp:247) | `report_log_error("CueEditorHost cleanup", *result.try_error(), LogLevel::Error);` |
| [Engine/Source/EditorHost/Private/ImGuiManager.cpp:219](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/ImGuiManager.cpp:219) | `auto loaded = state.files->read_all(*path.try_value(), 4 * 1024 * 1024);` |
| [Engine/Source/EditorHost/Private/ImGuiManager.cpp:319](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/ImGuiManager.cpp:319) | `report_log_error("ImGuiManager cleanup", *result.try_error(), LogLevel::Error);` |
| [Engine/Source/EditorHost/Private/ImGuiManager.cpp:1124](C:/Users/sinse/source/repos/CueEngine/Engine/Source/EditorHost/Private/ImGuiManager.cpp:1124) | `auto saved = m_state->files->replace_file(*path.try_value(), std::as_bytes(std::span(settings)));` |
| [Engine/Source/Platform/Private/FileSystem.cpp:15](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Private/FileSystem.cpp:15) | `Result<std::vector<std::byte>> IFileSystem::read_all(const Path &a_path, std::size_t a_maxBytes)` |
| [Engine/Source/Platform/Private/FileSystem.cpp:71](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Private/FileSystem.cpp:71) | `Result<void> IFileSystem::write_all(const Path &a_path, std::span<const std::byte> a_data, bool a_createParents)` |
| [Engine/Source/Platform/Windows/Private/WindowsDebugLogSink.cpp:49](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsDebugLogSink.cpp:49) | `OutputDebugStringW(wide.c_str());` |
| [Engine/Source/Platform/Windows/Private/Diagnostics.cpp:39](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/Diagnostics.cpp:39) | `OutputDebugStringW(wide);` |
| [Engine/Source/Platform/Windows/Private/Diagnostics.cpp:158](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/Diagnostics.cpp:158) | `void report_log(const char *a_source, const char *a_message, LogLevel a_level, std::source_location a_location) noexcept` |
| [Engine/Source/Platform/Windows/Private/Diagnostics.cpp:168](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/Diagnostics.cpp:168) | `void report_log_error(const char *a_source, const Error &a_error, LogLevel a_level,` |
| [Engine/Source/Platform/Windows/Private/Diagnostics.cpp:179](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/Diagnostics.cpp:179) | `void report_error(const char *a_source, const Error &a_error, DiagnosticSeverity a_severity,` |
| [Engine/Source/Platform/Windows/Private/Diagnostics.cpp:182](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/Diagnostics.cpp:182) | `report_log_error(a_source, a_error, convert_level(a_severity), a_location);` |
| [Engine/Source/Platform/Windows/Private/Diagnostics.cpp:188](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/Diagnostics.cpp:188) | `report_log(a_source, a_message, convert_level(a_severity), a_location);` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:130](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:130) | `report_log("WindowsFile", "CloseHandle failed", LogLevel::Error);` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:142](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:142) | `if (!ReadFile(m_handle, a_destination.data(), size, &count, nullptr))` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:157](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:157) | `if (!WriteFile(m_handle, a_source.data(), size, &count, nullptr))` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:207](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:207) | `if (!GetFileSizeEx(m_handle, &size))` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:220](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:220) | `return FlushFileBuffers(m_handle) ? Result<void>::success()` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:287](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:287) | `if (!GetFileAttributesExW(path.try_value()->c_str(), GetFileExInfoStandard, &data))` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:332](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:332) | `if (CreateDirectoryW(prefix.c_str(), nullptr))` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:337](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:337) | `const auto attributes = GetFileAttributesW(prefix.c_str());` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:369](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:369) | `const auto handle = FindFirstFileW(pattern.c_str(), &data);` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:404](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:404) | `} while (FindNextFileW(handle, &data));` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:418](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:418) | `const auto attributes = GetFileAttributesW(text.c_str());` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:426](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:426) | `attributes & FILE_ATTRIBUTE_DIRECTORY ? RemoveDirectoryW(text.c_str()) : DeleteFileW(text.c_str());` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:444](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:444) | `return MoveFileExW(from.try_value()->c_str(), to.try_value()->c_str(), 0)` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:457](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:457) | `return CopyFileW(from.try_value()->c_str(), to.try_value()->c_str(), !a_overwrite)` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:516](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:516) | `CreateFileW(path.try_value()->c_str(), access, share, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:530](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:530) | `Result<FileSaveOutcome> replace_file(const Path &a_path, std::span<const std::byte> a_data) override` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:579](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:579) | `if (!isPublished && !DeleteFileW(path.c_str()))` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:581](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:581) | `report_log("FileSystem.replace_file", "temporary cleanup failed", LogLevel::Error);` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:595](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:595) | `if (!DeleteFileW(nativeTemporary.try_value()->c_str()))` |
| [Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:626](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Windows/Private/WindowsFileSystem.cpp:626) | `if (!MoveFileExW(nativeTemporary.try_value()->c_str(), target.try_value()->c_str(),` |
| [Engine/Source/Platform/Public/Platform/Diagnostics.h:42](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Public/Platform/Diagnostics.h:42) | `void report_log(const char *a_source, const char *a_message, LogLevel a_level = LogLevel::Info,` |
| [Engine/Source/Platform/Public/Platform/Diagnostics.h:46](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Public/Platform/Diagnostics.h:46) | `void report_log_error(const char *a_source, const Error &a_error, LogLevel a_level,` |
| [Engine/Source/Platform/Public/Platform/Diagnostics.h:57](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Public/Platform/Diagnostics.h:57) | `void report_error(const char *a_source, const Error &a_error, DiagnosticSeverity a_severity,` |
| [Engine/Source/Platform/Public/Platform/FileSystem.h:134](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Public/Platform/FileSystem.h:134) | `[[nodiscard]] Result<std::vector<std::byte>> read_all(const Path &a_path,` |
| [Engine/Source/Platform/Public/Platform/FileSystem.h:137](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Public/Platform/FileSystem.h:137) | `[[nodiscard]] Result<void> write_all(const Path &a_path, std::span<const std::byte> a_data,` |
| [Engine/Source/Platform/Public/Platform/FileSystem.h:143](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Platform/Public/Platform/FileSystem.h:143) | `[[nodiscard]] virtual Result<FileSaveOutcome> replace_file(const Path &a_path,` |
| [Engine/Source/WindowsHost/Private/WindowsHost.cpp:389](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:389) | `report_log_error("CueWindowsHost cleanup", *result.try_error(), LogLevel::Error);` |
| [Engine/Source/WindowsHost/Private/WindowsHost.cpp:430](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:430) | `report_log_error("WindowsHost.initialize", a_error, LogLevel::Error);` |
| [Engine/Source/WindowsHost/Private/WindowsHost.cpp:434](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:434) | `report_log_error("CueWindowsHost cleanup", *cleanupResult.try_error(), LogLevel::Error);` |
| [Engine/Source/WindowsHost/Private/WindowsHost.cpp:500](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:500) | `report_log_error("WindowsHost.performance_settings.load", *valid.try_error(), LogLevel::Warning);` |
| [Engine/Source/WindowsHost/Private/WindowsHost.cpp:504](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:504) | `report_log("WindowsHost", "initializing");` |
| [Engine/Source/WindowsHost/Private/WindowsHost.cpp:726](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:726) | `report_log("WindowsHost", "initialized");` |
| [Engine/Source/WindowsHost/Private/WindowsHost.cpp:1153](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:1153) | `report_log("WindowsHost", "performance settings applied");` |
| [Engine/Source/WindowsHost/Private/WindowsHost.cpp:1319](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:1319) | `report_log_error("WindowsHost.shutdown", failure ? *failure : *hostResult.try_error(), LogLevel::Error);` |
| [Engine/Source/WindowsHost/Private/WindowsHost.cpp:1375](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:1375) | `report_log_error("WindowsHost.shutdown", *failure, LogLevel::Error);` |
| [Engine/Source/WindowsHost/Private/WindowsHost.cpp:1379](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:1379) | `report_log("WindowsHost", "stopped");` |
| [Engine/Source/WindowsHost/Private/WindowsHost.cpp:1388](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/WindowsHost.cpp:1388) | `report_log_error("WindowsHost.logging.shutdown", *stopped.try_error(), LogLevel::Error);` |
| [Engine/Source/WindowsHost/Private/PerformanceSettings.cpp:137](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/PerformanceSettings.cpp:137) | `auto saved = a_files.replace_file(a_path, std::as_bytes(std::span(text.data(), text.size())));` |
| [Engine/Source/WindowsHost/Private/PerformanceSettings.cpp:149](C:/Users/sinse/source/repos/CueEngine/Engine/Source/WindowsHost/Private/PerformanceSettings.cpp:149) | `auto bytes = a_files.read_all(a_path, 4096);` |
| [Engine/Source/Renderer/DX12/Private/DX12Backend.cpp:150](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12Backend.cpp:150) | `report_log_error("DX12Backend.shutdown", *result.try_error(), LogLevel::Error);` |
| [Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:222](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:222) | `report_log_error("DX12GpuCommandContext", command_error("ID3D12CommandAllocator.SetName", allocatorNameResult),` |
| [Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:229](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:229) | `report_log_error("DX12GpuCommandContext", command_error("ID3D12GraphicsCommandList.SetName", listNameResult),` |
| [Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:354](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:354) | `report_log_error("DX12CommandPool.State", *result.try_error(),` |
| [Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:440](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:440) | `report_log_error("DX12CommandPool.shutdown", *result.try_error(), LogLevel::Error);` |
| [Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:519](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:519) | `report_log_error("DX12CommandPool.acquire", *resetResult.try_error(), LogLevel::Error);` |
| [Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:618](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:618) | `report_log_error("DX12CommandPool.release", *closeResult.try_error(),` |
| [Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:696](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12CommandPool.cpp:696) | `report_log_error("DX12CommandPool.submit", *idleResult.try_error(), LogLevel::Fatal);` |
| [Engine/Source/Renderer/DX12/Private/DX12DescriptorAllocator.cpp:103](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12DescriptorAllocator.cpp:103) | `report_log_error("DX12DescriptorAllocator", descriptor_error("ID3D12DescriptorHeap.SetName", nameResult),` |
| [Engine/Source/Renderer/DX12/Private/DX12FrameGraphExecutor.cpp:443](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12FrameGraphExecutor.cpp:443) | `PIXBeginEvent(&list, PIX_COLOR_DEFAULT, "%s", name.c_str());` |
| [Engine/Source/Renderer/DX12/Private/DX12FrameGraphExecutor.cpp:445](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12FrameGraphExecutor.cpp:445) | `PIXEndEvent(&list);` |
| [Engine/Source/Renderer/DX12/Private/DX12FrameGraphFrames.cpp:255](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12FrameGraphFrames.cpp:255) | `report_log_error("DX12FrameGraphFrames.shutdown", *result.try_error(), LogLevel::Fatal);` |
| [Engine/Source/Renderer/DX12/Private/DX12FrameGraphResources.cpp:141](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12FrameGraphResources.cpp:141) | `report_log_error("DX12FrameGraphResources.shutdown", *result.try_error(), LogLevel::Fatal);` |
| [Engine/Source/Renderer/DX12/Private/DX12GpuResource.cpp:45](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12GpuResource.cpp:45) | `report_log_error("DX12GpuResource", resource_error("ID3D12Resource.SetName", result),` |
| [Engine/Source/Renderer/DX12/Private/DX12GpuResourcePool.cpp:73](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12GpuResourcePool.cpp:73) | `report_log_error("DX12GpuResourcePool", *result.try_error(), LogLevel::Fatal);` |
| [Engine/Source/Renderer/DX12/Private/DX12GpuResourcePool.cpp:217](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12GpuResourcePool.cpp:217) | `report_log_error("DX12GpuResourcePool.shutdown", *result.try_error(), LogLevel::Error);` |
| [Engine/Source/Renderer/DX12/Private/DX12MainFrameGraph.cpp:131](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12MainFrameGraph.cpp:131) | `report_log_error("DX12MainFrameGraph.shutdown", *result.try_error(), LogLevel::Fatal);` |
| [Engine/Source/Renderer/DX12/Private/DX12PlacedResourceAllocator.cpp:432](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12PlacedResourceAllocator.cpp:432) | `report_log_error("DX12PlacedResourceAllocator",` |
| [Engine/Source/Renderer/DX12/Private/DX12PlacedResourceAllocator.cpp:478](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12PlacedResourceAllocator.cpp:478) | `report_log_error("DX12PlacedResourceAllocator",` |
| [Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:101](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:101) | `report_log_error("DX12RenderDevice", gpu_error("IDXGIFactory.SetPrivateData", result),` |
| [Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:167](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:167) | `report_log_error("DX12RenderDevice", gpu_error("IDXGIAdapter.SetPrivateData", result),` |
| [Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:173](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:173) | `report_log_error("DX12RenderDevice", gpu_error("ID3D12Device.SetName", result),` |
| [Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:189](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:189) | `result = infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);` |
| [Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:194](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:194) | `result = infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);` |
| [Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:199](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12RenderDevice.cpp:199) | `result = infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);` |
| [Engine/Source/Renderer/DX12/Private/DX12QueuePool.cpp:119](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12QueuePool.cpp:119) | `report_log_error("DX12GpuCommandQueue", queue_error("ID3D12CommandQueue.SetName", queueNameResult),` |
| [Engine/Source/Renderer/DX12/Private/DX12QueuePool.cpp:126](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12QueuePool.cpp:126) | `report_log_error("DX12GpuCommandQueue", queue_error("ID3D12Fence.SetName", fenceNameResult),` |
| [Engine/Source/Renderer/DX12/Private/DX12QueuePool.cpp:539](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12QueuePool.cpp:539) | `report_log_error("DX12QueuePool.State", *result.try_error(), LogLevel::Error);` |
| [Engine/Source/Renderer/DX12/Private/DX12QueuePool.cpp:588](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12QueuePool.cpp:588) | `report_log_error("DX12QueuePool.shutdown", *result.try_error(), LogLevel::Error);` |
| [Engine/Source/Renderer/DX12/Private/DX12ResourceLeakChecker.cpp:24](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12ResourceLeakChecker.cpp:24) | `report_log("DX12ResourceLeakChecker", "DXGI Debug interface is unavailable; live object report was skipped",` |
| [Engine/Source/Renderer/DX12/Private/DX12ResourceLeakChecker.cpp:30](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12ResourceLeakChecker.cpp:30) | `const HRESULT reportResult = debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);` |
| [Engine/Source/Renderer/DX12/Private/DX12ResourceLeakChecker.cpp:33](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12ResourceLeakChecker.cpp:33) | `report_log_error("DX12ResourceLeakChecker",` |
| [Engine/Source/Renderer/DX12/Private/DX12ShaderCompiler.cpp:77](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12ShaderCompiler.cpp:77) | `auto bytes = m_files->read_all(*path.try_value());` |
| [Engine/Source/Renderer/DX12/Private/DX12ShaderCompiler.cpp:188](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12ShaderCompiler.cpp:188) | `state.module = LoadLibraryExW(libraryPath.try_value()->c_str(), nullptr,` |
| [Engine/Source/Renderer/DX12/Private/DX12ShaderCompiler.cpp:293](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12ShaderCompiler.cpp:293) | `auto source = m_state->files->read_all(*path.try_value());` |
| [Engine/Source/Renderer/DX12/Private/DX12SwapChain.cpp:139](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12SwapChain.cpp:139) | `report_log_error("DX12SwapChain", swap_chain_error("IDXGISwapChain.SetPrivateData", nameResult),` |
| [Engine/Source/Renderer/DX12/Private/DX12SwapChain.cpp:163](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12SwapChain.cpp:163) | `report_log_error("DX12SwapChain.shutdown", *result.try_error(), LogLevel::Fatal);` |
| [Engine/Source/Renderer/DX12/Private/DX12SwapChain.cpp:450](C:/Users/sinse/source/repos/CueEngine/Engine/Source/Renderer/DX12/Private/DX12SwapChain.cpp:450) | `report_log_error("DX12SwapChain", swap_chain_error("ID3D12Resource.SetName", nameResult),` |

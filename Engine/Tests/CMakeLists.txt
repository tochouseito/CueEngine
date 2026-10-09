if(MSVC)
    # Test Source の日本語コメントを UTF-8 として扱う
    add_compile_options(/utf-8)
endif()

# Foundation と公開 Header は Platform 実装なしで単独検証する
add_executable(CueFoundationResultTests Foundation/ResultTests.cpp)
target_link_libraries(CueFoundationResultTests PRIVATE Cue.Foundation)
add_test(NAME Cue.Foundation.Result COMMAND CueFoundationResultTests)

add_executable(CueFoundationPathTests Foundation/PathTests.cpp)
target_link_libraries(CueFoundationPathTests PRIVATE Cue.Foundation)
add_test(NAME Cue.Foundation.Path COMMAND CueFoundationPathTests)

# 計測分布の p95、固定容量、Overflow と入れ子 Guard を確認する
add_executable(CueFoundationTimingTests Foundation/TimingSamplesTests.cpp)
target_link_libraries(CueFoundationTimingTests PRIVATE Cue.Foundation)
add_test(NAME Cue.Foundation.TimingSamples COMMAND CueFoundationTimingTests)

add_executable(CueLoggingHeaderCompile Logging/PublicHeaderCompile.cpp)
target_link_libraries(CueLoggingHeaderCompile PRIVATE Cue.Logging)
add_test(NAME Cue.Logging.PublicHeaders COMMAND CueLoggingHeaderCompile)

if(WIN32)
    add_executable(CueLoggerTests Logging/LoggerTests.cpp)
    target_link_libraries(CueLoggerTests PRIVATE Cue.Logging Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.Logging.Logger COMMAND CueLoggerTests)
    set_tests_properties(Cue.Logging.Logger PROPERTIES TIMEOUT 15)
    add_executable(CueDiagnosticRoutingTests Logging/DiagnosticRoutingTests.cpp)
    target_link_libraries(CueDiagnosticRoutingTests PRIVATE Cue.Logging Cue.Platform.Windows)
    add_test(NAME Cue.Logging.DiagnosticRouting COMMAND CueDiagnosticRoutingTests)
    set_tests_properties(Cue.Logging.DiagnosticRouting PROPERTIES TIMEOUT 15)
    add_executable(CueHostLoggingTests Logging/HostLoggingTests.cpp)
    target_link_libraries(CueHostLoggingTests PRIVATE Cue.EditorHost Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.Logging.Host COMMAND CueHostLoggingTests)
    set_tests_properties(Cue.Logging.Host PROPERTIES TIMEOUT 30)
endif()

add_executable(CuePlatformHeaderCompile Platform/PublicHeaderCompile.cpp)
target_link_libraries(CuePlatformHeaderCompile PRIVATE Cue.Platform)
add_test(NAME Cue.Platform.PublicHeaders COMMAND CuePlatformHeaderCompile)

# Queue の公開契約を DX12／Win32 の実装 Header なしで Compile する
add_executable(CueRHIQueueHeaderCompile Renderer/RHIQueueHeaderCompile.cpp)
target_link_libraries(CueRHIQueueHeaderCompile PRIVATE Cue.Renderer.RHI)
add_test(NAME Cue.Renderer.RHI.QueueHeader COMMAND CueRHIQueueHeaderCompile)

# Pass の依存関係と論理 Resource 寿命を Backend なしで検証する
add_executable(CueFrameGraphBuilderTests Renderer/FrameGraphBuilderTests.cpp)
target_link_libraries(CueFrameGraphBuilderTests PRIVATE Cue.Renderer.FrameGraph)
add_test(NAME Cue.Renderer.FrameGraph.Builder COMMAND CueFrameGraphBuilderTests)

    # 旧 Pass の setup、名前付き Resource 参照、依存順を確認する
    add_executable(CueFrameGraphTests Renderer/FrameGraphTests.cpp)
    target_link_libraries(CueFrameGraphTests PRIVATE Cue.Renderer.Passes)
    add_test(NAME Cue.Renderer.FrameGraph.Passes COMMAND CueFrameGraphTests)

# Build の Pipeline 生成・設定伝達・失敗回収を Backend なしで検証する
add_executable(CueFrameGraphPipelineTests Renderer/FrameGraphPipelineTests.cpp)
target_link_libraries(CueFrameGraphPipelineTests PRIVATE Cue.Renderer.Passes)
add_test(NAME Cue.Renderer.FrameGraph.Pipelines COMMAND CueFrameGraphPipelineTests)

if(WIN32)
    add_executable(CueFileSystemTests Platform/FileSystemTests.cpp)
    target_link_libraries(CueFileSystemTests PRIVATE Cue.Platform.Windows Cue.WindowsHost Cue.Foundation.Windows)
    add_test(NAME Cue.Platform.Windows.FileSystem COMMAND CueFileSystemTests)
    set_tests_properties(Cue.Platform.Windows.FileSystem PROPERTIES TIMEOUT 15)
    # 共通 Factory から DX12 Backend を生成し、Device の所有と停止を検証する
    add_executable(CueDX12BackendTests Renderer/DX12BackendTests.cpp)
    target_link_libraries(CueDX12BackendTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.Backend COMMAND CueDX12BackendTests)
    set_tests_properties(Cue.Renderer.DX12.Backend PROPERTIES TIMEOUT 15)

    # Device 作成を WindowHost と独立して実 Adapter または WARP で検証する
    add_executable(CueDX12RenderDeviceTests Renderer/DX12RenderDeviceTests.cpp)
    target_link_libraries(CueDX12RenderDeviceTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.RenderDevice COMMAND CueDX12RenderDeviceTests)
    set_tests_properties(Cue.Renderer.DX12.RenderDevice PROPERTIES TIMEOUT 15)

    # WARP の実 Window で Back Buffer、RTV、VSync とティアリング設定を検証する
    add_executable(CueDX12SwapChainTests Renderer/DX12SwapChainTests.cpp)
    target_link_libraries(CueDX12SwapChainTests PRIVATE Cue.Renderer.DX12 Cue.Platform.Windows)
    add_test(NAME Cue.Renderer.DX12.SwapChain COMMAND CueDX12SwapChainTests)
    set_tests_properties(Cue.Renderer.DX12.SwapChain PROPERTIES TIMEOUT 20)

    # 実 DXC と WARP で Pipeline の失敗、Handle と Manager 破棄後の GPU 利用を確認する
    add_executable(CueDX12PipelineManagerTests Renderer/DX12PipelineManagerTests.cpp)
    target_link_libraries(CueDX12PipelineManagerTests PRIVATE Cue.Renderer.DX12 Cue.Platform.Windows Cue.Foundation.Windows)
    target_compile_definitions(CueDX12PipelineManagerTests PRIVATE
        CUE_TEST_SHADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/Renderer/Shaders/PipelineValidation.hlsl")
    add_test(NAME Cue.Renderer.DX12.PipelineManager COMMAND CueDX12PipelineManagerTests)
    set_tests_properties(Cue.Renderer.DX12.PipelineManager PROPERTIES TIMEOUT 30)

    # Descriptor Heap の生成、世代付き再利用と並行貸出を WARP で検証する
    add_executable(CueDX12DescriptorAllocatorTests Renderer/DX12DescriptorAllocatorTests.cpp)
    target_link_libraries(CueDX12DescriptorAllocatorTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.DescriptorAllocator COMMAND CueDX12DescriptorAllocatorTests)
    set_tests_properties(Cue.Renderer.DX12.DescriptorAllocator PROPERTIES TIMEOUT 15)

    # View の Device 所属、Mip 範囲、容量と異常系の Slot 保全を WARP で確認する
    add_executable(CueDX12ViewManagerTests Renderer/DX12ViewManagerTests.cpp)
    target_link_libraries(CueDX12ViewManagerTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.ViewManager COMMAND CueDX12ViewManagerTests)
    set_tests_properties(Cue.Renderer.DX12.ViewManager PROPERTIES TIMEOUT 15)

    # 複数 Queue の貸出、GPU Fence 同期、返却後の再利用を WARP で検証する
    add_executable(CueDX12QueuePoolTests Renderer/DX12QueuePoolTests.cpp)
    target_link_libraries(CueDX12QueuePoolTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.QueuePool COMMAND CueDX12QueuePoolTests)
    set_tests_properties(Cue.Renderer.DX12.QueuePool PROPERTIES TIMEOUT 15)

    # Command Context の提出と Fence 完了後再利用を WARP の実コピーで確認する
    add_executable(CueDX12CommandPoolTests Renderer/DX12CommandPoolTests.cpp)
    target_link_libraries(CueDX12CommandPoolTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.CommandPool COMMAND CueDX12CommandPoolTests)
    set_tests_properties(Cue.Renderer.DX12.CommandPool PROPERTIES TIMEOUT 15)

    # Resource の Heap 用途、CPU 範囲検証と GPU 転送を WARP で確認する
    add_executable(CueDX12GpuResourceTests Renderer/DX12GpuResourceTests.cpp)
    target_link_libraries(CueDX12GpuResourceTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.GpuResource COMMAND CueDX12GpuResourceTests)
    set_tests_properties(Cue.Renderer.DX12.GpuResource PROPERTIES TIMEOUT 15)

    # ResourcePool の世代、Access 競合、遅延破棄と WARP 実コピーを検証する
    add_executable(CueDX12GpuResourcePoolTests Renderer/DX12GpuResourcePoolTests.cpp)
    target_link_libraries(CueDX12GpuResourcePoolTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.GpuResourcePool COMMAND CueDX12GpuResourcePoolTests)
    set_tests_properties(Cue.Renderer.DX12.GpuResourcePool PROPERTIES TIMEOUT 15)

    # Placed Heap の Alignment、非重複、再利用と WARP 実コピーを確認する
    add_executable(CueDX12PlacedResourceAllocatorTests Renderer/DX12PlacedResourceAllocatorTests.cpp)
    target_link_libraries(CueDX12PlacedResourceAllocatorTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.PlacedResourceAllocator COMMAND CueDX12PlacedResourceAllocatorTests)
    set_tests_properties(Cue.Renderer.DX12.PlacedResourceAllocator PROPERTIES TIMEOUT 15)

    # Graph の寿命から同じ Heap 領域を共有し、Activation と GPU Copy を WARP で確認する
    add_executable(CueDX12FrameGraphResourcesTests Renderer/DX12FrameGraphResourcesTests.cpp)
    target_link_libraries(CueDX12FrameGraphResourcesTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.FrameGraphResources COMMAND CueDX12FrameGraphResourcesTests)
    set_tests_properties(Cue.Renderer.DX12.FrameGraphResources PROPERTIES TIMEOUT 15)

    # Graph の一時 Texture を枠ごとに物理化し、WARP の Clear 結果を読み戻す
    add_executable(CueDX12FrameGraphFramesTests Renderer/DX12FrameGraphFramesTests.cpp)
    target_link_libraries(CueDX12FrameGraphFramesTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.FrameGraphFrames COMMAND CueDX12FrameGraphFramesTests)
    set_tests_properties(Cue.Renderer.DX12.FrameGraphFrames PROPERTIES TIMEOUT 15)

    # 旧 Pass 契約による Clear と表示の Barrier および WARP Back Buffer 画素を確認する
    add_executable(CueDX12MainFrameGraphTests Renderer/DX12MainFrameGraphTests.cpp)
    target_link_libraries(CueDX12MainFrameGraphTests PRIVATE Cue.Renderer.DX12 Cue.Platform.Windows)
    target_compile_definitions(CueDX12MainFrameGraphTests PRIVATE
        CUE_TEST_SHADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/Renderer/Shaders/PipelineValidation.hlsl")
    add_test(NAME Cue.Renderer.DX12.MainFrameGraph COMMAND CueDX12MainFrameGraphTests)
    set_tests_properties(Cue.Renderer.DX12.MainFrameGraph PROPERTIES TIMEOUT 20)

    # 論理 Barrier と Pass を実 Command List に記録し、WARP の転送結果を確認する
    add_executable(CueDX12FrameGraphExecutorTests Renderer/DX12FrameGraphExecutorTests.cpp)
    target_link_libraries(CueDX12FrameGraphExecutorTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.FrameGraphExecutor COMMAND CueDX12FrameGraphExecutorTests)
    set_tests_properties(Cue.Renderer.DX12.FrameGraphExecutor PROPERTIES TIMEOUT 15)

    # Runtime と Host は実際の Windows Service と Window で検証する
    add_executable(CueRuntimeTests Runtime/RuntimeTests.cpp)
    target_link_libraries(CueRuntimeTests PRIVATE Cue.Runtime Cue.Platform.Windows)
    add_test(NAME Cue.Runtime.Windowless COMMAND CueRuntimeTests)

    add_executable(CueWindowsHostTests Platform/WindowsHostTests.cpp)
    target_link_libraries(CueWindowsHostTests PRIVATE Cue.WindowsHost Cue.Platform.Windows)
    add_test(NAME Cue.WindowsHost.Lifecycle COMMAND CueWindowsHostTests)
    set_tests_properties(Cue.WindowsHost.Lifecycle PROPERTIES TIMEOUT 10)

    add_executable(CueFrameControllerTests Runtime/FrameControllerTests.cpp)
    target_link_libraries(CueFrameControllerTests PRIVATE Cue.Runtime.Frame Cue.Platform.Windows)
    add_test(NAME Cue.Runtime.FrameController COMMAND CueFrameControllerTests)
    set_tests_properties(Cue.Runtime.FrameController PROPERTIES TIMEOUT 5)

    # 時間計測は環境依存のため CTest の速度判定に使わず、手動の比較用 Target とする
    add_executable(CueFrameControllerTimingProbe Performance/FrameControllerTimingProbe.cpp)
    target_link_libraries(CueFrameControllerTimingProbe PRIVATE Cue.Runtime.Frame Cue.Platform.Windows user32)

    add_executable(CueWindowsThreadServicesTests Platform/WindowsThreadServicesTests.cpp)
    target_link_libraries(CueWindowsThreadServicesTests PRIVATE Cue.Platform.Windows)
    add_test(NAME Cue.Platform.Windows.Threading COMMAND CueWindowsThreadServicesTests)
    set_tests_properties(Cue.Platform.Windows.Threading PROPERTIES TIMEOUT 5)

    add_executable(CueWindowsUtfConversionTests Foundation/WindowsUtfConversionTests.cpp)
    target_link_libraries(CueWindowsUtfConversionTests PRIVATE Cue.Foundation.Windows)
    add_test(NAME Cue.Foundation.Windows.UtfConversion COMMAND CueWindowsUtfConversionTests)

    add_executable(CueWindowsWindowLifecycleTests Platform/WindowsWindowLifecycleTests.cpp)
    target_link_libraries(CueWindowsWindowLifecycleTests PRIVATE Cue.Platform.Windows user32)
    add_test(NAME Cue.Platform.Windows.Lifecycle COMMAND CueWindowsWindowLifecycleTests)

    # 製品 Window の表示、状態変更、Close を別 Process から確認する
    add_executable(CueWindowsHostProcessTests Platform/WindowsHostProcessTests.cpp)
    target_link_libraries(CueWindowsHostProcessTests PRIVATE user32)
    add_test(NAME Cue.WindowsHost.Process COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueWindowsHost>)
    add_test(NAME Cue.WindowsHost.ResizeMinimizeRestore
             COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueWindowsHost> --resize)
    set_tests_properties(Cue.WindowsHost.Process PROPERTIES TIMEOUT 7)
    set_tests_properties(Cue.WindowsHost.ResizeMinimizeRestore PROPERTIES TIMEOUT 20)

    # Docking API と公式 Backend の構成別 Library が利用できることを確認する
    add_executable(CueImGuiDependencyTests Editor/ImGuiDependencyTests.cpp)
    target_link_libraries(CueImGuiDependencyTests PRIVATE Cue.EditorHost imgui::imgui)
    add_test(NAME Cue.Editor.ImGuiDependency COMMAND CueImGuiDependencyTests)
    set_tests_properties(Cue.Editor.ImGuiDependency PROPERTIES TIMEOUT 5)

    # Handler の消費 / 例外が Window Lifecycle を壊さず、所有 Token で解除できることを確認する
    add_executable(CueWindowsMessageHandlerTests Platform/WindowsMessageHandlerTests.cpp)
    target_link_libraries(CueWindowsMessageHandlerTests PRIVATE Cue.Platform.Windows)
    add_test(NAME Cue.Platform.Windows.MessageHandler COMMAND CueWindowsMessageHandlerTests)
    set_tests_properties(Cue.Platform.Windows.MessageHandler PROPERTIES TIMEOUT 10)

    # Context、CPU 描画 Data、入力接続、Layout 保存と失敗回収を GPU Backend なしで検証する
    add_executable(CueImGuiManagerTests Editor/ImGuiManagerTests.cpp)
    target_link_libraries(CueImGuiManagerTests PRIVATE Cue.EditorHost Cue.Platform.Windows imgui::imgui)
    add_test(NAME Cue.Editor.ImGuiManager COMMAND CueImGuiManagerTests)
    set_tests_properties(Cue.Editor.ImGuiManager PROPERTIES TIMEOUT 15)

    # 公式 DX12 の実画素、動的 Texture、容量不足回復と終了後の GPU Object を検証する
    add_executable(CueImGuiDX12BackendTests Editor/ImGuiDX12BackendTests.cpp)
    target_link_libraries(CueImGuiDX12BackendTests PRIVATE Cue.EditorHost Cue.Renderer.DX12 Cue.Platform.Windows imgui::imgui)
    add_test(NAME Cue.Editor.ImGuiDX12Backend COMMAND CueImGuiDX12BackendTests)
    add_test(NAME Cue.Editor.ImGuiDX12Backend.Warp COMMAND CueImGuiDX12BackendTests --warp)
    set_tests_properties(Cue.Editor.ImGuiDX12Backend Cue.Editor.ImGuiDX12Backend.Warp PROPERTIES TIMEOUT 30)

    # Editor が所有する Pass の宣言、失敗伝播と Adapter 寿命を GPU なしで検証する
    add_executable(CueImGuiPassTests Editor/ImGuiPassTests.cpp)
    target_link_libraries(CueImGuiPassTests PRIVATE Cue.EditorHost)
    add_test(NAME Cue.Editor.ImGuiPass COMMAND CueImGuiPassTests)

    # Editor の既定 Thread 構成と失敗時停止を実 Window で確認する
    add_executable(CueEditorHostTests Editor/EditorHostTests.cpp)
    target_link_libraries(CueEditorHostTests PRIVATE Cue.EditorHost)
    add_test(NAME Cue.EditorHost.Lifecycle COMMAND CueEditorHostTests)
    set_tests_properties(Cue.EditorHost.Lifecycle PROPERTIES TIMEOUT 30)

    # 共通の Process Fixture で Editor の Executable の表示と正常 Close を確認する
    add_dependencies(CueWindowsHostProcessTests CueEditorHost)
    add_test(NAME Cue.EditorHost.Process COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueEditorHost> --sustain)
    set_tests_properties(Cue.EditorHost.Process PROPERTIES TIMEOUT 15)
    # GPU Resize の成否と区別し、Worker 表示中の Window 状態変更と終了応答を確認する
    add_test(NAME Cue.EditorHost.ResizeMinimizeRestore
             COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueEditorHost> --resize)
    set_tests_properties(Cue.EditorHost.ResizeMinimizeRestore PROPERTIES TIMEOUT 20)
endif()

if(WIN32)
    cue_copy_shader_runtime(CueDX12PipelineManagerTests
        CueDX12BackendTests CueDX12MainFrameGraphTests CueDX12FrameGraphFramesTests CueDX12SwapChainTests
        CueEditorHostTests CueImGuiDX12BackendTests
    )
endif()

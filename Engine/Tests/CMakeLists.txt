if(MSVC)
    # Test Source の日本語コメントを UTF-8 として扱う
    add_compile_options(/utf-8)
endif()

# Foundation と公開 Header は Platform 実装なしで単独検証する
find_package(Threads REQUIRED)

# Camera の数値契約と Scene Component を Native API なしで検証する
add_executable(CueProjectionTests Math/ProjectionTests.cpp)
target_link_libraries(CueProjectionTests PRIVATE Cue.Math)
add_test(NAME Cue.Math.Projection COMMAND CueProjectionTests)
add_executable(CueCameraTests Runtime/CameraTests.cpp)
target_link_libraries(CueCameraTests PRIVATE Cue.Runtime)
add_test(NAME Cue.Runtime.Camera COMMAND CueCameraTests)
add_executable(CueSceneRenderComponentsTests Scene/SceneRenderComponentsTests.cpp)
target_link_libraries(CueSceneRenderComponentsTests PRIVATE Cue.SceneRuntime)
add_test(NAME Cue.Scene.RenderComponents COMMAND CueSceneRenderComponentsTests)
add_executable(CueRenderSceneTests Renderer/RenderSceneTests.cpp)
target_link_libraries(CueRenderSceneTests PRIVATE Cue.Renderer.SceneRender Cue.SceneRuntime Threads::Threads)
add_test(NAME Cue.Renderer.SceneExtraction COMMAND CueRenderSceneTests)
foreach(taskSceneRenderHeader RenderScene SceneRenderer)
    set(taskSceneRenderHeaderSource "${CMAKE_CURRENT_BINARY_DIR}/SceneRenderHeaders/${taskSceneRenderHeader}.cpp")
    file(GENERATE OUTPUT "${taskSceneRenderHeaderSource}" CONTENT "#include <SceneRender/${taskSceneRenderHeader}.h>\n")
    target_sources(CueRenderSceneTests PRIVATE "${taskSceneRenderHeaderSource}")
endforeach()

# CPU Geometry と Cube は Platform / Renderer なしで検証する
add_executable(CueMeshDataTests Assets/MeshDataTests.cpp)
target_link_libraries(CueMeshDataTests PRIVATE Cue.Assets)
add_test(NAME Cue.Assets.MeshData COMMAND CueMeshDataTests)
add_executable(CueMeshHeaderCompile Assets/PublicHeaderCompile.cpp)
target_link_libraries(CueMeshHeaderCompile PRIVATE Cue.Assets)
add_test(NAME Cue.Assets.PublicHeaders COMMAND CueMeshHeaderCompile)
foreach(taskAssetHeader MeshData BuiltinMesh)
    set(taskAssetHeaderSource "${CMAKE_CURRENT_BINARY_DIR}/AssetHeaders/${taskAssetHeader}.cpp")
    file(GENERATE OUTPUT "${taskAssetHeaderSource}" CONTENT "#include <Assets/${taskAssetHeader}.h>\n")
    target_sources(CueMeshHeaderCompile PRIVATE "${taskAssetHeaderSource}")
endforeach()

# Scene 定義は Runtime 非依存、実体化は別の接続 Target で検証する
add_executable(CueSceneTests Scene/SceneTests.cpp)
target_link_libraries(CueSceneTests PRIVATE Cue.Scene)
add_test(NAME Cue.Scene.Core COMMAND CueSceneTests)
add_executable(CueSceneHeaderCompile Scene/PublicHeaderCompile.cpp)
target_link_libraries(CueSceneHeaderCompile PRIVATE Cue.Scene)
add_test(NAME Cue.Scene.PublicHeaders COMMAND CueSceneHeaderCompile)
add_executable(CueSceneInstantiationTests Scene/SceneInstantiationTests.cpp)
target_link_libraries(CueSceneInstantiationTests PRIVATE Cue.SceneRuntime Threads::Threads)
add_test(NAME Cue.Scene.Instantiation COMMAND CueSceneInstantiationTests)
add_executable(CueSceneCapacityTests Scene/SceneCapacityTests.cpp)
target_link_libraries(CueSceneCapacityTests PRIVATE Cue.SceneRuntime)
add_test(NAME Cue.Scene.InstantiationCapacity COMMAND CueSceneCapacityTests)

# World の Instance・Handle・System・Transform を Window / GPU なしで検証する
add_executable(CueWorldTests Runtime/WorldTests.cpp)
target_link_libraries(CueWorldTests PRIVATE Cue.Runtime Threads::Threads)
add_test(NAME Cue.Runtime.World COMMAND CueWorldTests)
set_tests_properties(Cue.Runtime.World PROPERTIES TIMEOUT 20)
add_executable(CueWorldHeaderCompile Runtime/WorldHeaderCompile.cpp)
target_link_libraries(CueWorldHeaderCompile PRIVATE Cue.Runtime)
add_test(NAME Cue.Runtime.WorldPublicHeaders COMMAND CueWorldHeaderCompile)
add_executable(CueWorldComponentCapacityTests Runtime/WorldComponentCapacityTests.cpp)
target_link_libraries(CueWorldComponentCapacityTests PRIVATE Cue.Runtime)
add_test(NAME Cue.Runtime.WorldComponentCapacity COMMAND CueWorldComponentCapacityTests)

# 数学型と各 Header は Native Backend なしで検証する
add_executable(CueMathTests Math/MathTests.cpp)
target_link_libraries(CueMathTests PRIVATE Cue.Math)
add_test(NAME Cue.Math.Core COMMAND CueMathTests)

add_executable(CueMathHeaderCompile Math/PublicHeaderCompile.cpp)
target_link_libraries(CueMathHeaderCompile PRIVATE Cue.Math)
add_test(NAME Cue.Math.PublicHeaders COMMAND CueMathHeaderCompile)
# 各公開 Header を別の翻訳単位にし、Include 順の偶然による不足を検出する
foreach(taskMathHeader Scalar VectorOperations Vector2 Vector3 Vector4 Quaternion Matrix4x4 Transform Projection Math)
    set(taskMathHeaderSource "${CMAKE_CURRENT_BINARY_DIR}/MathHeaders/${taskMathHeader}.cpp")
    file(GENERATE OUTPUT "${taskMathHeaderSource}" CONTENT "#include <Math/${taskMathHeader}.h>\n")
    target_sources(CueMathHeaderCompile PRIVATE "${taskMathHeaderSource}")
endforeach()

# 旧 ECS の Entity / Pool / Pipeline / Prototype を Native Backend なしで検証する
add_executable(CueECSHeaderCompile ECS/PublicHeaderCompile.cpp)
target_link_libraries(CueECSHeaderCompile PRIVATE Cue.ECS)
add_test(NAME Cue.ECS.PublicHeaders COMMAND CueECSHeaderCompile)
add_executable(CueECSTests ECS/ECSTests.cpp ECS/RegistryPeer.cpp ECS/RegressionTests.cpp)
target_link_libraries(CueECSTests PRIVATE Cue.ECS Threads::Threads)
add_test(NAME Cue.ECS.LegacyParity COMMAND CueECSTests)
set_tests_properties(Cue.ECS.LegacyParity PROPERTIES TIMEOUT 15)

add_executable(CueTimingTraceTests Foundation/TimingTraceTests.cpp)
target_link_libraries(CueTimingTraceTests PRIVATE Cue.Profiler Threads::Threads)
add_test(NAME Cue.Foundation.TimingTrace COMMAND CueTimingTraceTests)

add_executable(CueSpikeTriggerTests Performance/SpikeTriggerTests.cpp)
target_link_libraries(CueSpikeTriggerTests PRIVATE Cue.Profiler Threads::Threads)
add_test(NAME Cue.Performance.SpikeTrigger COMMAND CueSpikeTriggerTests)

add_executable(CueFoundationResultTests Foundation/ResultTests.cpp)
target_link_libraries(CueFoundationResultTests PRIVATE Cue.Foundation)
add_test(NAME Cue.Foundation.Result COMMAND CueFoundationResultTests)

add_executable(CueFoundationPathTests Foundation/PathTests.cpp)
target_link_libraries(CueFoundationPathTests PRIVATE Cue.Foundation)
add_test(NAME Cue.Foundation.Path COMMAND CueFoundationPathTests)

# Project の公開契約は Win32 / Editor / Renderer なしで Compile する
add_executable(CueProjectHeaderCompile Project/PublicHeaderCompile.cpp)
target_link_libraries(CueProjectHeaderCompile PRIVATE Cue.Project)
add_test(NAME Cue.Project.PublicHeaders COMMAND CueProjectHeaderCompile)

# 計測分布の p95、固定容量、Overflow と入れ子 Guard を確認する
add_executable(CueFoundationTimingTests Foundation/TimingSamplesTests.cpp)
target_link_libraries(CueFoundationTimingTests PRIVATE Cue.Profiler)
add_test(NAME Cue.Foundation.TimingSamples COMMAND CueFoundationTimingTests)

# 共通 Profiler の全標本集計、終了後保持、並行記録、容量超過と循環履歴を検証する
add_executable(CueProfilerTests Profiler/ProfilerTests.cpp)
target_link_libraries(CueProfilerTests PRIVATE Cue.Profiler Threads::Threads)
add_test(NAME Cue.Profiler.Core COMMAND CueProfilerTests)

# Windows 調査 Tool の File 出力を、Platform 非依存の Profiler 本体から分離して検証する
if(WIN32)
    add_executable(CueSpikeTraceExportTests Performance/SpikeTraceExportTests.cpp)
    target_link_libraries(CueSpikeTraceExportTests PRIVATE Cue.Profiler Threads::Threads)
    add_test(NAME Cue.Profiler.TraceExport COMMAND CueSpikeTraceExportTests)
endif()

add_executable(CueLoggingHeaderCompile Logging/PublicHeaderCompile.cpp)
target_link_libraries(CueLoggingHeaderCompile PRIVATE Cue.Logging)
add_test(NAME Cue.Logging.PublicHeaders COMMAND CueLoggingHeaderCompile)

if(WIN32)
    add_executable(CueSceneFileTests Scene/SceneFileTests.cpp)
    add_executable(CueDX12DepthTests Renderer/DX12DepthTests.cpp)
    target_link_libraries(CueDX12DepthTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.Depth COMMAND CueDX12DepthTests)
    set_tests_properties(Cue.Renderer.DX12.Depth PROPERTIES TIMEOUT 20)
    add_executable(CueDX12SceneRendererTests Renderer/DX12SceneRendererTests.cpp)
    target_link_libraries(CueDX12SceneRendererTests PRIVATE Cue.Renderer.DX12 Cue.Platform.Windows)
    add_test(NAME Cue.Renderer.DX12.SceneRenderer COMMAND CueDX12SceneRendererTests)
    set_tests_properties(Cue.Renderer.DX12.SceneRenderer PROPERTIES TIMEOUT 30)
    # Copy Queue の実転送・Page 共有・Fence と借用に基づく遅延破棄を WARP で検証する
    add_executable(CueDX12MeshPoolTests Renderer/DX12MeshPoolTests.cpp)
    target_link_libraries(CueDX12MeshPoolTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.MeshPool COMMAND CueDX12MeshPoolTests)
    set_tests_properties(Cue.Renderer.DX12.MeshPool PROPERTIES TIMEOUT 30)
    foreach(taskMeshHeader RHI/MeshPool DX12/DX12MeshPool)
        string(REPLACE "/" "_" taskMeshHeaderName "${taskMeshHeader}")
        set(taskMeshHeaderSource "${CMAKE_CURRENT_BINARY_DIR}/MeshHeaders/${taskMeshHeaderName}.cpp")
        file(GENERATE OUTPUT "${taskMeshHeaderSource}" CONTENT "#include <${taskMeshHeader}.h>\n")
        target_sources(CueDX12MeshPoolTests PRIVATE "${taskMeshHeaderSource}")
    endforeach()
    target_link_libraries(CueSceneFileTests PRIVATE Cue.Scene Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.Scene.FileLoading COMMAND CueSceneFileTests)
    add_executable(CueInputLatencyTests Editor/InputLatencyTests.cpp)
    target_link_libraries(CueInputLatencyTests PRIVATE Cue.EditorHost Cue.Platform.Windows Cue.Foundation.Windows imgui::imgui)
    add_test(NAME Cue.Editor.InputLatency COMMAND CueInputLatencyTests)
    set_tests_properties(Cue.Editor.InputLatency PROPERTIES TIMEOUT 45)

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

# GPU への先行数、取消と Device Lost を Native Device なしで確認する
add_executable(CueGpuFrameQueueTests Renderer/GpuFrameQueueTests.cpp)
target_link_libraries(CueGpuFrameQueueTests PRIVATE Cue.Renderer.RHI)
add_test(NAME Cue.Renderer.RHI.GpuFrameQueue COMMAND CueGpuFrameQueueTests)

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

    # Project の実 File 読み込みと起動引数の検証は GPU を作らず確認する
    add_executable(CueProjectTests Project/ProjectTests.cpp)
    target_link_libraries(CueProjectTests PRIVATE Cue.Project Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.Project.Loading COMMAND CueProjectTests)
    set_tests_properties(Cue.Project.Loading PROPERTIES TIMEOUT 15)
    add_executable(CueEditorLaunchOptionsTests Editor/EditorLaunchOptionsTests.cpp)
    target_link_libraries(CueEditorLaunchOptionsTests PRIVATE Cue.EditorHost)
    add_test(NAME Cue.Editor.ProjectArguments COMMAND CueEditorLaunchOptionsTests)
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
    # 設定の要求値と容量の契約を Window 生成前に確認する
    add_executable(CuePerformanceSettingsTests Platform/PerformanceSettingsTests.cpp)
    target_link_libraries(CuePerformanceSettingsTests PRIVATE Cue.WindowsHost Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.WindowsHost.PerformanceSettings COMMAND CuePerformanceSettingsTests)
    target_link_libraries(CueWindowsHostTests PRIVATE Cue.WindowsHost Cue.Platform.Windows Cue.Renderer.DX12)
    add_test(NAME Cue.WindowsHost.Lifecycle COMMAND CueWindowsHostTests)
    set_tests_properties(Cue.WindowsHost.Lifecycle PROPERTIES TIMEOUT 10)

    add_executable(CueFrameControllerTests Runtime/FrameControllerTests.cpp)
    target_link_libraries(CueFrameControllerTests PRIVATE Cue.Runtime.Frame Cue.Platform.Windows)
    add_test(NAME Cue.Runtime.FrameController COMMAND CueFrameControllerTests)

    # 表示同期の期限計算、Fallback、単一 Thread の非待機通知を検証する
    add_executable(CueFramePacingTests Runtime/FramePacingTests.cpp)
    target_link_libraries(CueFramePacingTests PRIVATE Cue.Runtime.Frame Cue.Platform.Windows)
    add_test(NAME Cue.Runtime.FramePacing COMMAND CueFramePacingTests)

    # 固定 Tick と描画 Frame の分離、補間、過負荷上限と周期切替を確認する
    add_executable(CueSimulationTests Runtime/SimulationTests.cpp)
    target_link_libraries(CueSimulationTests PRIVATE Cue.Runtime.Frame Cue.Platform.Windows)
    add_test(NAME Cue.Runtime.Simulation COMMAND CueSimulationTests)

    set_tests_properties(Cue.Runtime.FrameController PROPERTIES TIMEOUT 5)

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
    target_compile_features(CueWindowsHostProcessTests PRIVATE cxx_std_20)
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
    add_executable(CueEditorSceneTests Editor/EditorSceneTests.cpp)
    target_link_libraries(CueEditorSceneTests PRIVATE Cue.EditorHost Cue.Renderer.DX12)
    add_test(NAME Cue.EditorHost.Scene COMMAND CueEditorSceneTests)
    set_tests_properties(Cue.EditorHost.Scene PROPERTIES TIMEOUT 30)
    target_link_libraries(CueEditorHostTests PRIVATE Cue.EditorHost)
    add_test(NAME Cue.EditorHost.Lifecycle COMMAND CueEditorHostTests)
    set_tests_properties(Cue.EditorHost.Lifecycle PROPERTIES TIMEOUT 30)

    # 条件付きの編集値保持と容量変更時の依存値補正を GPU なしで検証する
    add_executable(CuePerformanceSettingsEditingTests Editor/PerformanceSettingsEditingTests.cpp)
    target_link_libraries(CuePerformanceSettingsEditingTests PRIVATE Cue.EditorHost Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.Editor.PerformanceSettingsEditing COMMAND CuePerformanceSettingsEditingTests)

    # 実行中の CPU / GPU / 表示枠変更と単調 Frame ID の継続を確認する
    add_executable(CuePerformanceSettingsIntegrationTests Editor/PerformanceSettingsIntegrationTests.cpp)
    target_link_libraries(CuePerformanceSettingsIntegrationTests PRIVATE Cue.EditorHost Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.EditorHost.PerformanceSettings COMMAND CuePerformanceSettingsIntegrationTests)
    set_tests_properties(Cue.EditorHost.PerformanceSettings PROPERTIES TIMEOUT 30)

    # 共通の Process Fixture で Editor の Executable の表示と正常 Close を確認する
    add_dependencies(CueWindowsHostProcessTests CueEditorHost)
    add_test(NAME Cue.EditorHost.Process COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueEditorHost> --sustain
             "${PROJECT_SOURCE_DIR}/Samples/RotateCube/RotateCube.cueproj")
    set_tests_properties(Cue.EditorHost.Process PROPERTIES TIMEOUT 15)
    add_test(NAME Cue.EditorHost.NoProject COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueEditorHost> --sustain)
    set_tests_properties(Cue.EditorHost.NoProject PROPERTIES TIMEOUT 15)
    # GPU Resize の成否と区別し、Worker 表示中の Window 状態変更と終了応答を確認する
    add_test(NAME Cue.EditorHost.ResizeMinimizeRestore
             COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueEditorHost> --resize
             "${PROJECT_SOURCE_DIR}/Samples/RotateCube/RotateCube.cueproj")
    set_tests_properties(Cue.EditorHost.ResizeMinimizeRestore PROPERTIES TIMEOUT 20)
    # 実行 File の配置先とは別の CWD から、相対 Project 引数で起動する
    add_test(NAME Cue.EditorHost.ProjectRelative
             COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueEditorHost> --sustain "RotateCube/RotateCube.cueproj")
    set_tests_properties(Cue.EditorHost.ProjectRelative PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/Samples" TIMEOUT 15)
    # Windows の Command Line 分割も含め、日本語・空白を含む設定 File の起動を検証する
    set(taskProjectFixture "${CMAKE_CURRENT_BINARY_DIR}/ProjectFixtures/回転 Cube")
    file(MAKE_DIRECTORY "${taskProjectFixture}/Source" "${taskProjectFixture}/Content")
    # gameTarget 付き Project は Source の CMake Entry も持つ。成果物未生成でも編集は起動できる
    configure_file("${PROJECT_SOURCE_DIR}/Samples/RotateCube/Source/CMakeLists.txt"
                   "${taskProjectFixture}/Source/CMakeLists.txt" COPYONLY)
    configure_file("${PROJECT_SOURCE_DIR}/Samples/RotateCube/RotateCube.cueproj"
                   "${taskProjectFixture}/回転 Cube.cueproj" COPYONLY)
    file(MAKE_DIRECTORY "${taskProjectFixture}/Content/Scenes")
    configure_file("${PROJECT_SOURCE_DIR}/Samples/RotateCube/Content/Scenes/RotateCube.cuescene"
                   "${taskProjectFixture}/Content/Scenes/RotateCube.cuescene" COPYONLY)
    add_test(NAME Cue.EditorHost.ProjectUnicode
             COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueEditorHost> --sustain
                     "${taskProjectFixture}/回転 Cube.cueproj")
    set_tests_properties(Cue.EditorHost.ProjectUnicode PROPERTIES TIMEOUT 15)
    add_test(NAME Cue.EditorHost.ProjectMissingArgument
             COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueEditorHost> --project-failure)
    add_test(NAME Cue.EditorHost.ProjectMissingFile
             COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueEditorHost> --project-failure
                     "${taskProjectFixture}/Missing.cueproj")
    # 未対応 Version も GPU 初期化前の診断と終了 Code 1 で処理する
    file(READ "${PROJECT_SOURCE_DIR}/Samples/RotateCube/RotateCube.cueproj" taskProjectDescriptor)
    string(REPLACE "formatVersion=1" "formatVersion=2" taskUnsupportedProject "${taskProjectDescriptor}")
    file(WRITE "${taskProjectFixture}/Unsupported.cueproj" "${taskUnsupportedProject}")
    add_test(NAME Cue.EditorHost.ProjectUnsupportedVersion
             COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueEditorHost> --project-failure
                     "${taskProjectFixture}/Unsupported.cueproj")
    set_tests_properties(Cue.EditorHost.ProjectMissingArgument Cue.EditorHost.ProjectMissingFile
                         Cue.EditorHost.ProjectUnsupportedVersion PROPERTIES TIMEOUT 15)
    # Project の配置は正しくても、Scene 本文が不正なら表示基盤の生成前に拒否する
    file(READ "${PROJECT_SOURCE_DIR}/Samples/RotateCube/Content/Scenes/RotateCube.cuescene" taskSceneDescriptor)
    string(REGEX REPLACE "formatVersion=[0-9]+" "formatVersion=3" taskBadScene "${taskSceneDescriptor}")
    file(WRITE "${taskProjectFixture}/Content/Scenes/Unsupported.cuescene" "${taskBadScene}")
    string(REPLACE "RotateCube.cuescene" "Unsupported.cuescene" taskBadSceneProject "${taskProjectDescriptor}")
    file(WRITE "${taskProjectFixture}/UnsupportedScene.cueproj" "${taskBadSceneProject}")
    add_test(NAME Cue.EditorHost.SceneUnsupportedVersion
             COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueEditorHost> --project-failure
                     "${taskProjectFixture}/UnsupportedScene.cueproj")
    set_tests_properties(Cue.EditorHost.SceneUnsupportedVersion PROPERTIES TIMEOUT 15)
endif()

if(CUE_BUILD_PERFORMANCE_TOOLS)
    add_subdirectory(Performance)
endif()

# Script の保存・SDK・実行を分け、Native Game Code の失敗を World / Editor と混同しない
add_executable(CueScriptSdkTests Script/ScriptSdkTests.cpp)
target_link_libraries(CueScriptSdkTests PRIVATE Cue.Script)
add_test(NAME Cue.Script.Sdk COMMAND CueScriptSdkTests)

add_executable(CueScriptRuntimeTests Script/ScriptRuntimeTests.cpp)
target_link_libraries(CueScriptRuntimeTests PRIVATE Cue.ScriptRuntime Cue.SceneRuntime Threads::Threads)
add_test(NAME Cue.Script.Runtime COMMAND CueScriptRuntimeTests)

add_executable(CueSceneScriptTests Scene/SceneScriptTests.cpp)
target_link_libraries(CueSceneScriptTests PRIVATE Cue.Scene)
add_test(NAME Cue.Scene.Script COMMAND CueSceneScriptTests)

add_executable(CueEditorScriptTests Editor/EditorScriptTests.cpp)
target_link_libraries(CueEditorScriptTests PRIVATE Cue.Editor Threads::Threads)
add_test(NAME Cue.Editor.Script COMMAND CueEditorScriptTests)

if(WIN32)
    add_executable(CueProjectBuildContractTests Project/ProjectBuildContractTests.cpp)
    target_link_libraries(CueProjectBuildContractTests PRIVATE Cue.Project Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.Project.BuildContract COMMAND CueProjectBuildContractTests)
    add_executable(CueGameBuildArtifactTests Project/GameBuildArtifactTests.cpp)
    target_link_libraries(CueGameBuildArtifactTests PRIVATE Cue.ProjectBuild Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.Project.GameBuildArtifacts COMMAND CueGameBuildArtifactTests)

    # Build 対象の選択と、製品・Play 共通の実行契約を独立して確認する
    add_executable(CueGameBuildTargetTests Project/GameBuildTargetTests.cpp)
    target_link_libraries(CueGameBuildTargetTests PRIVATE Cue.ProjectBuild)
    add_test(NAME Cue.Project.GameBuildTarget COMMAND CueGameBuildTargetTests)
    add_executable(CueEditorBuildUiTests Editor/EditorBuildUiTests.cpp)
    target_link_libraries(CueEditorBuildUiTests PRIVATE Cue.Editor.Ui imgui::imgui)
    add_test(NAME Cue.Editor.BuildUi COMMAND CueEditorBuildUiTests)
    add_executable(CueEditorGameBuildTests EditorHost/EditorGameBuildTests.cpp)
    target_link_libraries(CueEditorGameBuildTests PRIVATE
        Cue.EditorHost Cue.Renderer.DX12 Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.Editor.GameBuild COMMAND CueEditorGameBuildTests)
    set_tests_properties(Cue.Editor.GameBuild PROPERTIES TIMEOUT 30)
    cue_copy_shader_runtime(CueEditorGameBuildTests)
    # Window と GPU を維持する Project 再選択の所有権と失敗時保全を実 WARP で確認する
    add_executable(CueEditorProjectSwitchTests EditorHost/EditorProjectSwitchTests.cpp)
    target_link_libraries(CueEditorProjectSwitchTests PRIVATE
        Cue.EditorHost Cue.Renderer.DX12 Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.Editor.ProjectSwitch COMMAND CueEditorProjectSwitchTests)
    set_tests_properties(Cue.Editor.ProjectSwitch PROPERTIES TIMEOUT 60)
    cue_copy_shader_runtime(CueEditorProjectSwitchTests)
    add_executable(CueGameBuildServiceFixture WIN32 Project/GameBuildServiceFixture.cpp)
    target_link_libraries(CueGameBuildServiceFixture PRIVATE shell32)
    target_compile_features(CueGameBuildServiceFixture PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(CueGameBuildServiceFixture PRIVATE /utf-8)
    endif()
    add_executable(CueGameBuildServiceTests Project/GameBuildServiceTests.cpp)
    target_link_libraries(CueGameBuildServiceTests PRIVATE
        Cue.ProjectBuild.Windows Cue.Platform.Windows Cue.Foundation.Windows)
    add_dependencies(CueGameBuildServiceTests CueGameBuildServiceFixture)
    add_test(NAME Cue.Project.GameBuildService COMMAND CueGameBuildServiceTests $<TARGET_FILE:CueGameBuildServiceFixture>)
    set_tests_properties(Cue.Project.GameBuildService PROPERTIES TIMEOUT 45)
    add_executable(CueScriptSourceTests EditorIde/ScriptSourceTests.cpp)
    target_link_libraries(CueScriptSourceTests PRIVATE
        Cue.EditorIde Cue.Project Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.EditorIde.ScriptSource COMMAND CueScriptSourceTests)
    add_executable(CueIdeBridgeFixture WIN32 EditorIde/IdeBridgeFixture.cpp)
    target_link_libraries(CueIdeBridgeFixture PRIVATE shell32)
    target_compile_features(CueIdeBridgeFixture PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(CueIdeBridgeFixture PRIVATE /utf-8)
    endif()
    add_executable(CueWindowsIdeServiceTests EditorIde/WindowsIdeServiceTests.cpp)
    target_link_libraries(CueWindowsIdeServiceTests PRIVATE
        Cue.EditorIde.Windows Cue.Platform.Windows Cue.Foundation.Windows)
    add_dependencies(CueWindowsIdeServiceTests CueIdeBridgeFixture Cue.VisualStudioBridge)
    add_test(NAME Cue.EditorIde.WindowsService COMMAND CueWindowsIdeServiceTests $<TARGET_FILE:CueIdeBridgeFixture>)
    set_tests_properties(Cue.EditorIde.WindowsService PROPERTIES TIMEOUT 15)
    get_property(taskVisualStudioBridge GLOBAL PROPERTY CUE_VISUAL_STUDIO_BRIDGE_EXE)
    add_test(NAME Cue.EditorIde.BridgeInvalidArguments COMMAND "${taskVisualStudioBridge}" --invalid)
    set_tests_properties(Cue.EditorIde.BridgeInvalidArguments PROPERTIES WILL_FAIL TRUE TIMEOUT 10)
    add_executable(CueEditorScriptControllerTests EditorHost/EditorScriptControllerTests.cpp)
    target_include_directories(CueEditorScriptControllerTests PRIVATE "${PROJECT_SOURCE_DIR}/Engine/Source/EditorHost/Private")
    target_link_libraries(CueEditorScriptControllerTests PRIVATE Cue.EditorHost Cue.EditorIde Cue.ProjectBuild
        Cue.ScriptPlay Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.Editor.ScriptController COMMAND CueEditorScriptControllerTests)
    add_executable(CueGameStartupTests GameRuntime/GameStartupTests.cpp)
    target_link_libraries(CueGameStartupTests PRIVATE Cue.GameRuntime Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.Runtime.GameStartup COMMAND CueGameStartupTests)
    add_executable(CueGameSessionTests Runtime/GameSessionTests.cpp)
    target_link_libraries(CueGameSessionTests PRIVATE Cue.GameRuntime Threads::Threads)
    add_test(NAME Cue.Runtime.GameSession COMMAND CueGameSessionTests)

    get_property(taskConfiguredGame GLOBAL PROPERTY CUE_CONFIGURED_GAME_TARGET)
    get_property(taskConfiguredProject GLOBAL PROPERTY CUE_CONFIGURED_GAME_PROJECT)
    get_property(taskConfiguredProjectId GLOBAL PROPERTY CUE_CONFIGURED_GAME_PROJECT_ID)
    get_property(taskPlayerDirectory GLOBAL PROPERTY CUE_CONFIGURED_GAME_PLAYER_DIRECTORY)
    if(taskConfiguredGame)
        add_dependencies(CueGameBuildArtifactTests Cue.GameProjectBuild)
        add_test(NAME Cue.Project.GameBuildArtifactIntegration COMMAND CueGameBuildArtifactTests
                 "${taskConfiguredProject}" "$<TARGET_FILE:${taskConfiguredGame}>")
        add_executable(CueEditorPlayServicesTests EditorHost/EditorPlayServicesTests.cpp)
        target_include_directories(CueEditorPlayServicesTests PRIVATE "${PROJECT_SOURCE_DIR}/Engine/Source/EditorHost/Private")
        target_link_libraries(CueEditorPlayServicesTests PRIVATE
            Cue.EditorHost Cue.ProjectBuild Cue.ScriptPlay Cue.Renderer.DX12
            Cue.Platform.Windows Cue.Foundation.Windows)
        add_dependencies(CueEditorPlayServicesTests Cue.GameProjectBuild CuePlayHost)
        add_test(NAME Cue.Editor.PlayConfiguration COMMAND CueEditorPlayServicesTests
                 $<TARGET_FILE:CuePlayHost> "${taskConfiguredProject}")
        set_tests_properties(Cue.Editor.PlayConfiguration PROPERTIES TIMEOUT 30)
        cue_copy_shader_runtime(CueEditorPlayServicesTests)
    endif()
    add_test(NAME Cue.Project.GameCMake COMMAND "${CMAKE_COMMAND}"
        "-DCUE_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        "-DCUE_FIXTURE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/ProjectCMakeFixtures"
        "-DCUE_GENERATOR=${CMAKE_GENERATOR}"
        "-DCUE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DCUE_CONFIGURATION=$<CONFIG>"
        -P "${PROJECT_SOURCE_DIR}/Engine/Tests/Project/GameProjectCMakeTests.cmake")
    set_tests_properties(Cue.Project.GameCMake PROPERTIES TIMEOUT 90)
    add_test(NAME Cue.Project.GamePlayerCMake COMMAND "${CMAKE_COMMAND}"
        "-DCUE_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        "-DCUE_FIXTURE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/ProjectCMakeFixtures"
        "-DCUE_GENERATOR=${CMAKE_GENERATOR}"
        "-DCUE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DCUE_CONFIGURATION=$<CONFIG>"
        -P "${PROJECT_SOURCE_DIR}/Engine/Tests/Project/GamePlayerCMakeTests.cmake")
    set_tests_properties(Cue.Project.GamePlayerCMake PROPERTIES TIMEOUT 90)

    # 選択 Project が Sample 以外でも Engine のテスト群を構成できるよう、Sample 固有検証だけを条件化する
    if(taskConfiguredProjectId STREQUAL "7f533c49-d416-47ea-98c1-026822b1e6ed"
       AND TARGET RotateCubeGame AND TARGET RotateCubeGame.Static)
        if(taskPlayerDirectory AND TARGET Cue.GamePlayerPackage)
            # 配布物だけを別 Directory へ移し、可視描画・静的登録・回転・停止を検証する
            add_executable(CueGamePlayerProcessTests GameRuntime/GamePlayerProcessTests.cpp)
            target_link_libraries(CueGamePlayerProcessTests PRIVATE
                Cue.GameRuntime Cue.Platform.Windows Cue.Foundation.Windows user32 psapi)
            add_dependencies(CueGamePlayerProcessTests Cue.GamePlayerPackage)
            add_test(NAME Cue.GamePlayer.Relocated COMMAND CueGamePlayerProcessTests "${taskPlayerDirectory}")
            set_tests_properties(Cue.GamePlayer.Relocated PROPERTIES TIMEOUT 40)
        endif()
        add_executable(CueScriptModuleLoadingTests Script/ScriptModuleLoadingTests.cpp)
        target_link_libraries(CueScriptModuleLoadingTests PRIVATE Cue.ScriptRuntime Cue.SceneRuntime RotateCubeGame.Static)
        add_dependencies(CueScriptModuleLoadingTests RotateCubeGame)
        add_test(NAME Cue.Script.ModuleLoading COMMAND CueScriptModuleLoadingTests $<TARGET_FILE:RotateCubeGame>)
    endif()
    add_executable(CueEditorScriptUiTests Editor/EditorScriptUiTests.cpp)
    target_link_libraries(CueEditorScriptUiTests PRIVATE Cue.Editor.Ui imgui::imgui)
    add_test(NAME Cue.Editor.ScriptUi COMMAND CueEditorScriptUiTests)
    # Native Crash / Hang を Test Process へ持ち込まず、所有 Job の回収と再 Play を検証する
    add_library(CuePlayFailureGame SHARED Script/PlayFailureGame.cpp)
    target_link_libraries(CuePlayFailureGame PRIVATE Cue.Script)
    set_target_properties(CuePlayFailureGame PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    if(MSVC)
        target_compile_options(CuePlayFailureGame PRIVATE /utf-8)
    endif()
    add_executable(CuePlayProtocolFixture WIN32 Script/PlayProtocolFixture.cpp)
    target_link_libraries(CuePlayProtocolFixture PRIVATE shell32)
    set_target_properties(CuePlayProtocolFixture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    if(MSVC)
        target_compile_options(CuePlayProtocolFixture PRIVATE /utf-8)
    endif()
    if(taskConfiguredProjectId STREQUAL "7f533c49-d416-47ea-98c1-026822b1e6ed" AND TARGET RotateCubeGame)
        add_executable(CueScriptPlayTests Script/ScriptPlayTests.cpp)
        target_link_libraries(CueScriptPlayTests PRIVATE Cue.ScriptPlay)
        add_dependencies(CueScriptPlayTests CuePlayHost RotateCubeGame CuePlayFailureGame CuePlayProtocolFixture)
        add_test(NAME Cue.Script.Play COMMAND CueScriptPlayTests $<TARGET_FILE:CuePlayHost>
                 $<TARGET_FILE:RotateCubeGame> $<TARGET_FILE:CuePlayFailureGame> $<TARGET_FILE:CuePlayProtocolFixture>)
        set_tests_properties(Cue.Script.Play PROPERTIES TIMEOUT 40)
        add_executable(CueScriptDebugPlayTests Script/ScriptDebugPlayTests.cpp)
        target_link_libraries(CueScriptDebugPlayTests PRIVATE Cue.ScriptPlay Cue.EditorIde.Windows
            Cue.Platform.Windows Cue.Foundation.Windows)
        add_dependencies(CueScriptDebugPlayTests CuePlayHost RotateCubeGame)
        add_test(NAME Cue.Script.DebugPlay COMMAND CueScriptDebugPlayTests
            $<TARGET_FILE:CuePlayHost> $<TARGET_FILE:RotateCubeGame>)
        set_tests_properties(Cue.Script.DebugPlay PROPERTIES TIMEOUT 60)
    endif()
endif()

# 編集正本・保存の失敗境界を Native UI / GPU から独立して検証する
if(WIN32)
    add_executable(CueEngineInstallationTests Project/EngineInstallationTests.cpp)
    target_link_libraries(CueEngineInstallationTests PRIVATE Cue.EngineInstallation Cue.EngineVersion
        Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.EngineInstallation COMMAND CueEngineInstallationTests)
    add_executable(CueProjectCreatorTests ProjectTools/ProjectCreatorTests.cpp)
    target_link_libraries(CueProjectCreatorTests PRIVATE Cue.ProjectTools Cue.Scene Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.ProjectTools.Create COMMAND CueProjectCreatorTests "${PROJECT_SOURCE_DIR}/Templates")
    set_tests_properties(Cue.ProjectTools.Create PROPERTIES TIMEOUT 30)
    add_test(NAME Cue.ProjectTools.CMakeVersion COMMAND "${CMAKE_COMMAND}"
        "-DCUE_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        "-DCUE_FIXTURE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/ProjectToolsCMake"
        -P "${PROJECT_SOURCE_DIR}/Engine/Tests/ProjectTools/ProjectVersionCMakeTests.cmake")
    add_test(NAME Cue.ProjectTools.Cli COMMAND pwsh -NoProfile -File
        "${PROJECT_SOURCE_DIR}/Engine/Tests/ProjectTools/ProjectToolCliTests.ps1"
        -Tool "$<TARGET_FILE:CueProjectTool>" -TestRoot "${CMAKE_CURRENT_BINARY_DIR}/ProjectToolCli")
    set_tests_properties(Cue.ProjectTools.Cli PROPERTIES TIMEOUT 30)
    add_test(NAME Cue.Packaging.Contracts COMMAND pwsh -NoProfile -File
        "${PROJECT_SOURCE_DIR}/Tools/Packaging/Scripts/PackageCommonTests.ps1"
        -TestRoot "${CMAKE_CURRENT_BINARY_DIR}/PackageContractTests")
    set_tests_properties(Cue.Packaging.Contracts PROPERTIES TIMEOUT 30)
    # GitHub へ送信せず、署名検証と Release 公開の失敗境界を確認する
    add_test(NAME Cue.Packaging.TaggedRelease COMMAND pwsh -NoProfile -File
        "${PROJECT_SOURCE_DIR}/Tools/Packaging/Scripts/TaggedReleaseTests.ps1"
        -TestRoot "${CMAKE_CURRENT_BINARY_DIR}/TaggedReleaseTests")
    set_tests_properties(Cue.Packaging.TaggedRelease PROPERTIES TIMEOUT 60)
endif()

add_executable(CueEditorApplicationTests Editor/EditorApplicationTests.cpp)
target_link_libraries(CueEditorApplicationTests PRIVATE Cue.Editor)
add_test(NAME Cue.Editor.Application COMMAND CueEditorApplicationTests)
add_executable(CueSceneDocumentTests Editor/SceneDocumentTests.cpp)
target_link_libraries(CueSceneDocumentTests PRIVATE Cue.Editor)
add_test(NAME Cue.Editor.Document COMMAND CueSceneDocumentTests)
add_executable(CueMathEulerTests Math/EulerTests.cpp)
target_link_libraries(CueMathEulerTests PRIVATE Cue.Math)
add_test(NAME Cue.Math.Euler COMMAND CueMathEulerTests)

if(WIN32)
    # Native Close の取消と Object UUID 発行を Dialog 操作なしで検証する
    add_executable(CueEditorUiTests Editor/EditorUiTests.cpp)
    target_link_libraries(CueEditorUiTests PRIVATE Cue.Editor.Ui imgui::imgui)
    add_test(NAME Cue.Editor.Ui COMMAND CueEditorUiTests)
    add_executable(CueWindowsCloseCancellationTests Platform/WindowsCloseCancellationTests.cpp)
    target_link_libraries(CueWindowsCloseCancellationTests PRIVATE Cue.Platform.Windows)
    add_test(NAME Cue.Platform.Windows.CloseCancellation COMMAND CueWindowsCloseCancellationTests)
    add_executable(CueEditorWindowsServicesTests EditorHost/EditorWindowsServicesTests.cpp)
    target_include_directories(CueEditorWindowsServicesTests PRIVATE "${PROJECT_SOURCE_DIR}/Engine/Source/EditorHost/Private")
    target_link_libraries(CueEditorWindowsServicesTests PRIVATE Cue.EditorHost Cue.Platform.Windows Cue.Foundation.Windows)
    add_test(NAME Cue.Editor.WindowsServices COMMAND CueEditorWindowsServicesTests)
endif()

if(WIN32)
    set(taskShaderTests CueDX12PipelineManagerTests
        CueDX12SceneRendererTests
        CueDX12BackendTests CueDX12MainFrameGraphTests CueDX12FrameGraphFramesTests CueDX12SwapChainTests
        CueEditorHostTests CueEditorSceneTests CueImGuiDX12BackendTests CuePerformanceSettingsIntegrationTests CueInputLatencyTests
    )
    if(CUE_BUILD_PERFORMANCE_TOOLS)
        list(APPEND taskShaderTests CueM09FramePacingProbe CueSpikeTimingProbe)
    endif()
    cue_copy_shader_runtime(${taskShaderTests})
endif()

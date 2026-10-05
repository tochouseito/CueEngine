if(MSVC)
    # Test Source の日本語コメントを UTF-8 として扱う
    add_compile_options(/utf-8)
endif()

# Foundation と公開 Header は Platform 実装なしで単独検証する
add_executable(CueFoundationResultTests Foundation/ResultTests.cpp)
target_link_libraries(CueFoundationResultTests PRIVATE Cue.Foundation)
add_test(NAME Cue.Foundation.Result COMMAND CueFoundationResultTests)

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
    target_link_libraries(CueDX12PipelineManagerTests PRIVATE Cue.Renderer.DX12)
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
    target_link_libraries(CueWindowsHostTests PRIVATE Cue.WindowsHost)
    add_test(NAME Cue.WindowsHost.Lifecycle COMMAND CueWindowsHostTests)
    set_tests_properties(Cue.WindowsHost.Lifecycle PROPERTIES TIMEOUT 10)

    add_executable(CueFrameControllerTests Runtime/FrameControllerTests.cpp)
    target_link_libraries(CueFrameControllerTests PRIVATE Cue.Runtime.Frame Cue.Platform.Windows)
    add_test(NAME Cue.Runtime.FrameController COMMAND CueFrameControllerTests)
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
    target_link_libraries(CueWindowsHostProcessTests PRIVATE user32)
    add_test(NAME Cue.WindowsHost.Process COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueWindowsHost>)
    add_test(NAME Cue.WindowsHost.ResizeMinimizeRestore
             COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueWindowsHost> --resize)
    set_tests_properties(Cue.WindowsHost.Process PROPERTIES TIMEOUT 7)
    set_tests_properties(Cue.WindowsHost.ResizeMinimizeRestore PROPERTIES TIMEOUT 20)

    # Editor の既定 Thread 構成と失敗時停止を実 Window で確認する
    add_executable(CueEditorHostTests Editor/EditorHostTests.cpp)
    target_link_libraries(CueEditorHostTests PRIVATE Cue.EditorHost)
    add_test(NAME Cue.EditorHost.Lifecycle COMMAND CueEditorHostTests)
    set_tests_properties(Cue.EditorHost.Lifecycle PROPERTIES TIMEOUT 15)

    # 共通の Process Fixture で Editor の Executable の表示と正常 Close を確認する
    add_dependencies(CueWindowsHostProcessTests CueEditorHost)
    add_test(NAME Cue.EditorHost.Process COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueEditorHost>)
    set_tests_properties(Cue.EditorHost.Process PROPERTIES TIMEOUT 7)
endif()

if(WIN32)
    cue_copy_shader_runtime(CueDX12PipelineManagerTests
        CueDX12BackendTests CueDX12MainFrameGraphTests CueDX12FrameGraphFramesTests CueDX12SwapChainTests
        CueEditorHostTests
    )
endif()

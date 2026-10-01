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

    # 複数 Queue の貸出、GPU Fence 同期、返却後の再利用を WARP で検証する
    add_executable(CueDX12QueuePoolTests Renderer/DX12QueuePoolTests.cpp)
    target_link_libraries(CueDX12QueuePoolTests PRIVATE Cue.Renderer.DX12)
    add_test(NAME Cue.Renderer.DX12.QueuePool COMMAND CueDX12QueuePoolTests)
    set_tests_properties(Cue.Renderer.DX12.QueuePool PROPERTIES TIMEOUT 15)

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
endif()

# Foundation と公開 Header は Platform 実装なしで単独検証する
add_executable(CueFoundationResultTests
    Foundation/ResultTests.cpp
)

target_link_libraries(CueFoundationResultTests PRIVATE Cue.Foundation)

add_test(NAME Cue.Foundation.Result COMMAND CueFoundationResultTests)

add_executable(CuePlatformHeaderCompile
    Platform/PublicHeaderCompile.cpp
)

target_link_libraries(CuePlatformHeaderCompile PRIVATE Cue.Platform)

add_test(NAME Cue.Platform.PublicHeaders COMMAND CuePlatformHeaderCompile)

# Backend に依存しない Graph の依存順と状態計画を検証する
add_executable(CueFrameGraphTests Renderer/FrameGraphTests.cpp)
target_link_libraries(CueFrameGraphTests PRIVATE Cue.Renderer.FrameGraph)
add_test(NAME Cue.Renderer.FrameGraph COMMAND CueFrameGraphTests)

if(WIN32)
    # 利用可能な Adapter で実 Window に対する D3D12 初期化と停止を検証する
    add_executable(CueD3D12RendererInitializationTests
        Renderer/D3D12RendererInitializationTests.cpp
    )

    target_link_libraries(CueD3D12RendererInitializationTests PRIVATE
        Cue.Renderer.D3D12 Cue.Platform.Windows
    )

    add_test(NAME Cue.Renderer.D3D12.Initialization COMMAND CueD3D12RendererInitializationTests)
    set_tests_properties(Cue.Renderer.D3D12.Initialization PROPERTIES TIMEOUT 15)

    # 選択した Device の最高機能レベルを Runtime の報告値と照合する
    add_executable(CueD3D12FeatureLevelTests Renderer/D3D12FeatureLevelTests.cpp)
    target_include_directories(CueD3D12FeatureLevelTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Source/Renderer/D3D12/Private)
    target_link_libraries(CueD3D12FeatureLevelTests PRIVATE Cue.Renderer.D3D12)
    add_test(NAME Cue.Renderer.D3D12.FeatureLevel COMMAND CueD3D12FeatureLevelTests)
    set_tests_properties(Cue.Renderer.D3D12.FeatureLevel PROPERTIES TIMEOUT 15)

    # 内部 Owner の Slot 失効と Direct Queue Context の再利用条件を検証する
    add_executable(CueD3D12ResourceOwnershipTests
        Renderer/D3D12ResourceOwnershipTests.cpp
    )

    target_include_directories(CueD3D12ResourceOwnershipTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Source/Renderer/D3D12/Private
    )

    target_link_libraries(CueD3D12ResourceOwnershipTests PRIVATE Cue.Renderer.D3D12)

    add_test(NAME Cue.Renderer.D3D12.Ownership COMMAND CueD3D12ResourceOwnershipTests)
    set_tests_properties(Cue.Renderer.D3D12.Ownership PROPERTIES TIMEOUT 15)

    # 固定 Mesh の描画結果を選択した Adapter の Readback Pixel で確認する
    add_executable(CueD3D12FixedMeshPixelTests Renderer/D3D12FixedMeshPixelTests.cpp)
    target_include_directories(CueD3D12FixedMeshPixelTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Source/Renderer/D3D12/Private)
    target_link_libraries(CueD3D12FixedMeshPixelTests PRIVATE Cue.Renderer.D3D12)
    add_test(NAME Cue.Renderer.D3D12.FixedMeshPixel COMMAND CueD3D12FixedMeshPixelTests)
    set_tests_properties(Cue.Renderer.D3D12.FixedMeshPixel PROPERTIES TIMEOUT 15)

    # Runtime と Host の Test は実際の Windows Service を接続して動かす
    add_executable(CueRuntimeTests
        Runtime/RuntimeTests.cpp
    )

    target_link_libraries(CueRuntimeTests PRIVATE Cue.Runtime Cue.Platform.Windows)

    add_test(NAME Cue.Runtime.Windowless COMMAND CueRuntimeTests)

    add_executable(CueWindowsHostTests
        Platform/WindowsHostTests.cpp
    )

    target_link_libraries(CueWindowsHostTests PRIVATE Cue.WindowsHost)

    add_test(NAME Cue.WindowsHost.Lifecycle COMMAND CueWindowsHostTests)

    # Window を表示する Test が固着しないよう実行時間を制限する
    set_tests_properties(Cue.WindowsHost.Lifecycle PROPERTIES TIMEOUT 10)

    add_executable(CueFrameControllerTests
        Runtime/FrameControllerTests.cpp
    )

    target_link_libraries(CueFrameControllerTests PRIVATE Cue.Runtime.Frame Cue.Platform.Windows)

    add_test(NAME Cue.Runtime.FrameController COMMAND CueFrameControllerTests)

    set_tests_properties(Cue.Runtime.FrameController PROPERTIES TIMEOUT 5)

    add_executable(CueWindowsThreadServicesTests
        Platform/WindowsThreadServicesTests.cpp
    )

    target_link_libraries(CueWindowsThreadServicesTests PRIVATE Cue.Platform.Windows)

    add_test(NAME Cue.Platform.Windows.Threading COMMAND CueWindowsThreadServicesTests)
    set_tests_properties(Cue.Platform.Windows.Threading PROPERTIES TIMEOUT 5)

    add_executable(CueWindowsUtfConversionTests
        Foundation/WindowsUtfConversionTests.cpp
    )

    target_link_libraries(CueWindowsUtfConversionTests PRIVATE Cue.Foundation.Windows)

    add_test(NAME Cue.Foundation.Windows.UtfConversion COMMAND CueWindowsUtfConversionTests)

    add_executable(CueWindowsWindowLifecycleTests
        Platform/WindowsWindowLifecycleTests.cpp
    )

    target_link_libraries(CueWindowsWindowLifecycleTests PRIVATE Cue.Platform.Windows user32)

    add_test(NAME Cue.Platform.Windows.Lifecycle COMMAND CueWindowsWindowLifecycleTests)

    add_executable(CueWindowsHostProcessTests
        Platform/WindowsHostProcessTests.cpp
    )

    target_link_libraries(CueWindowsHostProcessTests PRIVATE Cue.WindowsHost user32)

    add_test(NAME Cue.WindowsHost.Process COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueWindowsHost>)

    # 製品Hostへ実際のResizeと最小化・復帰を送って終了経路まで確認する
    add_test(NAME Cue.WindowsHost.ResizeMinimizeRestore
             COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueWindowsHost> --resize)

    add_test(NAME Cue.WindowsHost.AutoFrames COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueWindowsHostProcessTests> --auto)

    add_test(NAME Cue.WindowsHost.SingleThread COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueWindowsHostProcessTests> --auto-single)

    add_test(NAME Cue.WindowsHost.RenderFailure COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueWindowsHostProcessTests> --expect-render-failure)

    # 子 Process を使う正常・異常終了の Test も時間制限を共有する
    set_tests_properties(Cue.WindowsHost.Process Cue.WindowsHost.AutoFrames Cue.WindowsHost.SingleThread
                         Cue.WindowsHost.RenderFailure PROPERTIES TIMEOUT 7)
    set_tests_properties(Cue.WindowsHost.ResizeMinimizeRestore PROPERTIES TIMEOUT 20)
endif()

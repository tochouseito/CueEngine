# Foundation と公開 Header は Platform 実装なしで単独検証する
add_executable(CueFoundationResultTests
    Foundation/ResultTests.cpp
)

target_link_libraries(CueFoundationResultTests PRIVATE Cue.Foundation)

add_test(NAME Cue.Foundation.Result COMMAND CueFoundationResultTests)

add_executable(CuePlatformHeaderCompile
    Platform/PublicHeaderCompile.cpp
)

target_link_libraries(CuePlatformHeaderCompile PRIVATE Cue.Platform Cue.Renderer.RHI)

add_test(NAME Cue.Platform.PublicHeaders COMMAND CuePlatformHeaderCompile)

# Backend に依存しない Graph の依存順と状態計画を検証する
add_executable(CueFrameGraphTests Renderer/FrameGraphTests.cpp)
target_link_libraries(CueFrameGraphTests PRIVATE Cue.Renderer.FrameGraph)
add_test(NAME Cue.Renderer.FrameGraph COMMAND CueFrameGraphTests)

# Legacy の SlotUploader の整列配置と Frame 単位の更新契約を検証する
add_executable(CueSlotUploaderTests Renderer/SlotUploaderTests.cpp)
target_link_libraries(CueSlotUploaderTests PRIVATE Cue.Renderer.RHI)
add_test(NAME Cue.Renderer.SlotUploader COMMAND CueSlotUploaderTests)

if(WIN32)
    # 利用可能な Adapter で実 Window に対する DX12 初期化と停止を検証する
    add_executable(CueDX12BackendInitializationTests
        Renderer/DX12BackendInitializationTests.cpp
    )

    target_link_libraries(CueDX12BackendInitializationTests PRIVATE
        Cue.Renderer.DX12 Cue.Renderer.FrameGraph Cue.Platform.Windows
    )
    cue_stage_dxc_runtime(CueDX12BackendInitializationTests)

    add_test(NAME Cue.Renderer.DX12.Initialization COMMAND CueDX12BackendInitializationTests)
    set_tests_properties(Cue.Renderer.DX12.Initialization PROPERTIES TIMEOUT 15)

    # 選択した Device の最高機能レベルを Runtime の報告値と照合する
    add_executable(CueDX12FeatureLevelTests Renderer/DX12FeatureLevelTests.cpp)
    target_include_directories(CueDX12FeatureLevelTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Source/Renderer/DX12/Private)
    target_link_libraries(CueDX12FeatureLevelTests PRIVATE Cue.Renderer.DX12)
    cue_stage_dxc_runtime(CueDX12FeatureLevelTests)
    add_test(NAME Cue.Renderer.DX12.FeatureLevel COMMAND CueDX12FeatureLevelTests)
    set_tests_properties(Cue.Renderer.DX12.FeatureLevel PROPERTIES TIMEOUT 15)

    # 内部 Owner の Slot 失効と Direct Queue Context の再利用条件を検証する
    add_executable(CueDX12ResourceOwnershipTests
        Renderer/DX12ResourceOwnershipTests.cpp
    )

    target_include_directories(CueDX12ResourceOwnershipTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Source/Renderer/DX12/Private
    )

    target_link_libraries(CueDX12ResourceOwnershipTests PRIVATE Cue.Renderer.DX12)
    cue_stage_dxc_runtime(CueDX12ResourceOwnershipTests)

    add_test(NAME Cue.Renderer.DX12.Ownership COMMAND CueDX12ResourceOwnershipTests)
    set_tests_properties(Cue.Renderer.DX12.Ownership PROPERTIES TIMEOUT 15)

    # 固定 Mesh の描画結果を選択した Adapter の Readback Pixel で確認する
    add_executable(CueDX12FixedMeshPixelTests Renderer/DX12FixedMeshPixelTests.cpp)
    target_include_directories(CueDX12FixedMeshPixelTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Source/Renderer/DX12/Private)
    target_link_libraries(CueDX12FixedMeshPixelTests PRIVATE Cue.Renderer.DX12 Cue.Renderer.FrameGraph)
    cue_stage_dxc_runtime(CueDX12FixedMeshPixelTests)
    add_test(NAME Cue.Renderer.DX12.FixedMeshPixel COMMAND CueDX12FixedMeshPixelTests)
    set_tests_properties(Cue.Renderer.DX12.FixedMeshPixel PROPERTIES TIMEOUT 15)

    # Copy→Compute→Graphics の Queue 間依存と Readback 結果を実 GPU で確認する
    add_executable(CueDX12MultiQueueGraphTests Renderer/DX12MultiQueueGraphTests.cpp)
    target_include_directories(CueDX12MultiQueueGraphTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Source/Renderer/DX12/Private)
    target_link_libraries(CueDX12MultiQueueGraphTests PRIVATE Cue.Renderer.DX12 Cue.Renderer.FrameGraph)
    target_compile_definitions(CueDX12MultiQueueGraphTests PRIVATE
        CUE_TEST_SHADER_PATH="${CMAKE_SOURCE_DIR}/Engine/Shader/FixedMesh.hlsl")
    cue_stage_dxc_runtime(CueDX12MultiQueueGraphTests)
    add_test(NAME Cue.Renderer.DX12.MultiQueueGraph COMMAND CueDX12MultiQueueGraphTests)
    set_tests_properties(Cue.Renderer.DX12.MultiQueueGraph PROPERTIES TIMEOUT 15)

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
    cue_stage_dxc_runtime(CueWindowsHostTests)

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
    cue_stage_dxc_runtime(CueWindowsHostProcessTests)

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

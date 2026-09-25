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

if(WIN32)
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

    add_test(NAME Cue.WindowsHost.AutoFrames COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueWindowsHostProcessTests> --auto)

    add_test(NAME Cue.WindowsHost.SingleThread COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueWindowsHostProcessTests> --auto-single)

    add_test(NAME Cue.WindowsHost.RenderFailure COMMAND CueWindowsHostProcessTests $<TARGET_FILE:CueWindowsHostProcessTests> --expect-render-failure)

    # 子 Process を使う正常・異常終了の Test も時間制限を共有する
    set_tests_properties(Cue.WindowsHost.Process Cue.WindowsHost.AutoFrames Cue.WindowsHost.SingleThread
                         Cue.WindowsHost.RenderFailure PROPERTIES TIMEOUT 7)
endif()

add_test(NAME CueEngine.Smoke COMMAND CueEngine)

set_tests_properties(CueEngine.Smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "CueEngine smoke OK"
)

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
    add_executable(CueRuntimeHostTests
        Runtime/RuntimeHostTests.cpp
    )

    target_link_libraries(CueRuntimeHostTests PRIVATE Cue.RuntimeHost)

    add_test(NAME Cue.RuntimeHost.Lifecycle COMMAND CueRuntimeHostTests)

    set_tests_properties(Cue.RuntimeHost.Lifecycle PROPERTIES TIMEOUT 10)

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

    add_executable(CueWindowHostProcessTests
        Platform/WindowHostProcessTests.cpp
    )

    target_link_libraries(CueWindowHostProcessTests PRIVATE Cue.Platform.Windows user32)

    add_test(NAME Cue.WindowHost.Process COMMAND CueWindowHostProcessTests $<TARGET_FILE:CueWindowHost>)

    add_test(NAME Cue.WindowHost.AutoFrames COMMAND CueWindowHostProcessTests $<TARGET_FILE:CueWindowHost> --auto)

    add_test(NAME Cue.WindowHost.SingleThread COMMAND CueWindowHostProcessTests $<TARGET_FILE:CueWindowHost> --auto-single)

    set_tests_properties(Cue.WindowHost.AutoFrames PROPERTIES TIMEOUT 7)
    set_tests_properties(Cue.WindowHost.SingleThread PROPERTIES TIMEOUT 7)

    add_test(NAME Cue.RuntimeHost.Process COMMAND CueWindowHostProcessTests $<TARGET_FILE:CueRuntimeHost>)

    add_test(NAME Cue.RuntimeHost.AutoFrames COMMAND CueWindowHostProcessTests $<TARGET_FILE:CueRuntimeHost> --auto)

    add_test(NAME Cue.RuntimeHost.SingleThread COMMAND CueWindowHostProcessTests $<TARGET_FILE:CueRuntimeHost> --auto-single)

    add_test(NAME Cue.RuntimeHost.RenderFailure COMMAND CueWindowHostProcessTests $<TARGET_FILE:CueRuntimeHost> --expect-render-failure)

    set_tests_properties(Cue.RuntimeHost.Process Cue.RuntimeHost.AutoFrames Cue.RuntimeHost.SingleThread
                         Cue.RuntimeHost.RenderFailure PROPERTIES TIMEOUT 7)
endif()

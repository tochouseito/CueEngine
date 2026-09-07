get_target_property(cueIoLinks Cue.IO LINK_LIBRARIES)
get_target_property(cueIoInterfaceLinks Cue.IO INTERFACE_LINK_LIBRARIES)

if(NOT "${cueIoLinks}" STREQUAL "Cue.Foundation")
    message(FATAL_ERROR "Cue.IO must directly link only Cue.Foundation: ${cueIoLinks}")
endif()

if(NOT "${cueIoInterfaceLinks}" STREQUAL "Cue.Foundation")
    message(FATAL_ERROR "Cue.IO must expose only Cue.Foundation: ${cueIoInterfaceLinks}")
endif()

get_target_property(cueIoWindowsLinks Cue.IO.Windows LINK_LIBRARIES)

if(NOT "${cueIoWindowsLinks}" STREQUAL "Cue.IO;Cue.Foundation.Windows;bcrypt;Ole32;Shell32")
    message(FATAL_ERROR "Cue.IO.Windows must use only Cue.IO and Windows system dependencies: ${cueIoWindowsLinks}")
endif()

add_executable(CueIoTests IoTests.cpp)
target_link_libraries(CueIoTests PRIVATE Cue.IO.Windows)
target_compile_features(CueIoTests PRIVATE cxx_std_20)

add_executable(CueIoPublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueIoPublicHeaderTest PRIVATE Cue.IO.Windows)
target_compile_features(CueIoPublicHeaderTest PRIVATE cxx_std_20)

add_executable(CueWorkspaceCoreTests WorkspaceCoreTests.cpp)
target_link_libraries(CueWorkspaceCoreTests PRIVATE Cue.IO)
target_compile_features(CueWorkspaceCoreTests PRIVATE cxx_std_20)

add_executable(CueWindowsWorkspaceTests WindowsWorkspaceTests.cpp)
target_link_libraries(CueWindowsWorkspaceTests PRIVATE Cue.IO.Windows Advapi32)
target_compile_features(CueWindowsWorkspaceTests PRIVATE cxx_std_20)

add_executable(CueWindowsWorkspaceWatcherTests WindowsWorkspaceWatcherTests.cpp)
target_link_libraries(CueWindowsWorkspaceWatcherTests PRIVATE Cue.IO.Windows)
target_compile_features(CueWindowsWorkspaceWatcherTests PRIVATE cxx_std_20)

set(cueIoTestTargets
    CueIoTests
    CueIoPublicHeaderTest
    CueWorkspaceCoreTests
    CueWindowsWorkspaceTests
    CueWindowsWorkspaceWatcherTests
)

foreach(testTarget IN LISTS cueIoTestTargets)
    set_target_properties(
        ${testTarget}
        PROPERTIES
            CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )

    if(MSVC)
        target_compile_options(
            ${testTarget}
            PRIVATE
                /W4
                /WX
                /permissive-
                /Zc:__cplusplus
                /utf-8
                $<$<CONFIG:Development>:/O2>
                $<$<CONFIG:Development>:/Zi>
        )
        target_link_options(${testTarget} PRIVATE $<$<CONFIG:Development>:/DEBUG>)
    endif()
endforeach()

add_custom_target(Cue.IO.Tests DEPENDS ${cueIoTestTargets})

add_test(NAME Cue.IO.Storage COMMAND CueIoTests)
set_tests_properties(Cue.IO.Storage PROPERTIES LABELS "IO;Windows" TIMEOUT 30)

add_test(NAME Cue.IO.PublicHeaders COMMAND CueIoPublicHeaderTest)
set_tests_properties(Cue.IO.PublicHeaders PROPERTIES LABELS IO TIMEOUT 10)

add_test(NAME Cue.IO.WorkspaceCore COMMAND CueWorkspaceCoreTests)
set_tests_properties(Cue.IO.WorkspaceCore PROPERTIES LABELS IO TIMEOUT 10)

add_test(NAME Cue.IO.WindowsWorkspace COMMAND CueWindowsWorkspaceTests)
set_tests_properties(Cue.IO.WindowsWorkspace PROPERTIES LABELS "IO;Windows" TIMEOUT 30)

add_test(NAME Cue.IO.WindowsWorkspaceWatcher COMMAND CueWindowsWorkspaceWatcherTests)
set_tests_properties(Cue.IO.WindowsWorkspaceWatcher PROPERTIES LABELS "IO;Windows" TIMEOUT 30)

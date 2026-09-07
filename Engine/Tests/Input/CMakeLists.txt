get_target_property(cueInputLinks Cue.Input LINK_LIBRARIES)
get_target_property(cueInputInterfaceLinks Cue.Input INTERFACE_LINK_LIBRARIES)

if(cueInputLinks OR cueInputInterfaceLinks)
    message(FATAL_ERROR "Cue.Input must not depend on platform, rendering, editor, or third-party targets")
endif()

file(
    GLOB_RECURSE cueInputPublicHeaders
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Engine/Source/Input/Public/*.h"
)

foreach(inputHeader IN LISTS cueInputPublicHeaders)
    file(READ "${inputHeader}" inputHeaderContent)
    if(inputHeaderContent MATCHES "Windows\\.h|windowsx\\.h|HWND|WPARAM|LPARAM|VK_|D3D12|DXGI|ImGui|Editor")
        message(FATAL_ERROR "Portable Cue.Input header exposes a forbidden dependency: ${inputHeader}")
    endif()
endforeach()

add_executable(CueInputTests InputStateTests.cpp)
target_link_libraries(CueInputTests PRIVATE Cue.Input)
target_compile_features(CueInputTests PRIVATE cxx_std_20)

add_executable(CueInputPublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueInputPublicHeaderTest PRIVATE Cue.Input)
target_compile_features(CueInputPublicHeaderTest PRIVATE cxx_std_20)

set(cueInputTestTargets CueInputTests CueInputPublicHeaderTest)

if(WIN32)
    get_target_property(cueInputWindowsLinks Cue.Input.Windows LINK_LIBRARIES)
    get_target_property(cueInputWindowsInterfaceLinks Cue.Input.Windows INTERFACE_LINK_LIBRARIES)

    if(NOT "${cueInputWindowsLinks}" STREQUAL "Cue.Input;Cue.Platform.Windows;User32")
        message(FATAL_ERROR "Cue.Input.Windows has unexpected direct dependencies: ${cueInputWindowsLinks}")
    endif()
    if(NOT "${cueInputWindowsInterfaceLinks}" STREQUAL
       "Cue.Input;Cue.Platform.Windows;$<LINK_ONLY:User32>")
        message(FATAL_ERROR "Cue.Input.Windows exposes unexpected dependencies: ${cueInputWindowsInterfaceLinks}")
    endif()

    add_executable(CueInputWindowsPublicHeaderTest WindowsPublicHeaderCompile.cpp)
    target_link_libraries(CueInputWindowsPublicHeaderTest PRIVATE Cue.Input.Windows)
    target_compile_features(CueInputWindowsPublicHeaderTest PRIVATE cxx_std_20)

    add_executable(CueInputWindowsMessageSinkTests WindowsInputMessageSinkTests.cpp)
    target_link_libraries(CueInputWindowsMessageSinkTests PRIVATE Cue.Input.Windows)
    target_compile_features(CueInputWindowsMessageSinkTests PRIVATE cxx_std_20)

    list(APPEND cueInputTestTargets CueInputWindowsPublicHeaderTest CueInputWindowsMessageSinkTests)
endif()

foreach(testTarget IN LISTS cueInputTestTargets)
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

add_custom_target(Cue.Input.Tests DEPENDS ${cueInputTestTargets})

add_test(NAME Cue.Input.State COMMAND CueInputTests)
add_test(NAME Cue.Input.PublicHeaders COMMAND CueInputPublicHeaderTest)

if(WIN32)
    add_test(NAME Cue.Input.Windows.PublicHeaders COMMAND CueInputWindowsPublicHeaderTest)
    add_test(NAME Cue.Input.Windows.MessageSink COMMAND CueInputWindowsMessageSinkTests)
endif()

set_tests_properties(
    Cue.Input.State
    Cue.Input.PublicHeaders
    PROPERTIES
        LABELS Input
        TIMEOUT 10
)

if(WIN32)
    set_tests_properties(
        Cue.Input.Windows.PublicHeaders
        Cue.Input.Windows.MessageSink
        PROPERTIES
            LABELS Input
            TIMEOUT 10
    )
endif()

add_executable(CueRuntimeSceneSessionTests RuntimeSceneSessionTests.cpp)
target_link_libraries(CueRuntimeSceneSessionTests PRIVATE Cue.Runtime)
target_compile_features(CueRuntimeSceneSessionTests PRIVATE cxx_std_20)

add_executable(CueRuntimeSceneSessionProcessTests RuntimeSceneSessionProcessTests.cpp)
target_link_libraries(CueRuntimeSceneSessionProcessTests PRIVATE Cue.Runtime)
target_compile_features(CueRuntimeSceneSessionProcessTests PRIVATE cxx_std_20)
target_include_directories(
    CueRuntimeSceneSessionProcessTests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/Runtime/Private"
)

add_executable(CueRuntimePublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueRuntimePublicHeaderTest PRIVATE Cue.Runtime)
target_compile_features(CueRuntimePublicHeaderTest PRIVATE cxx_std_20)

set(cueRuntimeTestTargets CueRuntimeSceneSessionTests CueRuntimeSceneSessionProcessTests CueRuntimePublicHeaderTest)

foreach(testTarget IN LISTS cueRuntimeTestTargets)
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

add_custom_target(Cue.Runtime.Tests DEPENDS ${cueRuntimeTestTargets})

add_test(NAME Cue.Runtime.SceneSession COMMAND CueRuntimeSceneSessionTests)
add_test(NAME Cue.Runtime.PublicHeaders COMMAND CueRuntimePublicHeaderTest)

foreach(processMode IN ITEMS OuterEndFailure ReportEndFailure WrongThread LiveDestructor)
    add_test(
        NAME Cue.Runtime.Process.${processMode}
        COMMAND
            ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeSceneSessionProcessTests>
            -DTEST_MODE=${processMode}
            -DEXPECTED_EXIT_CODE=75
            -P ${CMAKE_CURRENT_SOURCE_DIR}/../Scene/VerifyProcessExit.cmake
    )
endforeach()

set_tests_properties(
    Cue.Runtime.SceneSession
    Cue.Runtime.PublicHeaders
    Cue.Runtime.Process.OuterEndFailure
    Cue.Runtime.Process.ReportEndFailure
    Cue.Runtime.Process.WrongThread
    Cue.Runtime.Process.LiveDestructor
    PROPERTIES
        LABELS Runtime
        TIMEOUT 10
)

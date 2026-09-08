add_executable(CueGameCoreTests GameCoreTests.cpp)
target_link_libraries(CueGameCoreTests PRIVATE Cue.GameCore)
target_compile_features(CueGameCoreTests PRIVATE cxx_std_20)

add_executable(CueGameCorePublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueGameCorePublicHeaderTest PRIVATE Cue.GameCore)
target_compile_features(CueGameCorePublicHeaderTest PRIVATE cxx_std_20)

add_executable(CueGameCoreProcessTests GameCoreProcessTests.cpp)
target_link_libraries(CueGameCoreProcessTests PRIVATE Cue.GameCore)
target_compile_features(CueGameCoreProcessTests PRIVATE cxx_std_20)

add_executable(CueGameCoreClockTests ClockTests.cpp)
target_link_libraries(CueGameCoreClockTests PRIVATE Cue.GameCore)
target_compile_features(CueGameCoreClockTests PRIVATE cxx_std_20)
target_include_directories(
    CueGameCoreClockTests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/GameCore/Private"
)

add_executable(CueGameCoreRuntimeSystemRegistryTests RuntimeSystemRegistryTests.cpp)
target_link_libraries(CueGameCoreRuntimeSystemRegistryTests PRIVATE Cue.GameCore Cue.Input)
target_compile_features(CueGameCoreRuntimeSystemRegistryTests PRIVATE cxx_std_20)

set(
    cueGameCoreTestTargets
    CueGameCoreTests
    CueGameCorePublicHeaderTest
    CueGameCoreProcessTests
    CueGameCoreClockTests
    CueGameCoreRuntimeSystemRegistryTests
)

foreach(testTarget IN LISTS cueGameCoreTestTargets)
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

add_custom_target(Cue.GameCore.Tests DEPENDS ${cueGameCoreTestTargets})

add_test(NAME Cue.GameCore.Storage COMMAND CueGameCoreTests)
add_test(NAME Cue.GameCore.PublicHeaders COMMAND CueGameCorePublicHeaderTest)
add_test(NAME Cue.GameCore.Clock COMMAND CueGameCoreClockTests)
add_test(NAME Cue.GameCore.RuntimeSystemRegistry COMMAND CueGameCoreRuntimeSystemRegistryTests)
foreach(processMode IN ITEMS
        StructuralReentry
        QueryMutation
        NestedQuery
        NestedEmptyQuery
        QueryDestruction
        QueryException
        RuntimeStopWrongThread
        RuntimeSystemRegistryWrongThread
        RuntimeSystemRegistryCallbackDestruction
)
    add_test(
        NAME Cue.GameCore.Process.${processMode}
        COMMAND
            ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:CueGameCoreProcessTests>
            -DTEST_MODE=${processMode}
            -DEXPECTED_EXIT_CODE=42
            -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyProcessExit.cmake
    )
endforeach()

add_test(
    NAME Cue.GameCore.Process.WrongThread
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueGameCoreProcessTests>
        -DTEST_MODE=WrongThread
        -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,42>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyProcessExit.cmake
)

add_test(
    NAME Cue.GameCore.Process.HeadlessRuntimeWorld
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueGameCoreProcessTests>
        -DTEST_MODE=HeadlessRuntimeWorld
        -DEXPECTED_EXIT_CODE=0
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyProcessExit.cmake
)

set_tests_properties(
    Cue.GameCore.Storage
    Cue.GameCore.PublicHeaders
    Cue.GameCore.Clock
    Cue.GameCore.RuntimeSystemRegistry
    Cue.GameCore.Process.StructuralReentry
    Cue.GameCore.Process.QueryMutation
    Cue.GameCore.Process.NestedQuery
    Cue.GameCore.Process.NestedEmptyQuery
    Cue.GameCore.Process.QueryDestruction
    Cue.GameCore.Process.QueryException
    Cue.GameCore.Process.RuntimeStopWrongThread
    Cue.GameCore.Process.RuntimeSystemRegistryWrongThread
    Cue.GameCore.Process.RuntimeSystemRegistryCallbackDestruction
    Cue.GameCore.Process.WrongThread
    Cue.GameCore.Process.HeadlessRuntimeWorld
    PROPERTIES
        LABELS GameCore
        TIMEOUT 30
)

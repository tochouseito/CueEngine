add_executable(CueBuildToolchainTests ToolchainTests.cpp)
target_link_libraries(CueBuildToolchainTests PRIVATE Cue.Build)
target_compile_features(CueBuildToolchainTests PRIVATE cxx_std_20)

add_executable(CueBuildPlanTests PlanTests.cpp)
target_link_libraries(CueBuildPlanTests PRIVATE Cue.Build)
target_compile_features(CueBuildPlanTests PRIVATE cxx_std_20)

add_executable(CueBuildCMakeRunnerTests CMakeRunnerTests.cpp)
target_link_libraries(CueBuildCMakeRunnerTests PRIVATE Cue.Build)
target_compile_features(CueBuildCMakeRunnerTests PRIVATE cxx_std_20)

add_executable(CueBuildServiceTests ServiceTests.cpp)
target_link_libraries(CueBuildServiceTests PRIVATE Cue.Build)
target_compile_features(CueBuildServiceTests PRIVATE cxx_std_20)

add_executable(CueBuildDiagnosticBundleTests DiagnosticBundleTests.cpp)
target_link_libraries(CueBuildDiagnosticBundleTests PRIVATE Cue.Build)
target_compile_features(CueBuildDiagnosticBundleTests PRIVATE cxx_std_20)

add_executable(CueBuildPublicHeaderTest CMakeRunnerHeaderCompile.cpp PlanHeaderCompile.cpp PublicHeaderCompile.cpp
                                        ServiceHeaderCompile.cpp)
target_link_libraries(CueBuildPublicHeaderTest PRIVATE Cue.Build)
target_compile_features(CueBuildPublicHeaderTest PRIVATE cxx_std_20)

set(cueBuildTestTargets CueBuildToolchainTests CueBuildPlanTests CueBuildCMakeRunnerTests CueBuildServiceTests
                        CueBuildDiagnosticBundleTests CueBuildPublicHeaderTest)
if(WIN32)
    add_library(CueBuildArtifactProbe SHARED ArtifactProbe.cpp)
    target_link_libraries(CueBuildArtifactProbe PRIVATE Cue.GameModule.Abi)
    target_compile_features(CueBuildArtifactProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildArtifactProbe
        PRIVATE
            CUE_GAME_MODULE_BUILD=1
            $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
            $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
            $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
    )

    add_executable(CueBuildCMakeRunnerProbe CMakeRunnerProbe.cpp)
    target_compile_features(CueBuildCMakeRunnerProbe PRIVATE cxx_std_20)

    add_executable(CueBuildCMakeRunnerProcessTests CMakeRunnerProcessTests.cpp)
    target_link_libraries(CueBuildCMakeRunnerProcessTests PRIVATE Cue.Build Cue.Platform.Windows)
    target_compile_features(CueBuildCMakeRunnerProcessTests PRIVATE cxx_std_20)

    add_executable(CueBuildWindowsToolchainTests WindowsToolchainTests.cpp)
    target_link_libraries(CueBuildWindowsToolchainTests PRIVATE Cue.Build.Windows)
    target_compile_features(CueBuildWindowsToolchainTests PRIVATE cxx_std_20)

    add_executable(CueBuildWindowsPublicHeaderTest WindowsPublicHeaderCompile.cpp)
    target_link_libraries(CueBuildWindowsPublicHeaderTest PRIVATE Cue.Build.Windows)
    target_compile_features(CueBuildWindowsPublicHeaderTest PRIVATE cxx_std_20)

    add_executable(CueBuildWindowsArtifactPublisherTests WindowsArtifactPublisherTests.cpp)
    target_link_libraries(CueBuildWindowsArtifactPublisherTests PRIVATE Cue.Build.Windows Cue.Project)
    target_compile_features(CueBuildWindowsArtifactPublisherTests PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildWindowsArtifactPublisherTests
        PRIVATE
            $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
            $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
            $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
    )
    list(
        APPEND
        cueBuildTestTargets
        CueBuildCMakeRunnerProbe
        CueBuildCMakeRunnerProcessTests
        CueBuildWindowsToolchainTests
        CueBuildWindowsPublicHeaderTest
        CueBuildArtifactProbe
        CueBuildWindowsArtifactPublisherTests
    )
endif()

foreach(testTarget IN LISTS cueBuildTestTargets)
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

add_custom_target(Cue.Build.Tests DEPENDS ${cueBuildTestTargets})

add_test(NAME Cue.Build.Toolchain COMMAND CueBuildToolchainTests)
set_tests_properties(Cue.Build.Toolchain PROPERTIES LABELS Build TIMEOUT 20)

add_test(NAME Cue.Build.Plan COMMAND CueBuildPlanTests $<TARGET_FILE_DIR:CueBuildPlanTests>)
set_tests_properties(Cue.Build.Plan PROPERTIES LABELS Build TIMEOUT 20)

add_test(NAME Cue.Build.CMakeRunner COMMAND CueBuildCMakeRunnerTests)
set_tests_properties(Cue.Build.CMakeRunner PROPERTIES LABELS Build TIMEOUT 20)

add_test(NAME Cue.Build.Service COMMAND CueBuildServiceTests)
set_tests_properties(Cue.Build.Service PROPERTIES LABELS Build TIMEOUT 30)

add_test(NAME Cue.Build.DiagnosticBundle COMMAND CueBuildDiagnosticBundleTests $<TARGET_FILE_DIR:CueBuildDiagnosticBundleTests>)
set_tests_properties(Cue.Build.DiagnosticBundle PROPERTIES LABELS Build TIMEOUT 30)

add_test(NAME Cue.Build.PublicHeaders COMMAND CueBuildPublicHeaderTest)
set_tests_properties(Cue.Build.PublicHeaders PROPERTIES LABELS Build TIMEOUT 10)

if(WIN32)
    add_test(NAME Cue.Build.CMakeRunner.Process
             COMMAND CueBuildCMakeRunnerProcessTests $<TARGET_FILE:CueBuildCMakeRunnerProbe>)
    set_tests_properties(Cue.Build.CMakeRunner.Process PROPERTIES LABELS "Build;Windows" TIMEOUT 30)

    add_test(NAME Cue.Build.Windows.Toolchain COMMAND CueBuildWindowsToolchainTests)
    set_tests_properties(Cue.Build.Windows.Toolchain PROPERTIES LABELS "Build;Windows" TIMEOUT 20)

    add_test(NAME Cue.Build.Windows.PublicHeaders COMMAND CueBuildWindowsPublicHeaderTest)
    set_tests_properties(Cue.Build.Windows.PublicHeaders PROPERTIES LABELS "Build;Windows" TIMEOUT 10)

    add_test(NAME Cue.Build.Windows.ArtifactPublisher
             COMMAND CueBuildWindowsArtifactPublisherTests $<TARGET_FILE:CueBuildArtifactProbe>)
    set_tests_properties(Cue.Build.Windows.ArtifactPublisher PROPERTIES LABELS "Build;Windows" TIMEOUT 30)
endif()

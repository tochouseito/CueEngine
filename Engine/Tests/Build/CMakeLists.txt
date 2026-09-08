add_executable(CueBuildToolchainTests ToolchainTests.cpp)
target_link_libraries(CueBuildToolchainTests PRIVATE Cue.Build)
target_compile_features(CueBuildToolchainTests PRIVATE cxx_std_20)

add_executable(CueBuildPublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueBuildPublicHeaderTest PRIVATE Cue.Build)
target_compile_features(CueBuildPublicHeaderTest PRIVATE cxx_std_20)

set(cueBuildTestTargets CueBuildToolchainTests CueBuildPublicHeaderTest)
if(WIN32)
    add_executable(CueBuildWindowsToolchainTests WindowsToolchainTests.cpp)
    target_link_libraries(CueBuildWindowsToolchainTests PRIVATE Cue.Build.Windows)
    target_compile_features(CueBuildWindowsToolchainTests PRIVATE cxx_std_20)
    list(APPEND cueBuildTestTargets CueBuildWindowsToolchainTests)
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

add_test(NAME Cue.Build.PublicHeaders COMMAND CueBuildPublicHeaderTest)
set_tests_properties(Cue.Build.PublicHeaders PROPERTIES LABELS Build TIMEOUT 10)

if(WIN32)
    add_test(NAME Cue.Build.Windows.Toolchain COMMAND CueBuildWindowsToolchainTests)
    set_tests_properties(Cue.Build.Windows.Toolchain PROPERTIES LABELS "Build;Windows" TIMEOUT 20)
endif()

add_executable(CueImGuiDockingContractTests)

target_sources(
    CueImGuiDockingContractTests
    PRIVATE
        DockingContractTests.cpp
)

target_link_libraries(CueImGuiDockingContractTests PRIVATE Cue.ImGui.Core)
target_compile_features(CueImGuiDockingContractTests PRIVATE cxx_std_20)

set_target_properties(
    CueImGuiDockingContractTests
    PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

if(MSVC)
    target_compile_options(
        CueImGuiDockingContractTests
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
    )
endif()

add_test(NAME Cue.ImGui.DockingContract COMMAND CueImGuiDockingContractTests)
set_tests_properties(Cue.ImGui.DockingContract PROPERTIES LABELS "ImGui")

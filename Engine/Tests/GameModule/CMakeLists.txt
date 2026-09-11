add_executable(CueGameModuleAbiCHeaderTest GameModuleAbiCCompile.c)
target_link_libraries(CueGameModuleAbiCHeaderTest PRIVATE Cue.GameModule.Abi)
target_compile_features(CueGameModuleAbiCHeaderTest PRIVATE c_std_11)

add_executable(CueGameModuleAbiCppHeaderTest GameModuleAbiCppCompile.cpp)
target_link_libraries(CueGameModuleAbiCppHeaderTest PRIVATE Cue.GameModule.Abi)
target_compile_features(CueGameModuleAbiCppHeaderTest PRIVATE cxx_std_20)

add_executable(CueGameModuleAbiStaticCHeaderTest GameModuleAbiCCompile.c)
target_link_libraries(CueGameModuleAbiStaticCHeaderTest PRIVATE Cue.GameModule.Abi)
target_compile_features(CueGameModuleAbiStaticCHeaderTest PRIVATE c_std_11)
target_compile_definitions(CueGameModuleAbiStaticCHeaderTest PRIVATE CUE_GAME_MODULE_STATIC=1)

add_executable(CueGameModuleAbiStaticCppHeaderTest GameModuleAbiCppCompile.cpp)
target_link_libraries(CueGameModuleAbiStaticCppHeaderTest PRIVATE Cue.GameModule.Abi)
target_compile_features(CueGameModuleAbiStaticCppHeaderTest PRIVATE cxx_std_20)
target_compile_definitions(CueGameModuleAbiStaticCppHeaderTest PRIVATE CUE_GAME_MODULE_STATIC=1)

set(cueGameModuleTestTargets CueGameModuleAbiCHeaderTest CueGameModuleAbiCppHeaderTest
                             CueGameModuleAbiStaticCHeaderTest CueGameModuleAbiStaticCppHeaderTest)
foreach(testTarget IN LISTS cueGameModuleTestTargets)
    set_target_properties(
        ${testTarget}
        PROPERTIES
            C_EXTENSIONS OFF
            CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
    if(MSVC)
        target_compile_options(${testTarget} PRIVATE /W4 /WX /utf-8)
    endif()
endforeach()

if(MSVC)
    target_compile_options(CueGameModuleAbiCppHeaderTest PRIVATE /permissive- /Zc:__cplusplus)
endif()

add_custom_target(Cue.GameModule.Tests DEPENDS ${cueGameModuleTestTargets})

add_test(NAME Cue.GameModule.Abi.CHeader COMMAND CueGameModuleAbiCHeaderTest)
set_tests_properties(Cue.GameModule.Abi.CHeader PROPERTIES LABELS GameModule TIMEOUT 10)

add_test(NAME Cue.GameModule.Abi.CppHeader COMMAND CueGameModuleAbiCppHeaderTest)
set_tests_properties(Cue.GameModule.Abi.CppHeader PROPERTIES LABELS GameModule TIMEOUT 10)

add_test(NAME Cue.GameModule.Abi.StaticCHeader COMMAND CueGameModuleAbiStaticCHeaderTest)
set_tests_properties(Cue.GameModule.Abi.StaticCHeader PROPERTIES LABELS GameModule TIMEOUT 10)

add_test(NAME Cue.GameModule.Abi.StaticCppHeader COMMAND CueGameModuleAbiStaticCppHeaderTest)
set_tests_properties(Cue.GameModule.Abi.StaticCppHeader PROPERTIES LABELS GameModule TIMEOUT 10)

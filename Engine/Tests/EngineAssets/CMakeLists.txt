add_executable(CueEngineAssetsTests BuiltInMeshTests.cpp)
target_link_libraries(CueEngineAssetsTests PRIVATE Cue.EngineAssets)
target_compile_features(CueEngineAssetsTests PRIVATE cxx_std_20)

add_executable(CueEngineAssetsPublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueEngineAssetsPublicHeaderTest PRIVATE Cue.EngineAssets)
target_compile_features(CueEngineAssetsPublicHeaderTest PRIVATE cxx_std_20)

set(cueEngineAssetsTestTargets CueEngineAssetsTests CueEngineAssetsPublicHeaderTest)

foreach(testTarget IN LISTS cueEngineAssetsTestTargets)
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

add_custom_target(Cue.EngineAssets.Tests DEPENDS ${cueEngineAssetsTestTargets})

add_test(NAME Cue.EngineAssets.CubeMesh COMMAND CueEngineAssetsTests)
add_test(NAME Cue.EngineAssets.PublicHeaders COMMAND CueEngineAssetsPublicHeaderTest)

set_tests_properties(
    Cue.EngineAssets.CubeMesh
    Cue.EngineAssets.PublicHeaders
    PROPERTIES
        LABELS EngineAssets
        TIMEOUT 10
)

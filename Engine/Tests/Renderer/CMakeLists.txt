add_executable(CueRendererTests RendererTests.cpp)
target_link_libraries(CueRendererTests PRIVATE Cue.Renderer)
target_compile_features(CueRendererTests PRIVATE cxx_std_20)

add_executable(CueRendererPublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueRendererPublicHeaderTest PRIVATE Cue.Renderer)
target_compile_features(CueRendererPublicHeaderTest PRIVATE cxx_std_20)

set(cueRendererTestTargets CueRendererTests CueRendererPublicHeaderTest)

foreach(testTarget IN LISTS cueRendererTestTargets)
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

add_custom_target(Cue.Renderer.Tests DEPENDS ${cueRendererTestTargets})

add_test(NAME Cue.Renderer.Unit COMMAND CueRendererTests)
add_test(NAME Cue.Renderer.PublicHeaders COMMAND CueRendererPublicHeaderTest)

set_tests_properties(
    Cue.Renderer.Unit
    Cue.Renderer.PublicHeaders
    PROPERTIES
        LABELS Renderer
        TIMEOUT 10
)

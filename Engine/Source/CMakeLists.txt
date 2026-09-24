add_executable(CueEngine
    Smoke/Main.cpp
)

set_target_properties(CueEngine PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

if(MSVC)
    target_compile_options(CueEngine PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )

    target_link_options(CueEngine PRIVATE
        $<$<CONFIG:Development>:/DEBUG>
    )
endif()

target_compile_definitions(CueEngine PRIVATE
    $<$<CONFIG:Release>:CUE_SHIPPING=1>
)

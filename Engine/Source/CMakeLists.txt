add_executable(CueEngine
    Smoke/Main.cpp
)

add_subdirectory(Foundation)
add_subdirectory(Platform)
add_subdirectory(Runtime/Frame)

if(WIN32)
    add_executable(CueWindowHost WIN32
        WindowHost/Main.cpp
        WindowHost/WindowHost.cpp
        WindowHost/WindowHost.h
    )

    target_link_libraries(CueWindowHost PRIVATE Cue.Platform.Windows Cue.Runtime.Frame)

    set_target_properties(CueWindowHost PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )

    if(MSVC)
        target_compile_options(CueWindowHost PRIVATE
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
        )

        target_link_options(CueWindowHost PRIVATE
            $<$<CONFIG:Development>:/DEBUG>
        )
    endif()

    target_compile_definitions(CueWindowHost PRIVATE
        $<$<CONFIG:Release>:CUE_SHIPPING=1>
    )
endif()

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

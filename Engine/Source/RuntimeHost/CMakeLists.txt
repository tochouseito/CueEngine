add_library(Cue.RuntimeHost STATIC
    Private/RuntimeHost.cpp
)

target_include_directories(Cue.RuntimeHost PUBLIC
    Public
)

target_link_libraries(Cue.RuntimeHost
    PUBLIC Cue.Runtime.Frame Cue.Platform
    PRIVATE Cue.Platform.Windows
)

target_compile_features(Cue.RuntimeHost PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.RuntimeHost PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

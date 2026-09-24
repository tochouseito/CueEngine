add_library(Cue.Platform.Windows STATIC
    Private/WindowsPlatform.cpp
)

target_include_directories(Cue.Platform.Windows PUBLIC
    Public
)

target_link_libraries(Cue.Platform.Windows PUBLIC
    Cue.Platform
)

target_link_libraries(Cue.Platform.Windows PRIVATE
    Cue.Foundation.Windows
)

target_compile_features(Cue.Platform.Windows PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Platform.Windows PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

target_compile_definitions(Cue.Platform.Windows PRIVATE
    $<$<CONFIG:Release>:CUE_SHIPPING=1>
)

target_link_libraries(Cue.Platform.Windows PRIVATE user32)

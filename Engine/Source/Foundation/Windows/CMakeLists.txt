add_library(Cue.Foundation.Windows STATIC
    Private/UtfConversion.cpp
)

target_include_directories(Cue.Foundation.Windows PUBLIC
    Public
)

target_link_libraries(Cue.Foundation.Windows PUBLIC Cue.Foundation)

target_compile_features(Cue.Foundation.Windows PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Foundation.Windows PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

target_compile_definitions(Cue.Foundation.Windows PRIVATE
    $<$<CONFIG:Release>:CUE_SHIPPING=1>
)

# FrameController は Platform の抽象 Clock、Waiter、Thread を借用する
add_library(Cue.Runtime.Frame STATIC
    Private/FrameController.cpp
)

target_include_directories(Cue.Runtime.Frame PUBLIC
    Public
)

target_link_libraries(Cue.Runtime.Frame PUBLIC
    Cue.Platform
)

target_compile_features(Cue.Runtime.Frame PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Runtime.Frame PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

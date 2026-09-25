# Frame 制御は Runtime 本体から独立した契約と実装を持つ
add_subdirectory(Frame)

add_library(Cue.Runtime STATIC
    Private/Runtime.cpp
)

target_include_directories(Cue.Runtime PUBLIC
    Public
)

target_link_libraries(Cue.Runtime PUBLIC Cue.Runtime.Frame)

target_compile_features(Cue.Runtime PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Runtime PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

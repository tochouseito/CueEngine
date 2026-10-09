# File I/O の共通処理には Win32 の実装を含めない
add_library(Cue.Platform STATIC Private/FileSystem.cpp)

target_include_directories(Cue.Platform PUBLIC
    Public
)

target_link_libraries(Cue.Platform PUBLIC Cue.Foundation)

if(MSVC)
    target_compile_options(Cue.Platform PRIVATE
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

# 具体的な Window と Thread Service は Windows Target で提供する
if(WIN32)
    add_subdirectory(Windows)
endif()

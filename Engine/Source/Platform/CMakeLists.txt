# Runtime が参照する Platform 契約に Win32 の実装を含めない
add_library(Cue.Platform INTERFACE)

target_include_directories(Cue.Platform INTERFACE
    Public
)

target_link_libraries(Cue.Platform INTERFACE Cue.Foundation)

# 具体的な Window と Thread Service は Windows Target で提供する
if(WIN32)
    add_subdirectory(Windows)
endif()

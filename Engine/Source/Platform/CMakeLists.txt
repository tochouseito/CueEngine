add_library(Cue.Platform INTERFACE)

target_include_directories(Cue.Platform INTERFACE
    Public
)

target_link_libraries(Cue.Platform INTERFACE Cue.Foundation)

if(WIN32)
    add_subdirectory(Windows)
endif()

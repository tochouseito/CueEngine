add_library(Cue.Foundation INTERFACE)

target_include_directories(Cue.Foundation INTERFACE
    Public
)

target_compile_features(Cue.Foundation INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Foundation INTERFACE /utf-8)
endif()

if(WIN32)
    add_subdirectory(Windows)
endif()

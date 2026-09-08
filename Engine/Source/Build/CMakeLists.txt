add_library(Cue.Build STATIC)

target_sources(
    Cue.Build
    PRIVATE
        Private/CMakeRunner.cpp
        Private/Plan.cpp
        Private/Toolchain.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Build/CMakeRunner.h
            Public/Cue/Build/Plan.h
            Public/Cue/Build/Toolchain.h
)

target_link_libraries(Cue.Build PUBLIC Cue.Foundation Cue.Platform)
target_compile_features(Cue.Build PUBLIC cxx_std_20)
target_include_directories(Cue.Build PUBLIC Public)

set_target_properties(
    Cue.Build
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Build
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Build PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

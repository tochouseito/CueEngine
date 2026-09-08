if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio ([0-9]+) ")
    message(FATAL_ERROR "Cue.Build.Windows requires a Visual Studio generator")
endif()
set(cueBuildVisualStudioMajor "${CMAKE_MATCH_1}")

function(cue_escape_cpp_string output input)
    string(REPLACE "\\" "\\\\" escaped "${input}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    set(${output} "${escaped}" PARENT_SCOPE)
endfunction()

cue_escape_cpp_string(cueBuildCMakeCommand "${CMAKE_COMMAND}")
cue_escape_cpp_string(cueBuildCMakeMinimumVersion "${CMAKE_MINIMUM_REQUIRED_VERSION}")
cue_escape_cpp_string(cueBuildVisualStudioRoot "${CMAKE_GENERATOR_INSTANCE}")
cue_escape_cpp_string(cueBuildMsvcCompiler "${CMAKE_CXX_COMPILER}")
cue_escape_cpp_string(cueBuildWindowsSdkVersion "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
cue_escape_cpp_string(cueBuildEngineSourceRoot "${CMAKE_SOURCE_DIR}")
cue_escape_cpp_string(cueBuildEngineBinaryRoot "${CMAKE_BINARY_DIR}")

configure_file(
    Private/EngineBuildMetadata.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/Generated/EngineBuildMetadata.h"
    @ONLY
)

add_library(Cue.Build.Windows STATIC)

target_sources(
    Cue.Build.Windows
    PRIVATE
        Private/WindowsToolchain.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Build/Windows/WindowsToolchain.h
)

target_link_libraries(
    Cue.Build.Windows
    PUBLIC
        Cue.Build
    PRIVATE
        Cue.Foundation.Windows
        advapi32
        version
)
target_compile_features(Cue.Build.Windows PUBLIC cxx_std_20)
target_include_directories(
    Cue.Build.Windows
    PUBLIC
        Public
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/Generated"
)
target_compile_definitions(Cue.Build.Windows PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties(
    Cue.Build.Windows
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Build.Windows
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Build.Windows PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

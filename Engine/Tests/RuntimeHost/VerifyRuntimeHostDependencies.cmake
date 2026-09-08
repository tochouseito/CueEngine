cmake_minimum_required(VERSION 4.2.0)

include("${CMAKE_CURRENT_LIST_DIR}/../CMake/CueDependencyVerification.cmake")

if(NOT EXISTS "${REPORT_FILE}")
    message(FATAL_ERROR "Runtime Host dependency report does not exist: ${REPORT_FILE}")
endif()

file(STRINGS "${REPORT_FILE}" dependencyReportLines)

foreach(
    requiredLine
    IN ITEMS
        "CueRuntimeHost LINK_LIBRARIES: Cue.Foundation;Cue.GameCore;Cue.Input.Windows;Cue.Platform.Windows;Cue.RHI.D3D12.Windows;Cue.Runtime;Cue.Scene;Cue.Schema;Cue.Platform.Windows.TestSupport"
        "Allowed direct dependencies: Cue.Foundation;Cue.GameCore;Cue.Input.Windows;Cue.Platform.Windows;Cue.RHI.D3D12.Windows;Cue.Runtime;Cue.Scene;Cue.Schema"
        "Testing-only direct dependency: Cue.Platform.Windows.TestSupport"
        "Forbidden source dependencies: WindowsSDK;D3D12NativeTypes;Renderer;Editor;ProjectFiles;IO;ECS"
)
    cue_require_report_line(
        dependencyReportLines
        "${requiredLine}"
        "Runtime Host dependency report is missing an exact line: "
    )
endforeach()

file(
    GLOB_RECURSE
    runtimeHostSources
    LIST_DIRECTORIES FALSE
    "${RUNTIME_HOST_SOURCE_DIR}/*.c"
    "${RUNTIME_HOST_SOURCE_DIR}/*.cc"
    "${RUNTIME_HOST_SOURCE_DIR}/*.cpp"
    "${RUNTIME_HOST_SOURCE_DIR}/*.cxx"
    "${RUNTIME_HOST_SOURCE_DIR}/*.h"
    "${RUNTIME_HOST_SOURCE_DIR}/*.hh"
    "${RUNTIME_HOST_SOURCE_DIR}/*.hpp"
    "${RUNTIME_HOST_SOURCE_DIR}/*.hxx"
)

foreach(runtimeHostSource IN LISTS runtimeHostSources)
    file(READ "${runtimeHostSource}" sourceContents)
    string(
        REGEX MATCH
        "Windows\\.h|WideCharToMultiByte|MultiByteToWideChar|d3d12\\.h|dxgi[0-9_]*\\.h|ID3D12|IDXGI|D3D12_|DXGI_|DirectX|Renderer|Editor|ProjectFiles|Cue/IO|Cue/ECS"
        forbiddenDependency
        "${sourceContents}"
    )

    if(forbiddenDependency)
        message(FATAL_ERROR "Runtime Host source contains a forbidden dependency: ${runtimeHostSource}")
    endif()
endforeach()

message(STATUS "CueRuntimeHost dependency direction: passed")

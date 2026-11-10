cmake_minimum_required(VERSION 4.2.0)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/CueGameSolution.cmake")
project("${CUE_SOLUTION_NAME}" LANGUAGES CXX)

include("${PROJECT_SOURCE_DIR}/cmake/CueVersion.cmake")

# Debug は Debug Runtime、Development と Release は Release Runtime を使う
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    # 各 C++ Target の Source を MSVC で並列 Compile する
    add_compile_options(/MP)
endif()

# Test の有無を BUILD_TESTING で切り替え、Source の構成は共通に保つ
include(CTest)

# 手動の性能調査 Tool は通常 Build から分離し、明示した構成だけに追加する
option(CUE_BUILD_PERFORMANCE_TOOLS "Build manual performance investigation tools" OFF)
if(CUE_BUILD_PERFORMANCE_TOOLS AND NOT BUILD_TESTING)
    message(FATAL_ERROR "CUE_BUILD_PERFORMANCE_TOOLS requires BUILD_TESTING=ON")
endif()

add_subdirectory(Engine/Source)

include("${PROJECT_SOURCE_DIR}/cmake/CueHub.cmake")

if(BUILD_TESTING)
    add_subdirectory(Engine/Tests)
endif()

cue_set_game_solution_startup()

# Executable の出力 Directory を集め、並列 Build でも同じ DLL を競合して Copy しない
function(cue_collect_pix_outputs a_directory)
    get_property(taskTargets DIRECTORY "${a_directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(taskTarget IN LISTS taskTargets)
        get_target_property(taskType ${taskTarget} TYPE)
        if(taskType STREQUAL "EXECUTABLE")
            add_dependencies(${taskTarget} Cue.PixRuntime)
            get_target_property(taskOutput ${taskTarget} RUNTIME_OUTPUT_DIRECTORY)
            if(NOT taskOutput)
                get_target_property(taskBinary ${taskTarget} BINARY_DIR)
                set(taskOutput "${taskBinary}/$<CONFIG>")
            endif()
            set_property(GLOBAL APPEND PROPERTY CUE_PIX_OUTPUT_DIRECTORIES "${taskOutput}")
        endif()
    endforeach()
    get_property(taskChildren DIRECTORY "${a_directory}" PROPERTY SUBDIRECTORIES)
    foreach(taskChild IN LISTS taskChildren)
        cue_collect_pix_outputs("${taskChild}")
    endforeach()
endfunction()
# DX12 を構成しない Platform では Windows 固有の配布 Target を追加しない
if(TARGET Cue.WinPixEventRuntime)
    add_custom_target(Cue.PixRuntime ALL)
    cue_collect_pix_outputs("${PROJECT_SOURCE_DIR}")
    get_property(taskPixOutputs GLOBAL PROPERTY CUE_PIX_OUTPUT_DIRECTORIES)
    list(REMOVE_DUPLICATES taskPixOutputs)
    foreach(taskOutput IN LISTS taskPixOutputs)
        add_custom_command(TARGET Cue.PixRuntime POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                "-DCUE_PIX_CONFIGURATION=$<CONFIG>"
                "-DCUE_PIX_SOURCE=$<TARGET_FILE:Cue.WinPixEventRuntime>"
                "-DCUE_PIX_LICENSE_DIRECTORY=${PROJECT_SOURCE_DIR}/ThirdParty/Licenses"
                "-DCUE_PIX_OUTPUT_DIRECTORY=${taskOutput}"
                -P "${PROJECT_SOURCE_DIR}/cmake/CopyPixRuntime.cmake"
            VERBATIM
        )
    endforeach()
endif()

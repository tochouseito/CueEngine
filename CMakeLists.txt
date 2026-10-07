cmake_minimum_required(VERSION 4.2.0)

project(CueEngine LANGUAGES CXX)

# Debug は Debug Runtime、Development と Release は Release Runtime を使う
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    # 各 C++ Target の Source を MSVC で並列 Compile する
    add_compile_options(/MP)
endif()

# Test の有無を BUILD_TESTING で切り替え、Source の構成は共通に保つ
include(CTest)

add_subdirectory(Engine/Source)

if(BUILD_TESTING)
    add_subdirectory(Engine/Tests)
endif()

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
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${taskOutput}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:Cue.WinPixEventRuntime>" "${taskOutput}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${PROJECT_SOURCE_DIR}/ThirdParty/Licenses/WinPixEventRuntime-MIT.txt"
                "${PROJECT_SOURCE_DIR}/ThirdParty/Licenses/WinPixEventRuntime-ThirdPartyNotices.txt"
                "${taskOutput}"
            VERBATIM
        )
    endforeach()
endif()

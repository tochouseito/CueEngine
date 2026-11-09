include_guard(GLOBAL)

# ゲームの Root から明示的に選んだ SDK だけを参照し、Engine の開発用 Root は読み込まない
option(CUE_GAME_BUILD_PLAYER "Include the standalone game application and runtime SDK" OFF)

# Source-backed SDK の限定した依存を追加し、ゲームを Configure と IDE の Root に保つ
function(cue_add_standalone_game_project a_file)
    get_filename_component(taskSdkRoot "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    if(NOT DEFINED CUE_ENGINE_ROOT OR NOT IS_ABSOLUTE "${CUE_ENGINE_ROOT}")
        message(FATAL_ERROR "An explicit absolute CUE_ENGINE_ROOT is required")
    endif()
    get_filename_component(taskRequestedRoot "${CUE_ENGINE_ROOT}" ABSOLUTE)
    string(TOLOWER "${taskSdkRoot}" taskExpectedRoot)
    string(TOLOWER "${taskRequestedRoot}" taskActualRoot)
    if(NOT taskActualRoot STREQUAL taskExpectedRoot OR TARGET Cue.Script)
        message(FATAL_ERROR "The selected CueEngine SDK does not match its CMake entry point or is already loaded")
    endif()
    get_filename_component(taskProject "${a_file}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    get_filename_component(taskProjectRoot "${taskProject}" DIRECTORY)
    string(TOLOWER "${taskProjectRoot}" taskActualProjectRoot)
    string(TOLOWER "${CMAKE_SOURCE_DIR}" taskExpectedProjectRoot)
    if(NOT taskActualProjectRoot STREQUAL taskExpectedProjectRoot)
        message(FATAL_ERROR "The standalone Cue project must belong to the open game folder")
    endif()

    # 既存 SDK Module の相対参照を依存の生成中だけに閉じ、ゲーム Source の Root は別に保持する
    set(taskGameSourceRoot "${PROJECT_SOURCE_DIR}")
    set(taskGameBinaryRoot "${PROJECT_BINARY_DIR}")
    set(PROJECT_SOURCE_DIR "${taskSdkRoot}")
    set(PROJECT_BINARY_DIR "${CMAKE_BINARY_DIR}/EngineSdk")
    include("${taskSdkRoot}/cmake/CueGameProject.cmake")
    cue_validate_real_path("${taskSdkRoot}")
    cue_validate_real_path("${CMAKE_SOURCE_DIR}")
    cue_validate_generated_directory("${PROJECT_BINARY_DIR}")
    cue_select_game_build_target()
    include("${taskSdkRoot}/cmake/CueVersion.cmake")

    if(MSVC)
        # Editor が生成する Module と同じ CRT、文字 Code、並列 Compile の条件を使う
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        add_compile_options(/MP /utf-8)
    endif()
    foreach(taskModule IN ITEMS Foundation Math ScriptData Script)
        add_subdirectory("${taskSdkRoot}/Engine/Source/${taskModule}" "${PROJECT_BINARY_DIR}/${taskModule}")
    endforeach()

    if(CUE_GAME_BUILD_PLAYER)
        # 製品用だけに描画と Runtime を追加し、Editor / Tool / Play Host は生成しない
        foreach(taskModule IN ITEMS Assets ECS Platform Scene Profiler Logging
                Renderer/RHI Renderer/FrameGraph Renderer/Passes Runtime SceneRuntime ScriptRuntime
                Renderer/SceneRender GameRuntime Renderer/DX12 WindowsHost GameHost/Windows)
            add_subdirectory("${taskSdkRoot}/Engine/Source/${taskModule}" "${PROJECT_BINARY_DIR}/${taskModule}")
        endforeach()
    endif()

    # ゲーム自身の CMake では Source / Binary Root をゲームの値として公開する
    set(PROJECT_SOURCE_DIR "${taskGameSourceRoot}")
    set(PROJECT_BINARY_DIR "${taskGameBinaryRoot}")
    if(CUE_GAME_BUILD_PLAYER)
        cue_add_game_project("${taskProject}")
        get_property(taskApplication GLOBAL PROPERTY CUE_CONFIGURED_GAME_APP_TARGET)
        if(NOT "${taskApplication}" STREQUAL "")
            # Root Engine の実行 Target 一覧は読み込まないため、製品の依存 Target で PIX を先に配置する
            add_custom_command(TARGET "${taskApplication}.ShaderRuntime" POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:Cue.WinPixEventRuntime>" "$<TARGET_FILE_DIR:${taskApplication}>"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${taskSdkRoot}/ThirdParty/Licenses/WinPixEventRuntime-MIT.txt"
                    "${taskSdkRoot}/ThirdParty/Licenses/WinPixEventRuntime-ThirdPartyNotices.txt"
                    "$<TARGET_FILE_DIR:${taskApplication}>"
                VERBATIM)
        endif()
    else()
        cue_add_game_project("${taskProject}" NO_PLAYER)
    endif()
endfunction()

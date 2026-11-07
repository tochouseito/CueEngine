add_subdirectory(Foundation)
add_subdirectory(ScriptData)
add_subdirectory(Math)
add_subdirectory(Script)
add_subdirectory(Assets)
add_subdirectory(ECS)
add_subdirectory(Platform)
add_subdirectory(EngineInstallation)
add_subdirectory(Project)
add_subdirectory(ProjectBuild)
add_subdirectory(EditorIde)
add_subdirectory(Scene)
add_subdirectory(ProjectTools)
add_subdirectory(Profiler)
add_subdirectory(Logging)
add_subdirectory(Renderer/RHI)
add_subdirectory(Renderer/FrameGraph)
add_subdirectory(Renderer/Passes)
add_subdirectory(Runtime)
add_subdirectory(SceneRuntime)
add_subdirectory(ScriptRuntime)
add_subdirectory(Editor)
add_subdirectory(Renderer/SceneRender)
add_subdirectory(GameRuntime)

# 出力対象の選択は Native Host の条件分岐より前で検証し、未対応値を無視しない
include("${PROJECT_SOURCE_DIR}/cmake/CueGameProject.cmake")
if(NOT CUE_GAME_PROJECT STREQUAL "")
    cue_select_game_build_target()
endif()

if(WIN32)
    add_subdirectory(ProjectTool)
    # DX12 Device は WindowHost と独立して生成・検証する
    add_subdirectory(Renderer/DX12)

    # WindowsHost Library を先に定義し、実行 Target は Window の起動と終了を扱う
    add_subdirectory(WindowsHost)
    add_subdirectory(GameHost/Windows)
    add_subdirectory(ScriptPlay)
    add_subdirectory(PlayHost)
    # 開発 DLL / 製品 Library は選択した .cueproj の Source CMake から生成する
    cue_add_game_project("${CUE_GAME_PROJECT}")

    add_executable(CueWindowsHost WIN32
        WindowsHost/Main.cpp
    )

    target_link_libraries(CueWindowsHost PRIVATE Cue.WindowsHost Cue.Platform.Windows Cue.EngineInstallation)

    # 構成別の実行 File を同じ配置規則で出力する
    set_target_properties(CueWindowsHost PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )

    if(MSVC)
        # Development は最適化と Debug 情報を併用する
        target_compile_options(CueWindowsHost PRIVATE
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
        )

        target_link_options(CueWindowsHost PRIVATE
            $<$<CONFIG:Development>:/DEBUG>
        )
    endif()

    target_compile_definitions(CueWindowsHost PRIVATE
        CUE_REPOSITORY_ROOT="${PROJECT_SOURCE_DIR}"
        $<$<CONFIG:Release>:CUE_SHIPPING=1>
    )
    # Editor の実行入口を追加し、Windows の表示・Frame 基盤を再利用する
    set(CMAKE_MAP_IMPORTED_CONFIG_DEVELOPMENT Release)
    add_subdirectory(EditorUi)
    add_subdirectory(EditorHost)

    # 同じ出力 Directory の Host は Shader と DXC Runtime の配置 Target を共有する
    cue_copy_shader_runtime(CueWindowsHost CueEditorHost)
endif()

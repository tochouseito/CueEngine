# Editor 固有の Composition Root は Windows 実行基盤より上位に置く
# vcpkg の Release Library を独自の Development 構成でも利用する
set(CMAKE_MAP_IMPORTED_CONFIG_DEVELOPMENT Release)
find_package(imgui CONFIG REQUIRED GLOBAL)
include("${PROJECT_SOURCE_DIR}/cmake/CueEditorFonts.cmake")

add_library(Cue.EditorHost STATIC
    Public/EditorHost/ImGuiPass.h
    Public/EditorHost/PerformanceSettingsEditing.h
    Public/EditorHost/EditorLaunchOptions.h
    Private/EditorHost.cpp
    Private/EditorLaunchOptions.cpp
    Private/EditorWindowsServices.cpp
    Private/EditorPlayServices.cpp
    Private/EditorScriptController.cpp
    Private/ImGuiManager.cpp
    Private/EditorFonts.h
    Private/EditorFonts.cpp
    Private/DX12ImGuiBackend.cpp
)

target_include_directories(Cue.EditorHost PUBLIC Public)
target_link_libraries(Cue.EditorHost PUBLIC Cue.WindowsHost Cue.Profiler Cue.Project Cue.SceneRuntime Cue.ProjectBuild Cue.EditorIde Cue.ScriptPlay PRIVATE Cue.Editor Cue.Editor.Ui imgui::imgui Cue.Platform.Windows Cue.Foundation.Windows Cue.Renderer.DX12 Cue.Renderer.SceneRender ole32 shell32)
target_compile_features(Cue.EditorHost PUBLIC cxx_std_20)
target_link_libraries(Cue.EditorHost PRIVATE Cue.EngineVersion)

add_executable(CueEditorHost WIN32 Main.cpp)
cue_copy_editor_fonts(CueEditorHost)
target_link_libraries(CueEditorHost PRIVATE Cue.EditorHost Cue.ProjectBuild.Windows Cue.EditorIde.Windows Cue.EngineInstallation Cue.Platform.Windows Cue.Foundation.Windows shell32)
cue_copy_visual_studio_bridge(CueEditorHost)
# 未選択起動から Project を開いた場合も Metadata / Play の実行先を用意する
add_dependencies(CueEditorHost CuePlayHost)
# Visual Studio で Editor Target だけを Build しても、選択 Project の完成 Manifest を用意する
if(TARGET Cue.GameProjectBuild)
    add_dependencies(CueEditorHost Cue.GameProjectBuild)
endif()
set_target_properties(CueEditorHost PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    # 全構成で Project 未選択から開始し、Editor の File Menu から対象を開く
    VS_DEBUGGER_COMMAND_ARGUMENTS ""
    VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT CueEditorHost)

# 配布 SDK の IDE Tree は Project 側にあるため、F5 は検証済み導入版 Editor を起動する
if(EXISTS "${PROJECT_SOURCE_DIR}/engine.cueengine")
    set_target_properties(CueEditorHost PROPERTIES
        VS_DEBUGGER_COMMAND "${PROJECT_SOURCE_DIR}/Bin/Editor/CueEditorHost.exe"
    )
endif()

if(MSVC)
    foreach(taskTarget IN ITEMS Cue.EditorHost CueEditorHost)
        target_compile_options(${taskTarget} PRIVATE
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
        )
    endforeach()
    target_link_options(CueEditorHost PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

target_compile_definitions(CueEditorHost PRIVATE $<$<CONFIG:Release>:CUE_SHIPPING=1>)
target_compile_definitions(CueEditorHost PRIVATE CUE_REPOSITORY_ROOT="${PROJECT_SOURCE_DIR}")

# Authoring Tool の配置は生成時に固定し、Explorer / Visual Studio 起動時の環境変数に依存させない
get_filename_component(taskEditorToolchain "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE)
get_filename_component(taskEditorVcpkgRoot "${taskEditorToolchain}" DIRECTORY)
get_filename_component(taskEditorVcpkgRoot "${taskEditorVcpkgRoot}" DIRECTORY)
get_filename_component(taskEditorVcpkgRoot "${taskEditorVcpkgRoot}" DIRECTORY)
file(TO_CMAKE_PATH "${taskEditorToolchain}" taskEditorToolchain)
file(TO_CMAKE_PATH "${taskEditorVcpkgRoot}" taskEditorVcpkgRoot)
target_compile_definitions(CueEditorHost PRIVATE
    CUE_EDITOR_CMAKE="${CMAKE_COMMAND}"
    CUE_EDITOR_TOOLCHAIN="${taskEditorToolchain}"
    CUE_EDITOR_VCPKG_ROOT="${taskEditorVcpkgRoot}"
)

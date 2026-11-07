# ImGui の Panel は Native Host と編集ドメインから分離し、操作要求だけを返す
find_package(imgui CONFIG REQUIRED GLOBAL)
add_library(Cue.Editor.Ui STATIC
    Private/EditorUi.cpp
    Private/SceneMenuBar.cpp
    Private/ObjectListPanel.cpp
    Private/TransformInspectorPanel.cpp
    Private/ScriptInspectorPanel.cpp
    Private/SceneViewPanel.cpp
    Private/BuildPanel.cpp
)
target_include_directories(Cue.Editor.Ui PUBLIC Public)
target_link_libraries(Cue.Editor.Ui PUBLIC Cue.Editor Cue.ProjectBuild PRIVATE imgui::imgui)
target_compile_features(Cue.Editor.Ui PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(Cue.Editor.Ui PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()

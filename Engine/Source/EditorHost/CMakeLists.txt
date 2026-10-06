# Editor 固有の Composition Root は Windows 実行基盤より上位に置く
# vcpkg の Release Library を独自の Development 構成でも利用する
set(CMAKE_MAP_IMPORTED_CONFIG_DEVELOPMENT Release)
find_package(imgui CONFIG REQUIRED GLOBAL)

add_library(Cue.EditorHost STATIC
    Public/EditorHost/ImGuiPass.h
    Private/EditorHost.cpp
    Private/ImGuiManager.cpp
    Private/DX12ImGuiBackend.cpp
)

target_include_directories(Cue.EditorHost PUBLIC Public)
target_link_libraries(Cue.EditorHost PUBLIC Cue.WindowsHost PRIVATE imgui::imgui Cue.Platform.Windows Cue.Renderer.DX12)
target_compile_features(Cue.EditorHost PUBLIC cxx_std_20)

add_executable(CueEditorHost WIN32 Main.cpp)
target_link_libraries(CueEditorHost PRIVATE Cue.EditorHost Cue.Platform.Windows)
set_target_properties(CueEditorHost PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

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

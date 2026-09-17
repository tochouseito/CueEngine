find_package(imgui CONFIG REQUIRED)

set_property(
    TARGET imgui::imgui
    PROPERTY MAP_IMPORTED_CONFIG_DEVELOPMENT Release
)

add_library(Cue.ImGui.Core INTERFACE)
target_link_libraries(Cue.ImGui.Core INTERFACE imgui::imgui)
target_compile_features(Cue.ImGui.Core INTERFACE cxx_std_20)
target_compile_definitions(Cue.ImGui.Core INTERFACE CUE_IMGUI_COMPILE_VERSION=19291)

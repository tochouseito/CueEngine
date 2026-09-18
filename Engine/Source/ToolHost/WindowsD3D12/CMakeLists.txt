add_library(Cue.ToolHost.WindowsD3D12 STATIC)

target_sources(
    Cue.ToolHost.WindowsD3D12
    PRIVATE
        Private/ToolHost.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/ToolHost/WindowsD3D12/ToolHost.h
)

target_link_libraries(
    Cue.ToolHost.WindowsD3D12
    PUBLIC
        Cue.Foundation
        Cue.Platform
        Cue.Renderer
    PRIVATE
        Cue.ImGui.Core
        Cue.Platform.Windows
        d3d12
        d3dcompiler
        dxgi
        dxguid
)
target_compile_features(Cue.ToolHost.WindowsD3D12 PUBLIC cxx_std_20)
target_include_directories(Cue.ToolHost.WindowsD3D12 PUBLIC Public)
target_compile_definitions(Cue.ToolHost.WindowsD3D12 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties(
    Cue.ToolHost.WindowsD3D12
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.ToolHost.WindowsD3D12
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.ToolHost.WindowsD3D12 PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

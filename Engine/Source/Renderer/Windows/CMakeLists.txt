# D3D12資源とWindows PresentationをWindows専用Targetに閉じ込める
add_library(Cue.Renderer.Windows STATIC
    Private/WindowsRenderer.cpp
)

target_include_directories(Cue.Renderer.Windows PUBLIC
    Public
)

target_link_libraries(Cue.Renderer.Windows
    PUBLIC Cue.Platform
    PRIVATE d3d12 dxgi
)

target_compile_features(Cue.Renderer.Windows PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Renderer.Windows PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

target_compile_definitions(Cue.Renderer.Windows PRIVATE
    $<$<CONFIG:Release>:CUE_SHIPPING=1>
)

# D3D12資源とWindows PresentationをWindows専用Targetに閉じ込める
add_library(Cue.Renderer.D3D12 STATIC
    Private/D3D12Renderer.cpp
)

target_include_directories(Cue.Renderer.D3D12 PUBLIC
    Public
)

target_link_libraries(Cue.Renderer.D3D12
    PUBLIC Cue.Platform
    PRIVATE d3d12 dxgi
)

target_compile_features(Cue.Renderer.D3D12 PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Renderer.D3D12 PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

target_compile_definitions(Cue.Renderer.D3D12 PRIVATE
    $<$<CONFIG:Release>:CUE_SHIPPING=1>
)

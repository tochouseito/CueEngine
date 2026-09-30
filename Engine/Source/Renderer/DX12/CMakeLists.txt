# DX12 固有の Device 作成と所有を独立した Module にする
add_library(Cue.Renderer.DX12 STATIC
    Private/BackendFactory.cpp
    Private/DX12Backend.cpp
    Private/DX12RenderDevice.cpp
)

target_include_directories(Cue.Renderer.DX12 PUBLIC Public)
target_link_libraries(Cue.Renderer.DX12
    PUBLIC Cue.Foundation Cue.Renderer.RHI
    PRIVATE Cue.Platform.Windows d3d12 dxgi dxguid
)
target_compile_features(Cue.Renderer.DX12 PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Renderer.DX12 PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

target_compile_definitions(Cue.Renderer.DX12 PRIVATE
    $<$<CONFIG:Release>:CUE_SHIPPING=1>
)

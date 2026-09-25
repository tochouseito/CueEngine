# 公開契約の Windows 実装と診断出力を一つの Target にまとめる
add_library(Cue.Platform.Windows STATIC
    Private/Diagnostics.cpp
    Private/WindowsPlatform.cpp
    Private/WindowsThreadServices.cpp
)

target_include_directories(Cue.Platform.Windows PUBLIC
    Public
)

target_link_libraries(Cue.Platform.Windows PUBLIC
    Cue.Platform
)

target_link_libraries(Cue.Platform.Windows PRIVATE
    Cue.Foundation.Windows
)

target_compile_features(Cue.Platform.Windows PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(Cue.Platform.Windows PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

# Debugger 出力は Debug の実装だけで有効にする
target_compile_definitions(Cue.Platform.Windows PRIVATE
    $<$<CONFIG:Debug>:CUE_DEBUG_OUTPUT=1>
    $<$<CONFIG:Release>:CUE_SHIPPING=1>
)

# Window と Message API を使う実装だけが user32 に依存する
target_link_libraries(Cue.Platform.Windows PRIVATE user32)

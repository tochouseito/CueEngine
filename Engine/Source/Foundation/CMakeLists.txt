# Error と Result は Header のみで公開する
add_library(Cue.Foundation INTERFACE)

target_include_directories(Cue.Foundation INTERFACE
    Public
)

target_compile_features(Cue.Foundation INTERFACE cxx_std_20)

# 通常実行の計測精度や負荷を変えず、手動調査 Build だけ区間 Trace を有効にする
option(CUE_ENABLE_TIMING_TRACE "Enable opt-in frame timing traces" OFF)
if(CUE_ENABLE_TIMING_TRACE)
    target_compile_definitions(Cue.Foundation INTERFACE CUE_ENABLE_TIMING_TRACE=1)
endif()

if(MSVC)
    # Header を利用する Target にも UTF-8 Source 指定を伝播する
    target_compile_options(Cue.Foundation INTERFACE /utf-8)
endif()

# Win32 変換実装は汎用 Foundation から分離する
if(WIN32)
    add_subdirectory(Windows)
endif()

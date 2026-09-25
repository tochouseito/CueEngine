# Error と Result は Header のみで公開する
add_library(Cue.Foundation INTERFACE)

target_include_directories(Cue.Foundation INTERFACE
    Public
)

target_compile_features(Cue.Foundation INTERFACE cxx_std_20)

if(MSVC)
    # Header を利用する Target にも UTF-8 Source 指定を伝播する
    target_compile_options(Cue.Foundation INTERFACE /utf-8)
endif()

# Win32 変換実装は汎用 Foundation から分離する
if(WIN32)
    add_subdirectory(Windows)
endif()

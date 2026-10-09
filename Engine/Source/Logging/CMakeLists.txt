# ログの整形・配送・File 出力は抽象 File API だけを使用する
add_library(Cue.Logging STATIC Private/Logger.cpp Private/FileLogSink.cpp)
target_include_directories(Cue.Logging PUBLIC Public)
target_link_libraries(Cue.Logging PUBLIC Cue.Foundation Cue.Platform)
if(MSVC)
    target_compile_options(Cue.Logging PRIVATE
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

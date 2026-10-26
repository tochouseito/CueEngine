# 記録と集計だけを提供し、File / ETW / Editor への出力依存を持たない
add_library(Cue.Profiler STATIC Private/Profiler.cpp)

target_include_directories(Cue.Profiler PUBLIC Public)
target_link_libraries(Cue.Profiler PUBLIC Cue.Foundation Cue.Platform)
target_compile_features(Cue.Profiler PUBLIC cxx_std_20)

# 基本統計の記録と独立して、手動調査だけ詳細な区間 Trace を有効にする
option(CUE_ENABLE_TIMING_TRACE "Enable opt-in frame timing traces" OFF)
if(CUE_ENABLE_TIMING_TRACE)
    target_compile_definitions(Cue.Profiler PUBLIC CUE_ENABLE_TIMING_TRACE=1)
endif()

if(MSVC)
    target_compile_options(Cue.Profiler PRIVATE
        /utf-8
        $<$<CONFIG:Development>:/O2>
        $<$<CONFIG:Development>:/Zi>
    )
endif()

# Game DLL のロードだけを Win32 Adapter へ閉じ込め、製品の静的 Link では追加しない
add_library(Cue.ScriptRuntime.Windows STATIC Private/WindowsGameModule.cpp)
target_include_directories(Cue.ScriptRuntime.Windows PUBLIC Public)
target_link_libraries(Cue.ScriptRuntime.Windows PUBLIC Cue.ScriptRuntime PRIVATE Cue.Foundation.Windows)
if(MSVC)
    target_compile_options(Cue.ScriptRuntime.Windows PRIVATE $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
endif()

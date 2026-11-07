# 再利用できる手動 Tool だけを定義し、長時間の計測を通常 CTest へ登録しない
add_executable(CueImGuiDescriptorLookupProbe ImGuiDescriptorLookupProbe.cpp)
set(taskPerformanceTargets CueImGuiDescriptorLookupProbe)

if(WIN32)
    add_executable(CueM09FramePacingProbe M09FramePacingProbe.cpp)
    target_link_libraries(CueM09FramePacingProbe PRIVATE Cue.EditorHost imgui::imgui)

    add_executable(CueSpikeTimingProbe SpikeTimingProbe.cpp)
    target_link_libraries(CueSpikeTimingProbe PRIVATE Cue.EditorHost Cue.Renderer.DX12 imgui::imgui user32 advapi32)

    add_executable(CueSpikeEtwExport SpikeEtwExport.cpp)
    target_link_libraries(CueSpikeEtwExport PRIVATE Cue.Foundation advapi32 tdh)

    add_executable(CueFrameControllerTimingProbe FrameControllerTimingProbe.cpp)
    target_link_libraries(CueFrameControllerTimingProbe PRIVATE Cue.Runtime.Frame Cue.Platform.Windows user32)

    list(APPEND taskPerformanceTargets CueM09FramePacingProbe CueSpikeTimingProbe CueSpikeEtwExport
        CueFrameControllerTimingProbe)
endif()

foreach(taskTarget IN LISTS taskPerformanceTargets)
    target_compile_features(${taskTarget} PRIVATE cxx_std_20)
    # 既存の解析 Script が参照する実行 File の配置を維持する
    set_target_properties(${taskTarget} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Engine/Tests/$<CONFIG>")
    if(MSVC)
        target_compile_options(${taskTarget} PRIVATE /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>)
    endif()
endforeach()

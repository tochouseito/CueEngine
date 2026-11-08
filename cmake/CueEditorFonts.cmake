# 同じ出力 Directory の利用者は共有 Copy Target にまとめ、並列 Copy の競合を避ける
function(cue_copy_editor_fonts a_target)
    set(taskFontDirectory "${PROJECT_SOURCE_DIR}/ThirdParty/Fonts")
    set(taskLicenseDirectory "${PROJECT_SOURCE_DIR}/ThirdParty/Licenses")
    set(taskOutput "$<TARGET_FILE_DIR:${a_target}>/EngineResources/Fonts")
    add_custom_target(${a_target}.EditorFonts
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${taskFontDirectory}" "${taskOutput}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${taskOutput}/Licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${taskLicenseDirectory}/Inter-OFL.txt"
                "${taskLicenseDirectory}/NotoSansJP-OFL.txt"
                "${taskLicenseDirectory}/JetBrainsMono-OFL.txt"
                "${taskLicenseDirectory}/MaterialSymbols-Apache-2.0.txt"
                "${taskLicenseDirectory}/MaterialSymbols-Notice.txt"
                "${taskLicenseDirectory}/ImGui-JapaneseGlyphRanges-Notice.txt"
                "${taskOutput}/Licenses"
        VERBATIM
    )
    foreach(taskTarget IN ITEMS ${a_target} ${ARGN})
        add_dependencies(${taskTarget} ${a_target}.EditorFonts)
    endforeach()
endfunction()

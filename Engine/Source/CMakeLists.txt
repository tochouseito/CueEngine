add_subdirectory(Foundation)
add_subdirectory(Platform)
add_subdirectory(Runtime)

if(WIN32)
    # WindowsHost Library を先に定義し、実行 Target は起動、Step、終了判断を持つ
    add_subdirectory(WindowsHost)

    add_executable(CueWindowsHost WIN32
        WindowsHost/Main.cpp
    )

    target_link_libraries(CueWindowsHost PRIVATE Cue.WindowsHost Cue.Platform.Windows)

    # 構成別の実行 File を同じ配置規則で出力する
    set_target_properties(CueWindowsHost PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )

    if(MSVC)
        # Development は最適化と Debug 情報を併用する
        target_compile_options(CueWindowsHost PRIVATE
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
        )

        target_link_options(CueWindowsHost PRIVATE
            $<$<CONFIG:Development>:/DEBUG>
        )
    endif()

    target_compile_definitions(CueWindowsHost PRIVATE
        $<$<CONFIG:Release>:CUE_SHIPPING=1>
    )
endif()

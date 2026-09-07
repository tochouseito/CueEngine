add_executable(CueProjectFilesPublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueProjectFilesPublicHeaderTest PRIVATE Cue.ProjectFiles)
target_compile_features(CueProjectFilesPublicHeaderTest PRIVATE cxx_std_20)

set(cueProjectFilesTestTargets CueProjectFilesPublicHeaderTest)

if(WIN32)
    add_executable(CueProjectFilesTests ProjectFileTests.cpp)
    target_link_libraries(CueProjectFilesTests PRIVATE Cue.ProjectFiles Cue.IO.Windows)
    target_compile_features(CueProjectFilesTests PRIVATE cxx_std_20)
    add_executable(CueProjectFilesFileDialogRevalidationTests FileDialogRevalidationTests.cpp)
    target_link_libraries(
        CueProjectFilesFileDialogRevalidationTests
        PRIVATE
            Cue.ProjectFiles
            Cue.IO.Windows
            Cue.Platform
    )
    target_compile_features(CueProjectFilesFileDialogRevalidationTests PRIVATE cxx_std_20)
    add_executable(CueProjectFilesRecoveryTests RecoveryTests.cpp)
    target_link_libraries(CueProjectFilesRecoveryTests PRIVATE Cue.ProjectFiles Cue.IO.Windows)
    target_compile_features(CueProjectFilesRecoveryTests PRIVATE cxx_std_20)
    add_executable(CueProjectFilesTrashRecordTests TrashRecordTests.cpp)
    target_include_directories(
        CueProjectFilesTrashRecordTests
        PRIVATE
            "${PROJECT_SOURCE_DIR}/Engine/Source/ProjectFiles/Private"
    )
    target_link_libraries(CueProjectFilesTrashRecordTests PRIVATE Cue.ProjectFiles)
    target_compile_features(CueProjectFilesTrashRecordTests PRIVATE cxx_std_20)
    list(
        APPEND
        cueProjectFilesTestTargets
        CueProjectFilesTests
        CueProjectFilesFileDialogRevalidationTests
        CueProjectFilesRecoveryTests
        CueProjectFilesTrashRecordTests
    )
endif()

foreach(testTarget IN LISTS cueProjectFilesTestTargets)
    set_target_properties(
        ${testTarget}
        PROPERTIES
            CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
    if(MSVC)
        target_compile_options(
            ${testTarget}
            PRIVATE
                /W4
                /WX
                /permissive-
                /Zc:__cplusplus
                /utf-8
                $<$<CONFIG:Development>:/O2>
                $<$<CONFIG:Development>:/Zi>
        )
        target_link_options(${testTarget} PRIVATE $<$<CONFIG:Development>:/DEBUG>)
    endif()
endforeach()

add_custom_target(Cue.ProjectFiles.Tests DEPENDS ${cueProjectFilesTestTargets})

add_test(NAME Cue.ProjectFiles.PublicHeaders COMMAND CueProjectFilesPublicHeaderTest)
set_tests_properties(Cue.ProjectFiles.PublicHeaders PROPERTIES LABELS ProjectFiles TIMEOUT 10)

if(WIN32)
    add_test(NAME Cue.ProjectFiles.Create COMMAND CueProjectFilesTests)
    set_tests_properties(Cue.ProjectFiles.Create PROPERTIES LABELS "ProjectFiles;Windows" TIMEOUT 30)
    add_test(NAME Cue.ProjectFiles.FileDialogRevalidation COMMAND CueProjectFilesFileDialogRevalidationTests)
    set_tests_properties(
        Cue.ProjectFiles.FileDialogRevalidation
        PROPERTIES
            LABELS "ProjectFiles;Platform;Windows"
            TIMEOUT 30
    )
    add_test(NAME Cue.ProjectFiles.Recovery COMMAND CueProjectFilesRecoveryTests)
    set_tests_properties(Cue.ProjectFiles.Recovery PROPERTIES LABELS "ProjectFiles;Windows" TIMEOUT 30)
    add_test(NAME Cue.ProjectFiles.TrashRecord COMMAND CueProjectFilesTrashRecordTests)
    set_tests_properties(Cue.ProjectFiles.TrashRecord PROPERTIES LABELS ProjectFiles TIMEOUT 10)
endif()

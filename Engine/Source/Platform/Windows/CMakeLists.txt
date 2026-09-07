add_library(Cue.Platform.Windows STATIC)

target_sources(
    Cue.Platform.Windows
    PRIVATE
        Private/UtfConversion.cpp
        Private/UtfConversion.h
        Private/SystemCapabilityMapper.cpp
        Private/SystemCapabilityMapper.h
        Private/WindowsMessageSink.cpp
        Private/WindowsFileDialog.cpp
        Private/WindowsWindowInterop.cpp
        Private/WindowsWindow.cpp
        Private/WindowsWindow.h
        Private/WindowsPlatform.cpp
        Private/WindowsSystemCapabilities.cpp
        Private/WindowsUtilities.cpp
        Private/WindowsUtilities.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Platform/Windows/WindowsPlatform.h
            Public/Cue/Platform/Windows/WindowsFileDialog.h
            Public/Cue/Platform/Windows/WindowsMessageSink.h
            Public/Cue/Platform/Windows/WindowsWindowInterop.h
)

target_link_libraries(
    Cue.Platform.Windows
    PUBLIC
        Cue.Platform
    PRIVATE
        Cue.Foundation.Windows
        Ole32
        Shell32
        User32
)
target_compile_features(Cue.Platform.Windows PUBLIC cxx_std_20)

target_compile_definitions(
    Cue.Platform.Windows
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

target_include_directories(
    Cue.Platform.Windows
    PUBLIC
        Public
    PRIVATE
        Private
)

set_target_properties(
    Cue.Platform.Windows
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Platform.Windows
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )

    target_link_options(
        Cue.Platform.Windows
        PRIVATE
            $<$<CONFIG:Development>:/DEBUG>
    )
endif()

if(BUILD_TESTING)
    add_library(Cue.Platform.Windows.TestSupport STATIC)

    target_sources(
        Cue.Platform.Windows.TestSupport
        PRIVATE
            Private/TestSupport/WindowsSystemCapabilityProbe.cpp
            Private/TestSupport/WindowsWindowLifecycleProbe.cpp
        PUBLIC
            FILE_SET HEADERS
            BASE_DIRS Public
            FILES
                Public/Cue/Platform/Windows/TestSupport/WindowsSystemCapabilityProbe.h
                Public/Cue/Platform/Windows/TestSupport/WindowsWindowLifecycleProbe.h
    )

    target_link_libraries(
        Cue.Platform.Windows.TestSupport
        PUBLIC
            Cue.Platform.Windows
        PRIVATE
            User32
    )
    target_compile_features(Cue.Platform.Windows.TestSupport PUBLIC cxx_std_20)

    target_compile_definitions(
        Cue.Platform.Windows.TestSupport
        PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
    )

    set_target_properties(
        Cue.Platform.Windows.TestSupport
        PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
            CXX_EXTENSIONS OFF
    )

    if(MSVC)
        target_compile_options(
            Cue.Platform.Windows.TestSupport
            PRIVATE
                /W4
                /WX
                /permissive-
                /Zc:__cplusplus
                /utf-8
                $<$<CONFIG:Development>:/O2>
                $<$<CONFIG:Development>:/Zi>
        )

        target_link_options(
            Cue.Platform.Windows.TestSupport
            PRIVATE
                $<$<CONFIG:Development>:/DEBUG>
        )
    endif()
endif()

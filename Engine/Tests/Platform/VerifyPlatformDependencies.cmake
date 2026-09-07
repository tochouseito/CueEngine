cmake_minimum_required(VERSION 4.2.0)

include("${CMAKE_CURRENT_LIST_DIR}/../CMake/CueDependencyVerification.cmake")

if(NOT EXISTS "${REPORT_FILE}")
    message(FATAL_ERROR "Platform dependency report does not exist: ${REPORT_FILE}")
endif()

file(READ "${REPORT_FILE}" dependencyReport)

foreach(
    requiredLine
    IN ITEMS
        "Cue.Platform LINK_LIBRARIES: Cue.Foundation"
        "Cue.Platform INTERFACE_LINK_LIBRARIES: Cue.Foundation"
        "Cue.Platform.Windows LINK_LIBRARIES: Cue.Platform;Cue.Foundation.Windows;Ole32;Shell32;User32"
        "Cue.Platform.Windows INTERFACE_LINK_LIBRARIES: Cue.Platform;$<LINK_ONLY:Cue.Foundation.Windows>;$<LINK_ONLY:Ole32>;$<LINK_ONLY:Shell32>;$<LINK_ONLY:User32>"
        "Cue.Platform MANUALLY_ADDED_DEPENDENCIES: <none>"
        "Cue.Platform INTERFACE_SOURCES: <none>"
        "Cue.Platform INTERFACE_LINK_LIBRARIES_DIRECT: <none>"
        "Cue.Platform.Windows MANUALLY_ADDED_DEPENDENCIES: <none>"
        "Cue.Platform.Windows INTERFACE_SOURCES: <none>"
        "Cue.Platform.Windows INTERFACE_LINK_LIBRARIES_DIRECT: <none>"
        "Generated and target object sources: <none>"
        "Target Source, Header, CXX Module, and precompiled header scan: enabled"
        "Named Header File Sets: scanned"
        "Named CXX Module File Sets: scanned"
        "PRECOMPILE_HEADERS and INTERFACE_PRECOMPILE_HEADERS: scanned"
        "PRECOMPILE_HEADERS_REUSE_FROM: <none>"
        "Target, interface, and source forced include options: forbidden"
        "Library-injecting link options: forbidden"
        "Allowed Windows PRIVATE links: Cue.Foundation.Windows;Ole32;Shell32;User32"
        "Required Windows PUBLIC link: Cue.Platform"
        "Allowed Windows LINK_ONLY links: Cue.Foundation.Windows;Ole32;Shell32;User32"
        "Forbidden dependencies: Cue.RHI;D3D12;Editor"
        "Windows SDK and non-standard UCRT include boundary: Windows/Private"
        "Compiler pragma link injection: forbidden"
        "Non-literal include directives: forbidden"
        "Preprocessor line continuations: normalized"
)
    cue_require_report_substring(
        "${dependencyReport}"
        "${requiredLine}"
        "Platform dependency report is missing: "
    )
endforeach()

file(
    GLOB_RECURSE
    platformSources
    LIST_DIRECTORIES FALSE
    "${PLATFORM_SOURCE_DIR}/*.c"
    "${PLATFORM_SOURCE_DIR}/*.cc"
    "${PLATFORM_SOURCE_DIR}/*.cpp"
    "${PLATFORM_SOURCE_DIR}/*.cxx"
    "${PLATFORM_SOURCE_DIR}/*.h"
    "${PLATFORM_SOURCE_DIR}/*.hh"
    "${PLATFORM_SOURCE_DIR}/*.hpp"
    "${PLATFORM_SOURCE_DIR}/*.hxx"
    "${PLATFORM_SOURCE_DIR}/*.inl"
    "${PLATFORM_SOURCE_DIR}/*.ixx"
)

if(NOT EXISTS "${PLATFORM_TARGET_FILE_LIST}")
    message(FATAL_ERROR "Platform Target file list does not exist: ${PLATFORM_TARGET_FILE_LIST}")
endif()

file(STRINGS "${PLATFORM_TARGET_FILE_LIST}" platformTargetFiles)
list(APPEND platformSources ${platformTargetFiles})
list(REMOVE_DUPLICATES platformSources)

set(hasWindowsSdkInclude FALSE)

if(NOT WIN32)
    message(FATAL_ERROR "Windows SDK dependency verification requires a Windows host")
endif()

if(NOT DEFINED WINDOWS_SDK_VERSION OR "${WINDOWS_SDK_VERSION}" STREQUAL "")
    message(FATAL_ERROR "Selected Windows SDK version is not available")
endif()

cmake_host_system_information(
    RESULT windowsSdkRoot
    QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
    VALUE KitsRoot10
    VIEW HOST
    ERROR_VARIABLE windowsSdkRegistryError
)

if(NOT "${windowsSdkRegistryError}" STREQUAL "" OR NOT IS_DIRECTORY "${windowsSdkRoot}")
    message(FATAL_ERROR "Windows SDK root could not be resolved: ${windowsSdkRegistryError}")
endif()

set(windowsSdkIncludeRoot "${windowsSdkRoot}/Include/${WINDOWS_SDK_VERSION}")
set(
    windowsPlatformIncludeRoots
    "${windowsSdkIncludeRoot}/um"
    "${windowsSdkIncludeRoot}/shared"
    "${windowsSdkIncludeRoot}/winrt"
    "${windowsSdkIncludeRoot}/cppwinrt"
)
set(windowsUcrtIncludeRoot "${windowsSdkIncludeRoot}/ucrt")

foreach(includeRoot IN LISTS windowsPlatformIncludeRoots)
    if(NOT IS_DIRECTORY "${includeRoot}")
        message(FATAL_ERROR "Windows SDK include directory does not exist: ${includeRoot}")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${windowsUcrtIncludeRoot}")
    message(FATAL_ERROR "Windows UCRT include directory does not exist: ${windowsUcrtIncludeRoot}")
endif()

set(
    standardCHeaders
    assert.h
    complex.h
    ctype.h
    errno.h
    fenv.h
    float.h
    inttypes.h
    iso646.h
    limits.h
    locale.h
    math.h
    setjmp.h
    signal.h
    stdalign.h
    stdarg.h
    stdatomic.h
    stdbool.h
    stddef.h
    stdint.h
    stdio.h
    stdlib.h
    stdnoreturn.h
    string.h
    tgmath.h
    threads.h
    time.h
    uchar.h
    wchar.h
    wctype.h
)

if(NOT EXISTS "${PLATFORM_DIRECT_PRECOMPILE_HEADER_LIST}")
    message(
        FATAL_ERROR
        "Platform direct precompiled header list does not exist: ${PLATFORM_DIRECT_PRECOMPILE_HEADER_LIST}"
    )
endif()

file(STRINGS "${PLATFORM_DIRECT_PRECOMPILE_HEADER_LIST}" directPrecompileHeaders)

foreach(directPrecompileHeader IN LISTS directPrecompileHeaders)
    if(NOT directPrecompileHeader MATCHES "^([^|]+)\\|([^|]+)\\|[<\"]([^>\"]+)[>\"]$")
        message(
            FATAL_ERROR
            "Platform direct precompiled header entry is invalid: ${directPrecompileHeader}"
        )
    endif()

    set(targetName "${CMAKE_MATCH_1}")
    set(visibility "${CMAKE_MATCH_2}")
    set(includedHeader "${CMAKE_MATCH_3}")
    set(isWindowsSdkInclude FALSE)

    foreach(includeRoot IN LISTS windowsPlatformIncludeRoots)
        if(EXISTS "${includeRoot}/${includedHeader}")
            set(isWindowsSdkInclude TRUE)
            break()
        endif()
    endforeach()

    if(NOT isWindowsSdkInclude AND EXISTS "${windowsUcrtIncludeRoot}/${includedHeader}")
        string(TOLOWER "${includedHeader}" includedHeaderLower)
        list(FIND standardCHeaders "${includedHeaderLower}" standardHeaderIndex)

        if(standardHeaderIndex EQUAL -1)
            set(isWindowsSdkInclude TRUE)
        endif()
    endif()

    if(
        isWindowsSdkInclude
        AND NOT (
            targetName STREQUAL "Cue.Platform.Windows"
            AND visibility STREQUAL "PRIVATE"
        )
    )
        message(
            FATAL_ERROR
            "Windows SDK precompiled header escaped the Windows private boundary: ${targetName} ${visibility}: ${includedHeader}"
        )
    endif()
endforeach()

foreach(platformSource IN LISTS platformSources)
    if(NOT EXISTS "${platformSource}" OR IS_DIRECTORY "${platformSource}")
        message(FATAL_ERROR "Platform source file does not exist: ${platformSource}")
    endif()

    file(READ "${platformSource}" sourceContents)
    file(RELATIVE_PATH relativeSource "${PLATFORM_SOURCE_DIR}" "${platformSource}")
    string(REPLACE "\\\r\n" "" sourceContents "${sourceContents}")
    string(REPLACE "\\\n" "" sourceContents "${sourceContents}")
    cue_strip_cpp_comments("${sourceContents}" sourceContents sourceCodeContents)
    string(TOLOWER "${sourceCodeContents}" sourceCodeContentsLower)
    string(
        REGEX MATCH
        "(^|\n)[ \t]*#[ \t]*pragma[ \t]+comment[ \t]*\\("
        preprocessorCommentDirective
        "${sourceCodeContentsLower}"
    )
    string(
        REGEX MATCH
        "(__pragma|_pragma)[ \t\r\n]*\\("
        compilerPragmaDirective
        "${sourceCodeContentsLower}"
    )

    if(preprocessorCommentDirective OR compilerPragmaDirective)
        message(
            FATAL_ERROR
            "Platform source contains a compiler pragma that can inject link dependencies: ${relativeSource}"
        )
    endif()

    string(
        REGEX MATCHALL
        "(^|\n)[ \t]*#[ \t]*include[^\r\n]*"
        allIncludeDirectives
        "${sourceContents}"
    )

    foreach(includeDirective IN LISTS allIncludeDirectives)
        string(STRIP "${includeDirective}" includeDirective)

        if(NOT includeDirective MATCHES "^#[ \t]*include[ \t]*[<\"][^>\"]+[>\"]")
            message(
                FATAL_ERROR
                "Platform source contains a non-literal include directive: ${relativeSource}: ${includeDirective}"
            )
        endif()
    endforeach()

    string(
        REGEX MATCHALL
        "#[ \t]*include[ \t]*[<\"][^>\"]+[>\"]"
        includeDirectives
        "${sourceContents}"
    )

    foreach(includeDirective IN LISTS includeDirectives)
        string(
            REGEX REPLACE
            "^#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"]$"
            "\\1"
            includedHeader
            "${includeDirective}"
        )
        set(isWindowsSdkInclude FALSE)

        foreach(includeRoot IN LISTS windowsPlatformIncludeRoots)
            if(EXISTS "${includeRoot}/${includedHeader}")
                set(isWindowsSdkInclude TRUE)
                break()
            endif()
        endforeach()

        if(NOT isWindowsSdkInclude AND EXISTS "${windowsUcrtIncludeRoot}/${includedHeader}")
            string(TOLOWER "${includedHeader}" includedHeaderLower)
            list(FIND standardCHeaders "${includedHeaderLower}" standardHeaderIndex)

            if(standardHeaderIndex EQUAL -1)
                set(isWindowsSdkInclude TRUE)
            endif()
        endif()

        if(isWindowsSdkInclude)
            if(NOT relativeSource MATCHES "^Windows/Private/")
                message(
                    FATAL_ERROR
                    "Windows SDK include escaped the Windows private boundary: ${relativeSource}: ${includedHeader}"
                )
            endif()

            set(hasWindowsSdkInclude TRUE)
        endif()
    endforeach()

    if(relativeSource MATCHES "(^|/)Public/")
        string(
            REGEX MATCH
            "(^|[^A-Za-z0-9_])(HWND|HINSTANCE|LRESULT)([^A-Za-z0-9_]|$)"
            exposedWin32Type
            "${sourceContents}"
        )

        if(exposedWin32Type)
            message(FATAL_ERROR "Public Platform header exposes a Win32 type: ${relativeSource}")
        endif()
    endif()
endforeach()

if(NOT hasWindowsSdkInclude)
    message(FATAL_ERROR "Cue.Platform.Windows does not exercise a private Windows SDK include")
endif()

message(STATUS "Cue.Platform dependency direction: passed")
message(STATUS "Cue.Platform Windows SDK and UCRT private boundary: passed")

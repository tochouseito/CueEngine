add_executable(
    CuePlatformPublicHeaderTest
    FileDialogHeaderCompile.cpp
    PublicHeaderCompile.cpp
    SystemCapabilitiesHeaderCompile.cpp
    WindowEventHeaderCompile.cpp
    WindowHeaderCompile.cpp
    WindowSystemHeaderCompile.cpp
)

target_link_libraries(CuePlatformPublicHeaderTest PRIVATE Cue.Platform)
target_compile_features(CuePlatformPublicHeaderTest PRIVATE cxx_std_20)

add_executable(CuePlatformFileDialogTests FileDialogTests.cpp)
target_link_libraries(CuePlatformFileDialogTests PRIVATE Cue.Platform)
target_compile_features(CuePlatformFileDialogTests PRIVATE cxx_std_20)

add_executable(
    CuePlatformWindowsPublicHeaderTest
    PublicHeaderCompile.cpp
    WindowsFileDialogHeaderCompile.cpp
    WindowsPlatformHeaderCompile.cpp
    WindowsMessageSinkHeaderCompile.cpp
    WindowsSystemCapabilityProbeHeaderCompile.cpp
    WindowsWindowLifecycleProbeHeaderCompile.cpp
    WindowsWindowInteropHeaderCompile.cpp
)

target_link_libraries(CuePlatformWindowsPublicHeaderTest PRIVATE Cue.Platform.Windows)
target_compile_features(CuePlatformWindowsPublicHeaderTest PRIVATE cxx_std_20)

add_executable(CuePlatformWindowsFileDialogTests WindowsFileDialogTests.cpp)
target_link_libraries(CuePlatformWindowsFileDialogTests PRIVATE Cue.Platform.Windows)
target_compile_features(CuePlatformWindowsFileDialogTests PRIVATE cxx_std_20)

add_executable(
    CuePlatformWindowsLifecycleTests
    WindowsWindowLifecycleTests.cpp
)

add_executable(
    CuePlatformWindowsSystemCapabilityTests
    WindowsSystemCapabilityTests.cpp
)

add_executable(
    CuePlatformWindowsMessageSinkTests
    WindowsMessageSinkTests.cpp
)

target_link_libraries(
    CuePlatformWindowsMessageSinkTests
    PRIVATE
        Cue.Platform.Windows
)
target_compile_features(CuePlatformWindowsMessageSinkTests PRIVATE cxx_std_20)

target_link_libraries(
    CuePlatformWindowsSystemCapabilityTests
    PRIVATE
        Cue.Platform.Windows
        Cue.Platform.Windows.TestSupport
)
target_compile_features(CuePlatformWindowsSystemCapabilityTests PRIVATE cxx_std_20)

target_link_libraries(
    CuePlatformWindowsLifecycleTests
    PRIVATE
        Cue.Platform.Windows
        Cue.Platform.Windows.TestSupport
)
target_compile_features(CuePlatformWindowsLifecycleTests PRIVATE cxx_std_20)

foreach(
    testTarget
    IN ITEMS
        CuePlatformPublicHeaderTest
        CuePlatformFileDialogTests
        CuePlatformWindowsPublicHeaderTest
        CuePlatformWindowsFileDialogTests
        CuePlatformWindowsLifecycleTests
        CuePlatformWindowsMessageSinkTests
        CuePlatformWindowsSystemCapabilityTests
)
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

        target_link_options(
            ${testTarget}
            PRIVATE
                $<$<CONFIG:Development>:/DEBUG>
        )
    endif()
endforeach()

function(cue_collect_platform_header_files targetName outputVariable)
    get_target_property(targetHeaderSets ${targetName} HEADER_SETS)
    get_target_property(targetInterfaceHeaderSets ${targetName} INTERFACE_HEADER_SETS)

    foreach(headerSetProperty IN ITEMS targetHeaderSets targetInterfaceHeaderSets)
        if(${headerSetProperty} MATCHES "-NOTFOUND$")
            set(${headerSetProperty} "")
        endif()
    endforeach()

    set(allHeaderSets ${targetHeaderSets} ${targetInterfaceHeaderSets})
    list(REMOVE_DUPLICATES allHeaderSets)
    set(headerFiles "")

    foreach(headerSet IN LISTS allHeaderSets)
        if(headerSet STREQUAL "HEADERS")
            set(headerSetProperty HEADER_SET)
        else()
            set(headerSetProperty "HEADER_SET_${headerSet}")
        endif()

        get_target_property(targetHeaderFiles ${targetName} "${headerSetProperty}")

        if(targetHeaderFiles MATCHES "-NOTFOUND$")
            set(targetHeaderFiles "")
        endif()

        list(APPEND headerFiles ${targetHeaderFiles})
    endforeach()

    list(REMOVE_DUPLICATES headerFiles)
    set(${outputVariable} "${headerFiles}" PARENT_SCOPE)
endfunction()

function(cue_collect_platform_cxx_module_files targetName outputVariable)
    get_target_property(targetModuleSets ${targetName} CXX_MODULE_SETS)
    get_target_property(targetInterfaceModuleSets ${targetName} INTERFACE_CXX_MODULE_SETS)

    foreach(moduleSetProperty IN ITEMS targetModuleSets targetInterfaceModuleSets)
        if(${moduleSetProperty} MATCHES "-NOTFOUND$")
            set(${moduleSetProperty} "")
        endif()
    endforeach()

    set(allModuleSets ${targetModuleSets} ${targetInterfaceModuleSets})
    list(REMOVE_DUPLICATES allModuleSets)
    set(moduleFiles "")

    foreach(moduleSet IN LISTS allModuleSets)
        if(moduleSet STREQUAL "CXX_MODULES")
            set(moduleSetProperty CXX_MODULE_SET)
        else()
            set(moduleSetProperty "CXX_MODULE_SET_${moduleSet}")
        endif()

        get_target_property(targetModuleFiles ${targetName} "${moduleSetProperty}")

        if(targetModuleFiles MATCHES "-NOTFOUND$")
            set(targetModuleFiles "")
        endif()

        list(APPEND moduleFiles ${targetModuleFiles})
    endforeach()

    list(REMOVE_DUPLICATES moduleFiles)
    set(${outputVariable} "${moduleFiles}" PARENT_SCOPE)
endfunction()

function(
    cue_collect_platform_precompile_headers
    targetName
    outputFiles
    outputDirectIncludes
)
    get_target_property(targetSourceDirectory ${targetName} SOURCE_DIR)
    get_target_property(targetReuseFrom ${targetName} PRECOMPILE_HEADERS_REUSE_FROM)

    if(targetReuseFrom MATCHES "-NOTFOUND$")
        set(targetReuseFrom "")
    endif()

    if(NOT "${targetReuseFrom}" STREQUAL "")
        message(
            FATAL_ERROR
            "${targetName} must not reuse another target's precompiled header: ${targetReuseFrom}"
        )
    endif()

    set(precompileHeaderFiles "")
    set(directIncludes "")

    set(precompileHeaderProperties PRECOMPILE_HEADERS INTERFACE_PRECOMPILE_HEADERS)
    set(precompileHeaderVisibilities PRIVATE INTERFACE)

    foreach(propertyIndex RANGE 0 1)
        list(GET precompileHeaderProperties ${propertyIndex} precompileHeaderProperty)
        list(GET precompileHeaderVisibilities ${propertyIndex} visibility)
        get_target_property(
            precompileHeaders
            ${targetName}
            ${precompileHeaderProperty}
        )

        if(precompileHeaders MATCHES "-NOTFOUND$")
            set(precompileHeaders "")
        endif()

        foreach(precompileHeader IN LISTS precompileHeaders)
            if(precompileHeader MATCHES "\\$<")
                message(
                    FATAL_ERROR
                    "${targetName} contains an uninspectable precompiled header: ${precompileHeader}"
                )
            endif()

            if(precompileHeader MATCHES "^<[^>]+>$" OR precompileHeader MATCHES "^\"[^\"]+\"$")
                list(
                    APPEND
                    directIncludes
                    "${targetName}|${visibility}|${precompileHeader}"
                )
                continue()
            endif()

            get_filename_component(
                precompileHeaderPath
                "${precompileHeader}"
                ABSOLUTE
                BASE_DIR "${targetSourceDirectory}"
            )

            if(NOT EXISTS "${precompileHeaderPath}" OR IS_DIRECTORY "${precompileHeaderPath}")
                message(
                    FATAL_ERROR
                    "${targetName} precompiled header cannot be inspected: ${precompileHeader}"
                )
            endif()

            file(
                RELATIVE_PATH
                relativePrecompileHeader
                "${targetSourceDirectory}"
                "${precompileHeaderPath}"
            )

            if(relativePrecompileHeader MATCHES "^\\.\\.(/|$)")
                message(
                    FATAL_ERROR
                    "${targetName} precompiled header must remain in its source tree: ${precompileHeaderPath}"
                )
            endif()

            if(
                targetName STREQUAL "Cue.Platform"
                AND relativePrecompileHeader MATCHES "^Windows/"
            )
                message(
                    FATAL_ERROR
                    "Cue.Platform must not consume a Windows implementation precompiled header: ${precompileHeaderPath}"
                )
            endif()

            if(
                visibility STREQUAL "INTERFACE"
                AND relativePrecompileHeader MATCHES "(^|/)Private/"
            )
                message(
                    FATAL_ERROR
                    "${targetName} must not expose a private precompiled header: ${precompileHeaderPath}"
                )
            endif()

            list(APPEND precompileHeaderFiles "${precompileHeaderPath}")
        endforeach()
    endforeach()

    list(REMOVE_DUPLICATES precompileHeaderFiles)
    list(REMOVE_DUPLICATES directIncludes)
    set(${outputFiles} "${precompileHeaderFiles}" PARENT_SCOPE)
    set(${outputDirectIncludes} "${directIncludes}" PARENT_SCOPE)
endfunction()

function(cue_verify_platform_forced_include_options targetName)
    foreach(
        compileOptionProperty
        IN ITEMS
            COMPILE_OPTIONS
            INTERFACE_COMPILE_OPTIONS
            COMPILE_FLAGS
    )
        get_target_property(compileOptions ${targetName} ${compileOptionProperty})

        if(compileOptions MATCHES "-NOTFOUND$")
            set(compileOptions "")
        endif()

        foreach(compileOption IN LISTS compileOptions)
            string(TOLOWER "${compileOption}" compileOptionLower)

            if(
                compileOptionLower MATCHES "(^|[:>; \\t])[-/]fi"
                OR compileOptionLower MATCHES "(^|[:>; \\t])-(include|imacros)($|[=,;:<> \\t])"
            )
                message(
                    FATAL_ERROR
                    "${targetName} contains a forced include option in ${compileOptionProperty}: ${compileOption}"
                )
            endif()
        endforeach()
    endforeach()

    get_target_property(targetSources ${targetName} SOURCES)
    get_target_property(targetSourceDirectory ${targetName} SOURCE_DIR)

    foreach(targetSource IN LISTS targetSources)
        get_filename_component(
            targetSourcePath
            "${targetSource}"
            ABSOLUTE
            BASE_DIR "${targetSourceDirectory}"
        )

        foreach(sourceOptionProperty IN ITEMS COMPILE_OPTIONS COMPILE_FLAGS)
            get_source_file_property(
                sourceOptions
                "${targetSourcePath}"
                TARGET_DIRECTORY ${targetName}
                ${sourceOptionProperty}
            )

            if(sourceOptions MATCHES "-NOTFOUND$")
                set(sourceOptions "")
            endif()

            foreach(sourceOption IN LISTS sourceOptions)
                string(TOLOWER "${sourceOption}" sourceOptionLower)

                if(
                    sourceOptionLower MATCHES "(^|[:>; \\t])[-/]fi"
                    OR sourceOptionLower MATCHES "(^|[:>; \\t])-(include|imacros)($|[=,;:<> \\t])"
                )
                    message(
                        FATAL_ERROR
                        "${targetName} source contains a forced include option in ${sourceOptionProperty}: ${targetSource}: ${sourceOption}"
                    )
                endif()
            endforeach()
        endforeach()
    endforeach()
endfunction()

function(cue_verify_platform_link_options targetName)
    foreach(
        linkOptionProperty
        IN ITEMS
            LINK_OPTIONS
            INTERFACE_LINK_OPTIONS
            LINK_FLAGS
            STATIC_LIBRARY_OPTIONS
    )
        get_target_property(linkOptions ${targetName} ${linkOptionProperty})

        if(linkOptions MATCHES "-NOTFOUND$")
            set(linkOptions "")
        endif()

        foreach(linkOption IN LISTS linkOptions)
            string(TOLOWER "${linkOption}" linkOptionLower)

            if(
                linkOptionLower MATCHES "defaultlib"
                OR linkOptionLower MATCHES "wholearchive"
                OR linkOptionLower MATCHES "\\.lib($|[,;:<> \\t])"
                OR linkOptionLower MATCHES "(^|[,;:<> \\t])-l[a-z0-9_]"
            )
                message(
                    FATAL_ERROR
                    "${targetName} contains a library-injecting link option in ${linkOptionProperty}: ${linkOption}"
                )
            endif()
        endforeach()
    endforeach()
endfunction()

get_target_property(cuePlatformLinks Cue.Platform LINK_LIBRARIES)
get_target_property(cuePlatformInterfaceLinks Cue.Platform INTERFACE_LINK_LIBRARIES)
get_target_property(cuePlatformManualDependencies Cue.Platform MANUALLY_ADDED_DEPENDENCIES)
get_target_property(cuePlatformInterfaceSources Cue.Platform INTERFACE_SOURCES)
get_target_property(
    cuePlatformInterfaceDirectLinks
    Cue.Platform
    INTERFACE_LINK_LIBRARIES_DIRECT
)
get_target_property(cuePlatformSources Cue.Platform SOURCES)
cue_collect_platform_header_files(Cue.Platform cuePlatformHeaders)
cue_collect_platform_cxx_module_files(Cue.Platform cuePlatformModules)
cue_collect_platform_precompile_headers(
    Cue.Platform
    cuePlatformPrecompileHeaders
    cuePlatformDirectPrecompileHeaders
)
cue_verify_platform_forced_include_options(Cue.Platform)
cue_verify_platform_link_options(Cue.Platform)
get_target_property(cuePlatformSourceDirectory Cue.Platform SOURCE_DIR)
get_target_property(cuePlatformWindowsLinks Cue.Platform.Windows LINK_LIBRARIES)
get_target_property(
    cuePlatformWindowsInterfaceLinks
    Cue.Platform.Windows
    INTERFACE_LINK_LIBRARIES
)
get_target_property(
    cuePlatformWindowsManualDependencies
    Cue.Platform.Windows
    MANUALLY_ADDED_DEPENDENCIES
)
get_target_property(
    cuePlatformWindowsInterfaceSources
    Cue.Platform.Windows
    INTERFACE_SOURCES
)
get_target_property(
    cuePlatformWindowsInterfaceDirectLinks
    Cue.Platform.Windows
    INTERFACE_LINK_LIBRARIES_DIRECT
)
get_target_property(cuePlatformWindowsSources Cue.Platform.Windows SOURCES)
cue_collect_platform_header_files(Cue.Platform.Windows cuePlatformWindowsHeaders)
cue_collect_platform_cxx_module_files(Cue.Platform.Windows cuePlatformWindowsModules)
cue_collect_platform_precompile_headers(
    Cue.Platform.Windows
    cuePlatformWindowsPrecompileHeaders
    cuePlatformWindowsDirectPrecompileHeaders
)
cue_verify_platform_forced_include_options(Cue.Platform.Windows)
cue_verify_platform_link_options(Cue.Platform.Windows)
get_target_property(
    cuePlatformWindowsSourceDirectory
    Cue.Platform.Windows
    SOURCE_DIR
)

foreach(
    targetProperty
    IN ITEMS
        cuePlatformManualDependencies
        cuePlatformInterfaceSources
        cuePlatformInterfaceDirectLinks
        cuePlatformWindowsManualDependencies
        cuePlatformWindowsInterfaceSources
        cuePlatformWindowsInterfaceDirectLinks
)
    if(${targetProperty} MATCHES "-NOTFOUND$")
        set(${targetProperty} "")
    endif()
endforeach()

if(NOT "${cuePlatformLinks}" STREQUAL "Cue.Foundation")
    message(FATAL_ERROR "Cue.Platform must link only Cue.Foundation: ${cuePlatformLinks}")
endif()

if(NOT "${cuePlatformInterfaceLinks}" STREQUAL "Cue.Foundation")
    message(
        FATAL_ERROR
        "Cue.Platform must expose only Cue.Foundation: ${cuePlatformInterfaceLinks}"
    )
endif()

set(cuePlatformWindowsAllowedLinks Cue.Platform Cue.Foundation.Windows Ole32 Shell32 User32)
set(cuePlatformWindowsAllowedLinkOnlyLibraries Cue.Foundation.Windows Ole32 Shell32 User32)

foreach(cuePlatformWindowsInterfaceLink IN LISTS cuePlatformWindowsInterfaceLinks)
    if(cuePlatformWindowsInterfaceLink STREQUAL "Cue.Platform")
        continue()
    endif()

    if(cuePlatformWindowsInterfaceLink MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
        list(
            FIND
            cuePlatformWindowsAllowedLinkOnlyLibraries
            "${CMAKE_MATCH_1}"
            cuePlatformWindowsLinkOnlyIndex
        )

        if(NOT cuePlatformWindowsLinkOnlyIndex EQUAL -1)
            continue()
        endif()
    endif()

    message(
        FATAL_ERROR
        "Cue.Platform.Windows contains an unapproved interface dependency: ${cuePlatformWindowsInterfaceLink}"
    )
endforeach()

list(
    FIND
    cuePlatformWindowsInterfaceLinks
    Cue.Platform
    cuePlatformWindowsPlatformInterfaceLinkIndex
)

if(cuePlatformWindowsPlatformInterfaceLinkIndex EQUAL -1)
    message(FATAL_ERROR "Cue.Platform.Windows must expose Cue.Platform")
endif()

foreach(cuePlatformWindowsLink IN LISTS cuePlatformWindowsLinks)
    list(
        FIND
        cuePlatformWindowsAllowedLinks
        "${cuePlatformWindowsLink}"
        cuePlatformWindowsLinkIndex
    )

    if(cuePlatformWindowsLinkIndex EQUAL -1)
        message(
            FATAL_ERROR
            "Cue.Platform.Windows contains an unapproved link dependency: ${cuePlatformWindowsLink}"
        )
    endif()
endforeach()

list(FIND cuePlatformWindowsLinks Cue.Platform cuePlatformWindowsPlatformLinkIndex)

if(cuePlatformWindowsPlatformLinkIndex EQUAL -1)
    message(FATAL_ERROR "Cue.Platform.Windows must link Cue.Platform")
endif()

list(
    FIND
    cuePlatformWindowsLinks
    Cue.Foundation.Windows
    cuePlatformWindowsFoundationWindowsLinkIndex
)

if(cuePlatformWindowsFoundationWindowsLinkIndex EQUAL -1)
    message(FATAL_ERROR "Cue.Platform.Windows must privately link Cue.Foundation.Windows")
endif()

list(
    FIND
    cuePlatformWindowsInterfaceLinks
    "$<LINK_ONLY:Cue.Foundation.Windows>"
    cuePlatformWindowsFoundationWindowsLinkOnlyIndex
)

if(cuePlatformWindowsFoundationWindowsLinkOnlyIndex EQUAL -1)
    message(FATAL_ERROR "Cue.Platform.Windows must expose Cue.Foundation.Windows only through LINK_ONLY")
endif()

foreach(
    targetProperty
    IN ITEMS
        cuePlatformManualDependencies
        cuePlatformInterfaceSources
        cuePlatformInterfaceDirectLinks
        cuePlatformWindowsManualDependencies
        cuePlatformWindowsInterfaceSources
        cuePlatformWindowsInterfaceDirectLinks
)
    if(NOT "${${targetProperty}}" STREQUAL "")
        message(
            FATAL_ERROR
            "Platform targets must not expose non-link target edges: ${targetProperty}=${${targetProperty}}"
        )
    endif()
endforeach()

set(cuePlatformTargetNames Cue.Platform Cue.Platform.Windows)
set(cuePlatformTargetSources cuePlatformSources cuePlatformWindowsSources)

foreach(targetIndex RANGE 0 1)
    list(GET cuePlatformTargetNames ${targetIndex} targetName)
    list(GET cuePlatformTargetSources ${targetIndex} targetSourcesVariable)
    set(targetSources "${${targetSourcesVariable}}")

    if(targetSources MATCHES "\\$<TARGET_OBJECTS:" OR targetSources MATCHES "\\$<")
        message(
            FATAL_ERROR
            "${targetName} must not consume generated target sources: ${targetSources}"
        )
    endif()

    foreach(targetSource IN LISTS targetSources)
        get_source_file_property(
            isGeneratedSource
            "${targetSource}"
            TARGET_DIRECTORY ${targetName}
            GENERATED
        )

        if(isGeneratedSource)
            message(
                FATAL_ERROR
                "${targetName} must not consume generated sources: ${targetSource}"
            )
        endif()
    endforeach()
endforeach()

set(cuePlatformTargetFiles "")

foreach(
    targetFile
    IN LISTS
        cuePlatformSources
        cuePlatformHeaders
        cuePlatformModules
        cuePlatformPrecompileHeaders
)
    get_filename_component(
        targetFilePath
        "${targetFile}"
        ABSOLUTE
        BASE_DIR "${cuePlatformSourceDirectory}"
    )
    list(APPEND cuePlatformTargetFiles "${targetFilePath}")
endforeach()

foreach(
    targetFile
    IN LISTS
        cuePlatformWindowsSources
        cuePlatformWindowsHeaders
        cuePlatformWindowsModules
        cuePlatformWindowsPrecompileHeaders
)
    get_filename_component(
        targetFilePath
        "${targetFile}"
        ABSOLUTE
        BASE_DIR "${cuePlatformWindowsSourceDirectory}"
    )
    list(APPEND cuePlatformTargetFiles "${targetFilePath}")
endforeach()

list(REMOVE_DUPLICATES cuePlatformTargetFiles)
string(REPLACE ";" "\n" cuePlatformTargetFileContent "${cuePlatformTargetFiles}")
set(
    cuePlatformTargetFileList
    "${CMAKE_CURRENT_BINARY_DIR}/Cue.Platform.TargetFiles.txt"
)
file(
    GENERATE
    OUTPUT "${cuePlatformTargetFileList}"
    CONTENT "${cuePlatformTargetFileContent}\n"
)

set(
    cuePlatformDirectPrecompileHeaderList
    "${CMAKE_CURRENT_BINARY_DIR}/Cue.Platform.DirectPrecompileHeaders.txt"
)
set(
    cuePlatformDirectPrecompileHeaders
    ${cuePlatformDirectPrecompileHeaders}
    ${cuePlatformWindowsDirectPrecompileHeaders}
)
string(
    REPLACE
    ";"
    "\n"
    cuePlatformDirectPrecompileHeaderContent
    "${cuePlatformDirectPrecompileHeaders}"
)
file(
    GENERATE
    OUTPUT "${cuePlatformDirectPrecompileHeaderList}"
    CONTENT "${cuePlatformDirectPrecompileHeaderContent}\n"
)

set(
    cuePlatformDependencyReport
    "${CMAKE_CURRENT_BINARY_DIR}/Cue.Platform.Dependencies.txt"
)

configure_file(
    PlatformDependencyReport.txt.in
    "${cuePlatformDependencyReport}"
    @ONLY
)

add_custom_target(
    Cue.Platform.Tests
    DEPENDS
        CuePlatformPublicHeaderTest
        CuePlatformFileDialogTests
        CuePlatformWindowsPublicHeaderTest
        CuePlatformWindowsFileDialogTests
        CuePlatformWindowsLifecycleTests
        CuePlatformWindowsMessageSinkTests
        CuePlatformWindowsSystemCapabilityTests
)

add_test(NAME Cue.Platform.PublicHeaders COMMAND CuePlatformPublicHeaderTest)
add_test(NAME Cue.Platform.FileDialog COMMAND CuePlatformFileDialogTests)
add_test(NAME Cue.Platform.Windows.PublicHeaders COMMAND CuePlatformWindowsPublicHeaderTest)
add_test(NAME Cue.Platform.Windows.FileDialog COMMAND CuePlatformWindowsFileDialogTests)
add_test(NAME Cue.Platform.Windows.SystemCapabilities COMMAND CuePlatformWindowsSystemCapabilityTests)
add_test(NAME Cue.Platform.Windows.MessageSink COMMAND CuePlatformWindowsMessageSinkTests)
add_test(
    NAME Cue.Platform.Windows.Lifecycle
    COMMAND CuePlatformWindowsLifecycleTests Lifecycle
)
add_test(
    NAME Cue.Platform.Windows.Events
    COMMAND CuePlatformWindowsLifecycleTests Events
)
add_test(
    NAME Cue.Platform.Windows.LifecycleProbeValidation
    COMMAND CuePlatformWindowsLifecycleTests LifecycleProbeValidation
)
add_test(
    NAME Cue.Platform.Windows.InvalidShow
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CuePlatformWindowsLifecycleTests>
        -DTEST_MODE=InvalidShow
        -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,75>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyProcessExit.cmake
)
add_test(
    NAME Cue.Platform.Windows.InvalidNativeView
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CuePlatformWindowsLifecycleTests>
        -DTEST_MODE=InvalidNativeView
        -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,75>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyProcessExit.cmake
)
add_test(
    NAME Cue.Platform.Windows.ThreadViolation
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CuePlatformWindowsLifecycleTests>
        -DTEST_MODE=ThreadViolation
        -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,75>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyProcessExit.cmake
)

add_test(
    NAME Cue.Platform.Dependencies
    COMMAND
        ${CMAKE_COMMAND}
        -DREPORT_FILE=${cuePlatformDependencyReport}
        -DPLATFORM_SOURCE_DIR=${PROJECT_SOURCE_DIR}/Engine/Source/Platform
        -DPLATFORM_TARGET_FILE_LIST=${cuePlatformTargetFileList}
        -DPLATFORM_DIRECT_PRECOMPILE_HEADER_LIST=${cuePlatformDirectPrecompileHeaderList}
        -DWINDOWS_SDK_VERSION=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyPlatformDependencies.cmake
)

set_tests_properties(
    Cue.Platform.PublicHeaders
    Cue.Platform.FileDialog
    Cue.Platform.Windows.PublicHeaders
    Cue.Platform.Windows.FileDialog
    Cue.Platform.Windows.SystemCapabilities
    Cue.Platform.Windows.MessageSink
    Cue.Platform.Windows.Lifecycle
    Cue.Platform.Windows.Events
    Cue.Platform.Windows.LifecycleProbeValidation
    Cue.Platform.Windows.InvalidShow
    Cue.Platform.Windows.InvalidNativeView
    Cue.Platform.Windows.ThreadViolation
    Cue.Platform.Dependencies
    PROPERTIES
        LABELS Platform
        TIMEOUT 10
)

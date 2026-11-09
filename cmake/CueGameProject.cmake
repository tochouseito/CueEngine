include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/CueGamePlatforms.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CueSemVer.cmake")

# Editor の起動・Build だけでは Sample を選ばず、明示した Project の契約だけを処理する
set(CUE_GAME_PROJECT ""
    CACHE FILEPATH "Cue project whose game module is built with the engine")

# C++ Project Parser と同じ、File 名にも利用できる Target 識別子へ制限する
function(cue_validate_game_target a_target)
    string(LENGTH "${a_target}" taskLength)
    string(TOLOWER "${a_target}" taskLower)
    set(taskReserved all all_build zero_check install clean test help package package_source rebuild
        edit_cache rebuild_cache run_tests con prn aux nul)
    if(taskLength GREATER 128 OR NOT "${a_target}" MATCHES "^[A-Za-z_][A-Za-z0-9_-]*$"
       OR taskLower IN_LIST taskReserved OR taskLower MATCHES "^(com|lpt)[1-9]$")
        message(FATAL_ERROR "Cue gameTarget is invalid: ${a_target}")
    endif()
endfunction()

# CMake の List / Generator Expression として再解釈される配置を受け付けない
function(cue_validate_build_path a_path)
    if("${a_path}" MATCHES ";|\\$<")
        message(FATAL_ERROR "Cue build paths cannot contain a semicolon or a generator expression: ${a_path}")
    endif()
endfunction()

# file(STRINGS) が不正 Byte を除去する前に、元の UTF-8 を過長表現 / Surrogate も含めて検証する
function(cue_validate_project_utf8 a_hex)
    string(REGEX MATCHALL ".." taskBytes "${a_hex}")
    set(taskRemaining 0)
    set(taskMinimum 128)
    set(taskMaximum 191)
    foreach(taskHex IN LISTS taskBytes)
        math(EXPR taskByte "0x${taskHex}")
        if(taskRemaining GREATER 0)
            if(taskByte LESS taskMinimum OR taskByte GREATER taskMaximum)
                message(FATAL_ERROR "Cue project is not valid UTF-8")
            endif()
            math(EXPR taskRemaining "${taskRemaining} - 1")
            set(taskMinimum 128)
            set(taskMaximum 191)
        elseif(taskByte EQUAL 0)
            message(FATAL_ERROR "Cue project contains a NUL byte")
        elseif(taskByte LESS 128)
            continue()
        elseif(taskByte GREATER_EQUAL 194 AND taskByte LESS_EQUAL 223)
            set(taskRemaining 1)
        elseif(taskByte GREATER_EQUAL 224 AND taskByte LESS_EQUAL 239)
            set(taskRemaining 2)
            if(taskByte EQUAL 224)
                set(taskMinimum 160)
            elseif(taskByte EQUAL 237)
                set(taskMaximum 159)
            endif()
        elseif(taskByte GREATER_EQUAL 240 AND taskByte LESS_EQUAL 244)
            set(taskRemaining 3)
            if(taskByte EQUAL 240)
                set(taskMinimum 144)
            elseif(taskByte EQUAL 244)
                set(taskMaximum 143)
            endif()
        else()
            message(FATAL_ERROR "Cue project is not valid UTF-8")
        endif()
    endforeach()
    if(NOT taskRemaining EQUAL 0)
        message(FATAL_ERROR "Cue project has an incomplete UTF-8 sequence")
    endif()
endfunction()

# 既存の親も含め Link / Junction を越える Build 出力を拒否する
function(cue_validate_real_path a_path)
    # CMake 4.2 の Windows Unicode Path で file(REAL_PATH) / cmake_path が異常終了するため旧 API を使う
    get_filename_component(taskPath "${a_path}" ABSOLUTE)
    while(NOT EXISTS "${taskPath}")
        get_filename_component(taskParent "${taskPath}" DIRECTORY)
        if(taskParent STREQUAL taskPath OR taskParent STREQUAL "")
            message(FATAL_ERROR "Cue build path has no existing parent: ${a_path}")
        endif()
        set(taskPath "${taskParent}")
    endwhile()
    get_filename_component(taskReal "${taskPath}" REALPATH)
    string(TOLOWER "${taskPath}" taskExpected)
    string(TOLOWER "${taskReal}" taskActual)
    if(NOT taskActual STREQUAL taskExpected)
        message(FATAL_ERROR "Cue build path traverses a link or junction: ${a_path}")
    endif()
endfunction()

# Directory 境界付きの Prefix を比較する。Windows の大小文字だけを同一視する
function(cue_path_is_within a_root a_path a_output)
    get_filename_component(taskRoot "${a_root}" ABSOLUTE)
    get_filename_component(taskPath "${a_path}" ABSOLUTE)
    string(TOLOWER "${taskRoot}" taskRoot)
    string(TOLOWER "${taskPath}" taskPath)
    string(REGEX REPLACE "/+$" "" taskRoot "${taskRoot}")
    string(FIND "${taskPath}/" "${taskRoot}/" taskPosition)
    if(taskPosition EQUAL 0)
        set(${a_output} TRUE PARENT_SCOPE)
    else()
        set(${a_output} FALSE PARENT_SCOPE)
    endif()
endfunction()

# 生成先の途中に存在する通常 File や Link を、書込みより前に検出する
function(cue_validate_generated_directory a_path)
    if(EXISTS "${a_path}" AND NOT IS_DIRECTORY "${a_path}")
        message(FATAL_ERROR "Cue generated directory is a regular file: ${a_path}")
    endif()
    cue_validate_real_path("${a_path}")
endfunction()

# 保存用相対 Directory を Project Root 内へ解決する。Source / Content は既存 Directory が必要
function(cue_resolve_project_directory a_root a_relative a_key a_output)
    cue_validate_build_path("${a_relative}")
    if(IS_ABSOLUTE "${a_relative}" OR "${a_relative}" MATCHES ":")
        message(FATAL_ERROR "Cue ${a_key} must be relative: ${a_relative}")
    endif()
    get_filename_component(taskPath "${a_root}/${a_relative}" ABSOLUTE)
    if(taskPath MATCHES "(^|/)[^/]*[. ](/|$)")
        message(FATAL_ERROR "Cue directory uses a Windows trailing-dot/space alias: ${a_relative}")
    endif()
    cue_path_is_within("${a_root}" "${taskPath}" taskInside)
    if(NOT taskInside OR NOT IS_DIRECTORY "${taskPath}")
        message(FATAL_ERROR "Cue ${a_key} is not an existing project directory: ${a_relative}")
    endif()
    cue_validate_real_path("${taskPath}")
    set(${a_output} "${taskPath}" PARENT_SCOPE)
endfunction()

# Source の一覧と依存はゲーム側 CMake の一箇所に置き、DLL / Static に共用する
function(cue_add_game_module a_target)
    cmake_parse_arguments(PARSE_ARGV 1 taskGame "" "" "SOURCES;LIBRARIES")
    cue_validate_game_target("${a_target}")
    if(NOT DEFINED _CUE_GAME_TARGET OR NOT "${a_target}" STREQUAL "${_CUE_GAME_TARGET}")
        message(FATAL_ERROR "Cue game CMake target does not match gameTarget: ${a_target}")
    endif()
    if(taskGame_UNPARSED_ARGUMENTS OR NOT taskGame_SOURCES OR TARGET "${a_target}" OR TARGET "${a_target}.Static")
        message(FATAL_ERROR "Cue game module declaration is invalid or duplicated: ${a_target}")
    endif()
    add_library(${a_target} SHARED ${taskGame_SOURCES})
    add_library(${a_target}.Static STATIC ${taskGame_SOURCES})
    foreach(taskTarget IN ITEMS ${a_target} ${a_target}.Static)
        target_link_libraries(${taskTarget} PRIVATE Cue.Script ${taskGame_LIBRARIES})
        target_compile_features(${taskTarget} PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(${taskTarget} PRIVATE /utf-8 $<$<CONFIG:Development>:/O2> $<$<CONFIG:Development>:/Zi>)
        endif()
    endforeach()
    set_target_properties(${a_target} PROPERTIES
        OUTPUT_NAME "${a_target}"
        PREFIX ""
        SUFFIX "${_CUE_GAME_MODULE_SUFFIX}"
        DEBUG_POSTFIX ""
        DEVELOPMENT_POSTFIX ""
        RELEASE_POSTFIX ""
        ARCHIVE_OUTPUT_NAME "${a_target}Import"
        RUNTIME_OUTPUT_DIRECTORY "${_CUE_GAME_OUTPUT}/$<CONFIG>/Runtime"
        ARCHIVE_OUTPUT_DIRECTORY "${_CUE_GAME_OUTPUT}/$<CONFIG>/Libraries"
        LIBRARY_OUTPUT_DIRECTORY "${_CUE_GAME_OUTPUT}/$<CONFIG>/Runtime"
        PDB_OUTPUT_DIRECTORY "${_CUE_GAME_OUTPUT}/$<CONFIG>/Runtime"
        PDB_NAME "${a_target}"
    )
    if(MSVC)
        # 開発 DLL の構成と同じ PDB を隣接配置し、製品 Static の配布契約とは分離する
        target_compile_options(${a_target} PRIVATE /Zi)
        target_link_options(${a_target} PRIVATE /DEBUG:FULL)
    endif()
    set_target_properties(${a_target}.Static PROPERTIES
        OUTPUT_NAME "${a_target}Static"
        PREFIX ""
        SUFFIX "${_CUE_GAME_STATIC_SUFFIX}"
        DEBUG_POSTFIX ""
        DEVELOPMENT_POSTFIX ""
        RELEASE_POSTFIX ""
        ARCHIVE_OUTPUT_DIRECTORY "${_CUE_GAME_OUTPUT}/$<CONFIG>/Libraries"
    )
endfunction()

# 選択した .cueproj だけを Build Graph に取り込む。未指定 / gameTarget のない旧 Project はゲームを追加しない
function(cue_add_game_project a_file)
    cmake_parse_arguments(PARSE_ARGV 1 taskProject "NO_PLAYER" "" "")
    if(taskProject_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unsupported Cue game project options")
    endif()
    if("${a_file}" STREQUAL "")
        return()
    endif()
    cue_validate_build_path("${a_file}")
    get_filename_component(taskFile "${a_file}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    if(NOT EXISTS "${taskFile}" OR IS_DIRECTORY "${taskFile}" OR NOT taskFile MATCHES "\\.cueproj$")
        message(FATAL_ERROR "Cue project file is missing or invalid: ${taskFile}")
    endif()
    cue_validate_real_path("${taskFile}")
    file(SIZE "${taskFile}" taskSize)
    if(taskSize GREATER 65536)
        message(FATAL_ERROR "Cue project exceeds 64 KiB: ${taskFile}")
    endif()
    file(READ "${taskFile}" taskHex HEX)
    cue_validate_project_utf8("${taskHex}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${taskFile}")
    file(STRINGS "${taskFile}" taskLines ENCODING UTF-8)
    set(taskKnown formatVersion projectId name applicationName sourceRoot contentRoot startupScene gameTarget engineVersion)
    set(taskSeen)
    foreach(taskLine IN LISTS taskLines)
        string(STRIP "${taskLine}" taskLine)
        if(taskLine STREQUAL "" OR taskLine MATCHES "^#")
            continue()
        endif()
        if(NOT taskLine MATCHES "^([^=]+)=(.*)$")
            message(FATAL_ERROR "Cue project line is invalid: ${taskFile}")
        endif()
        string(STRIP "${CMAKE_MATCH_1}" taskKey)
        string(STRIP "${CMAKE_MATCH_2}" taskValue)
        if(NOT taskKey IN_LIST taskKnown OR taskKey IN_LIST taskSeen OR taskValue STREQUAL "")
            message(FATAL_ERROR "Cue project key is invalid or duplicated: ${taskKey}")
        endif()
        list(APPEND taskSeen "${taskKey}")
        set("task_${taskKey}" "${taskValue}")
    endforeach()
    foreach(taskKey IN ITEMS formatVersion projectId name applicationName sourceRoot contentRoot)
        if(NOT taskKey IN_LIST taskSeen)
            message(FATAL_ERROR "Cue project key is missing: ${taskKey}")
        endif()
    endforeach()
    if(NOT task_formatVersion STREQUAL "1" AND NOT task_formatVersion STREQUAL "2")
        message(FATAL_ERROR "Cue project formatVersion is unsupported: ${task_formatVersion}")
    endif()
    if(task_formatVersion STREQUAL "2")
        if(NOT DEFINED task_engineVersion)
            message(FATAL_ERROR "Cue project engineVersion is required for formatVersion 2")
        endif()
        cue_validate_engine_version("${task_engineVersion}")
        if(NOT DEFINED CUE_ENGINE_VERSION OR NOT task_engineVersion STREQUAL CUE_ENGINE_VERSION)
            message(FATAL_ERROR "Cue project engineVersion does not match this Engine: ${task_engineVersion}")
        endif()
    elseif(DEFINED task_engineVersion)
        message(FATAL_ERROR "Cue engineVersion requires formatVersion 2")
    endif()
    string(LENGTH "${task_projectId}" taskIdLength)
    if(NOT taskIdLength EQUAL 36)
        message(FATAL_ERROR "Cue projectId is invalid")
    endif()
    string(REPLACE "-" "" taskIdDigits "${task_projectId}")
    string(LENGTH "${taskIdDigits}" taskIdDigitsLength)
    set(taskIdSeparators "")
    foreach(taskIndex IN ITEMS 8 13 18 23)
        string(SUBSTRING "${task_projectId}" ${taskIndex} 1 taskIdSeparator)
        string(APPEND taskIdSeparators "${taskIdSeparator}")
    endforeach()
    if(NOT taskIdDigitsLength EQUAL 32 OR NOT taskIdSeparators STREQUAL "----"
       OR NOT taskIdDigits MATCHES "^[0-9a-fA-F]+$" OR taskIdDigits STREQUAL "00000000000000000000000000000000")
        message(FATAL_ERROR "Cue projectId is invalid")
    endif()
    if(NOT DEFINED task_gameTarget)
        return()
    endif()
    cue_validate_game_target("${task_gameTarget}")
    if(NOT "${CUE_GAME_SOLUTION_NAME}" STREQUAL "" AND
       NOT "${CUE_GAME_SOLUTION_NAME}" STREQUAL "${task_applicationName}")
        message(FATAL_ERROR "Cue game solution name does not match the project's applicationName")
    endif()
    cue_select_game_build_target()
    get_filename_component(taskRoot "${taskFile}" DIRECTORY)
    cue_resolve_project_directory("${taskRoot}" "${task_sourceRoot}" sourceRoot taskSource)
    cue_resolve_project_directory("${taskRoot}" "${task_contentRoot}" contentRoot taskContent)
    if(NOT EXISTS "${taskSource}/CMakeLists.txt" OR IS_DIRECTORY "${taskSource}/CMakeLists.txt")
        message(FATAL_ERROR "Cue Source/CMakeLists.txt is missing: ${taskSource}")
    endif()
    cue_validate_real_path("${taskSource}/CMakeLists.txt")
    set(taskBuild "${taskRoot}/Build")
    set(taskIntermediate "${taskRoot}/Intermediate")
    foreach(taskOutput IN ITEMS "${taskBuild}" "${taskIntermediate}")
        cue_validate_generated_directory("${taskOutput}")
        foreach(taskInput IN ITEMS "${taskSource}" "${taskContent}")
            cue_path_is_within("${taskInput}" "${taskOutput}" taskOutputInside)
            cue_path_is_within("${taskOutput}" "${taskInput}" taskInputInside)
            if(taskOutputInside OR taskInputInside)
                message(FATAL_ERROR "Cue generated directories overlap Source / Content: ${taskOutput}")
            endif()
        endforeach()
    endforeach()
    string(SHA256 taskBuildHash "${CMAKE_BINARY_DIR}")
    string(SUBSTRING "${taskBuildHash}" 0 16 taskBuildHash)
    set(taskBinary "${taskIntermediate}/Build/${taskBuildHash}")
    cue_validate_generated_directory("${taskBinary}")
    set(_CUE_GAME_TARGET "${task_gameTarget}")
    set(_CUE_GAME_OUTPUT "${taskBuild}/${_CUE_GAME_PLATFORM}/${_CUE_GAME_ARCHITECTURE}")
    set(taskConfigurations ${CMAKE_CONFIGURATION_TYPES})
    if(NOT taskConfigurations)
        set(taskConfigurations "${CMAKE_BUILD_TYPE}")
    endif()
    foreach(taskConfiguration IN LISTS taskConfigurations)
        if(NOT taskConfiguration MATCHES "^(Debug|Development|Release)$")
            message(FATAL_ERROR "Cue game configuration is unsupported: ${taskConfiguration}")
        endif()
        cue_validate_generated_directory("${_CUE_GAME_OUTPUT}/${taskConfiguration}/Runtime")
        cue_validate_generated_directory("${_CUE_GAME_OUTPUT}/${taskConfiguration}/Libraries")
        cue_validate_generated_directory("${taskBinary}/Manifest/${taskConfiguration}")
    endforeach()
    add_subdirectory("${taskSource}" "${taskBinary}")
    if(NOT TARGET "${task_gameTarget}" OR NOT TARGET "${task_gameTarget}.Static")
        message(FATAL_ERROR "Cue gameTarget is not declared by Source/CMakeLists.txt: ${task_gameTarget}")
    endif()
    get_target_property(taskDynamicType "${task_gameTarget}" TYPE)
    get_target_property(taskStaticType "${task_gameTarget}.Static" TYPE)
    if(NOT taskDynamicType STREQUAL "SHARED_LIBRARY" OR NOT taskStaticType STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "Cue gameTarget must provide a shared module and a static library")
    endif()
    set(taskManifest
        "formatVersion=1\nprojectId=${task_projectId}\ngameTarget=${task_gameTarget}\nconfiguration=$<CONFIG>\nplatform=${_CUE_GAME_PLATFORM}\narchitecture=${_CUE_GAME_ARCHITECTURE}\nabiVersion=1\nmodule=Runtime/${task_gameTarget}${_CUE_GAME_MODULE_SUFFIX}\nstaticLibrary=Libraries/${task_gameTarget}Static${_CUE_GAME_STATIC_SUFFIX}\n")
    file(GENERATE OUTPUT "${taskBinary}/Manifest/$<CONFIG>/GameModule.cuebuild" CONTENT "${taskManifest}")
    # 両方の Link 成功後だけ、Hash 付き Manifest を原子的に公開する
    add_custom_target(Cue.GameProjectBuild ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DCUE_MANIFEST_TEMPLATE=${taskBinary}/Manifest/$<CONFIG>/GameModule.cuebuild"
            "-DCUE_MANIFEST_OUTPUT=${_CUE_GAME_OUTPUT}/$<CONFIG>/GameModule.cuebuild"
            "-DCUE_GAME_MODULE=$<TARGET_FILE:${task_gameTarget}>"
            "-DCUE_GAME_STATIC=$<TARGET_FILE:${task_gameTarget}.Static>"
            "-DCUE_EXPECTED_MODULE=${_CUE_GAME_OUTPUT}/$<CONFIG>/Runtime/${task_gameTarget}${_CUE_GAME_MODULE_SUFFIX}"
            "-DCUE_EXPECTED_STATIC=${_CUE_GAME_OUTPUT}/$<CONFIG>/Libraries/${task_gameTarget}Static${_CUE_GAME_STATIC_SUFFIX}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PublishGameModule.cmake"
        DEPENDS ${task_gameTarget} ${task_gameTarget}.Static
        VERBATIM
    )
    set_property(GLOBAL PROPERTY CUE_CONFIGURED_GAME_TARGET "${task_gameTarget}")
    set_property(GLOBAL PROPERTY CUE_CONFIGURED_GAME_PROJECT "${taskFile}")
    set_property(GLOBAL PROPERTY CUE_CONFIGURED_GAME_PROJECT_ID "${task_projectId}")
    # Scene を持たない旧 Project は従来どおり DLL / Static だけを作る
    if(taskProject_NO_PLAYER OR NOT DEFINED task_startupScene)
        message(STATUS "Cue project has no startupScene; only the game module is generated")
        return()
    endif()
    cue_add_game_player("${taskRoot}" "${taskContent}" "${taskBinary}" "${task_projectId}"
                        "${task_gameTarget}" "${task_applicationName}" "${task_name}" "${task_startupScene}")
endfunction()

# 独立アプリとその配布 Target を追加する。Source / Content へは書き込まない
function(cue_add_game_player a_root a_content a_binary a_projectId a_gameTarget a_applicationName a_title a_startupScene)
    cue_validate_game_target("${a_applicationName}")
    if(TARGET "${a_applicationName}" OR TARGET "${a_applicationName}.Package")
        message(FATAL_ERROR "Cue applicationName collides with an existing target: ${a_applicationName}")
    endif()
    cue_validate_build_path("${a_startupScene}")
    if(IS_ABSOLUTE "${a_startupScene}" OR a_startupScene MATCHES ":" OR NOT a_startupScene MATCHES "\\.cuescene$")
        message(FATAL_ERROR "Cue startupScene must be a relative .cuescene file")
    endif()
    get_filename_component(taskScene "${a_content}/${a_startupScene}" ABSOLUTE)
    cue_path_is_within("${a_content}" "${taskScene}" taskSceneInside)
    if(NOT taskSceneInside OR NOT EXISTS "${taskScene}" OR IS_DIRECTORY "${taskScene}")
        message(FATAL_ERROR "Cue startupScene is missing or outside Content: ${a_startupScene}")
    endif()
    cue_validate_real_path("${taskScene}")
    file(RELATIVE_PATH taskSceneRelative "${a_content}" "${taskScene}")
    set(taskRuntime "${a_binary}/Player/$<CONFIG>")
    set(taskPlayer "${_CUE_GAME_OUTPUT}/$<CONFIG>/Player")
    foreach(taskConfiguration IN LISTS CMAKE_CONFIGURATION_TYPES)
        cue_validate_generated_directory("${a_binary}/Player/${taskConfiguration}")
        cue_validate_generated_directory("${_CUE_GAME_OUTPUT}/${taskConfiguration}/Player")
    endforeach()
    cue_add_selected_game_application("${a_applicationName}" "${a_gameTarget}.Static" "${taskRuntime}")
    # 表示名に Generator Expression のような文字列があっても、表示用 Data としてそのまま保存する
    set(taskStartupTemplate "${a_binary}/GameStartup.base.cueapp")
    set(taskPackageTemplate "${a_binary}/GamePackage.base.cuepackage")
    file(WRITE "${taskStartupTemplate}"
        "formatVersion=1\nprojectId=${a_projectId}\ngameTarget=${a_gameTarget}\napplicationName=${a_applicationName}\nplatform=${_CUE_GAME_PLATFORM}\narchitecture=${_CUE_GAME_ARCHITECTURE}\nstartupScene=Content/${taskSceneRelative}\nwindowTitle=${a_title}\nwindowWidth=1280\nwindowHeight=720\ntargetFramesPerSecond=60\n")
    file(WRITE "${taskPackageTemplate}"
        "formatVersion=1\nprojectId=${a_projectId}\ngameTarget=${a_gameTarget}\napplicationName=${a_applicationName}\nplatform=${_CUE_GAME_PLATFORM}\narchitecture=${_CUE_GAME_ARCHITECTURE}\n")
    get_property(taskRuntimeFiles GLOBAL PROPERTY "CUE_GAME_${_CUE_GAME_PLATFORM}_RUNTIME_FILES")
    get_property(taskLicenseFiles GLOBAL PROPERTY "CUE_GAME_${_CUE_GAME_PLATFORM}_LICENSE_FILES")
    string(REPLACE ";" "\n" taskRuntimeList "${taskRuntimeFiles}")
    string(REPLACE ";" "\n" taskLicenseList "${taskLicenseFiles}")
    file(WRITE "${a_binary}/PlayerRuntimeFiles.txt" "${taskRuntimeList}\n")
    file(WRITE "${a_binary}/PlayerLicenseFiles.txt" "${taskLicenseList}\n")
    set(taskPackageCommand "${CMAKE_COMMAND}"
        "-DCUE_PLAYER_SOURCE=$<TARGET_FILE_DIR:${a_applicationName}>"
        "-DCUE_PLAYER_EXECUTABLE=$<TARGET_FILE:${a_applicationName}>"
        "-DCUE_PLAYER_OUTPUT=${taskPlayer}"
        "-DCUE_PLAYER_CONFIGURATION=$<CONFIG>"
        "-DCUE_PLAYER_PROJECT_ID=${a_projectId}"
        "-DCUE_PLAYER_APPLICATION=${a_applicationName}"
        "-DCUE_PLAYER_EXECUTABLE_NAME=${a_applicationName}${_CUE_GAME_APPLICATION_SUFFIX}"
        "-DCUE_STARTUP_TEMPLATE=${taskStartupTemplate}"
        "-DCUE_PACKAGE_TEMPLATE=${taskPackageTemplate}"
        "-DCUE_STARTUP_SCENE=${taskScene}"
        "-DCUE_STARTUP_SCENE_RELATIVE=Content/${taskSceneRelative}"
        "-DCUE_PLAYER_RUNTIME_LIST=${a_binary}/PlayerRuntimeFiles.txt"
        "-DCUE_PLAYER_LICENSE_LIST=${a_binary}/PlayerLicenseFiles.txt"
        -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PublishGamePlayer.cmake")
    add_custom_command(TARGET ${a_applicationName} POST_BUILD COMMAND ${taskPackageCommand} VERBATIM)
    add_custom_target(${a_applicationName}.Package ALL COMMAND ${taskPackageCommand} DEPENDS ${a_applicationName} VERBATIM)
    # Player が共有 Static を更新した後も、Editor の成果物検証情報を最新に保つ
    add_custom_target(Cue.GamePlayerPackage DEPENDS ${a_applicationName}.Package Cue.GameProjectBuild)
    set_target_properties(${a_applicationName} PROPERTIES
        VS_DEBUGGER_COMMAND "${taskPlayer}/${a_applicationName}${_CUE_GAME_APPLICATION_SUFFIX}"
        VS_DEBUGGER_WORKING_DIRECTORY "${taskPlayer}")
    set_property(GLOBAL PROPERTY CUE_CONFIGURED_GAME_APP_TARGET "${a_applicationName}")
    set_property(GLOBAL PROPERTY CUE_CONFIGURED_GAME_PLAYER_DIRECTORY "${taskPlayer}")
    set_property(GLOBAL PROPERTY CUE_CONFIGURED_GAME_PLAYER_EXECUTABLE "${taskPlayer}/${a_applicationName}${_CUE_GAME_APPLICATION_SUFFIX}")
endfunction()

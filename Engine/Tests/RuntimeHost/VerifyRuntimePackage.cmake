if(NOT DEFINED TEST_EXECUTABLE OR NOT EXISTS "${TEST_EXECUTABLE}")
    message(FATAL_ERROR "TEST_EXECUTABLE must identify CueRuntimeHost")
endif()
if(NOT DEFINED MODULE_LIBRARY OR NOT EXISTS "${MODULE_LIBRARY}")
    message(FATAL_ERROR "MODULE_LIBRARY must identify the test Game Module")
endif()
if(NOT DEFINED TEST_ROOT OR TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "TEST_ROOT is required")
endif()
if(NOT CONFIGURATION MATCHES "^(Debug|Development|Release)$")
    message(FATAL_ERROR "CONFIGURATION is invalid: ${CONFIGURATION}")
endif()
if(NOT DEFINED COMPILER_VERSION OR NOT DEFINED COMPILER_FULL_VERSION OR NOT DEFINED COMPILER_BUILD)
    message(FATAL_ERROR "MSVC version values are required")
endif()

set(projectId "41234567-89ab-4cde-8f01-23456789abcd")
set(sceneId "51234567-89ab-4cde-8f01-23456789abcd")
set(stagingRoot "${TEST_ROOT}/BuiltPackage")
set(packageRoot "${TEST_ROOT}/RelocatedPackage")
set(workingRoot "${TEST_ROOT}/UnrelatedWorkingDirectory")
set(projectPath "${stagingRoot}/Data/CueProject.runtime.json")
set(sceneRelativePath "Data/Scenes/${sceneId}.cueruntime.json")
set(scenePath "${stagingRoot}/${sceneRelativePath}")
set(metadataPath "${stagingRoot}/Game/CueGameModule.metadata.json")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${stagingRoot}/Data/Scenes" "${stagingRoot}/Game" "${workingRoot}")
file(WRITE "${workingRoot}/CuePackage.json" "{\"schemaVersion\":999}\n")
file(COPY_FILE "${TEST_EXECUTABLE}" "${stagingRoot}/CueRuntimeHost.exe" ONLY_IF_DIFFERENT)
file(COPY_FILE "${MODULE_LIBRARY}" "${stagingRoot}/Game/CueGameModule.dll" ONLY_IF_DIFFERENT)

file(WRITE "${projectPath}"
    "{\"schemaVersion\":1,\"projectId\":\"${projectId}\",\"engineCompatibility\":{\"minimum\":\"1.0.0\",\"maximumExclusive\":\"2.0.0\"},\"requiredCapabilities\":[],\"startupSceneAssetId\":\"${sceneId}\"}\n")
file(WRITE "${scenePath}"
    "{\"schemaVersion\":1,\"sceneAssetId\":\"${sceneId}\",\"objects\":[]}\n")

if(CONFIGURATION STREQUAL "Debug")
    set(runtimeLibrary "DebugDll")
    set(iteratorDebugLevel 2)
else()
    set(runtimeLibrary "Dll")
    set(iteratorDebugLevel 0)
endif()
file(WRITE "${metadataPath}"
    "{\n    \"schemaVersion\": 1,\n    \"artifactId\": \"runtime-package-probe\",\n    \"projectId\": \"${projectId}\",\n    \"engineCompatibility\": {\n        \"minimum\": \"1.0.0\",\n        \"maximumExclusive\": \"2.0.0\"\n    },\n    \"abiVersion\": 1,\n    \"configuration\": \"${CONFIGURATION}\",\n    \"architecture\": \"x64\",\n    \"compilerFamily\": \"msvc\",\n    \"msvcToolset\": {\n        \"compilerVersion\": ${COMPILER_VERSION},\n        \"fullVersion\": ${COMPILER_FULL_VERSION},\n        \"build\": ${COMPILER_BUILD}\n    },\n    \"runtimeLibrary\": \"${runtimeLibrary}\",\n    \"iteratorDebugLevel\": ${iteratorDebugLevel},\n    \"moduleFile\": \"CueGameModule.dll\",\n    \"entrySymbol\": \"cue_game_module_query\"\n}\n")

set(paths
    "CueRuntimeHost.exe"
    "Data/CueProject.runtime.json"
    "${sceneRelativePath}"
    "Game/CueGameModule.dll"
    "Game/CueGameModule.metadata.json"
)
set(roles
    "runtimeHost"
    "projectRuntimeData"
    "startupSceneRuntimeData"
    "gameModule"
    "gameModuleMetadata"
)
set(filesJson "")
list(LENGTH paths fileCount)
math(EXPR lastFileIndex "${fileCount} - 1")
foreach(index RANGE 0 ${lastFileIndex})
    list(GET paths ${index} relativePath)
    list(GET roles ${index} role)
    set(absolutePath "${stagingRoot}/${relativePath}")
    file(SIZE "${absolutePath}" sizeBytes)
    file(SHA256 "${absolutePath}" sha256)
    if(NOT filesJson STREQUAL "")
        string(APPEND filesJson ",")
    endif()
    string(APPEND filesJson
        "{\"role\":\"${role}\",\"path\":\"${relativePath}\",\"sizeBytes\":${sizeBytes},\"sha256\":\"${sha256}\"}")
endforeach()
file(WRITE "${stagingRoot}/CuePackage.json"
    "{\"schemaVersion\":1,\"projectId\":\"${projectId}\",\"engineVersion\":\"1.0.0\",\"configuration\":\"${CONFIGURATION}\",\"startupScene\":{\"sceneAssetId\":\"${sceneId}\",\"runtimeDataPath\":\"${sceneRelativePath}\"},\"files\":[${filesJson}]}\n")

file(RENAME "${stagingRoot}" "${packageRoot}")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE packageResult
    OUTPUT_VARIABLE packageOutput
    ERROR_VARIABLE packageError
    TIMEOUT 15
)
set(combinedOutput "${packageOutput}\n${packageError}")
if(NOT packageResult EQUAL 0)
    message(FATAL_ERROR "Relocated Runtime Package exited with ${packageResult}\n${combinedOutput}")
endif()
foreach(requiredMessage IN ITEMS
    "Runtime Application Session started: Generation=1, WorldId="
    "Runtime Application Session stopped: Reason=WindowClosed, FrameCount=1"
)
    string(FIND "${combinedOutput}" "${requiredMessage}" messagePosition)
    if(messagePosition EQUAL -1)
        message(FATAL_ERROR "Relocated Runtime Package output is missing: ${requiredMessage}\n${combinedOutput}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${packageRoot}/Runtime")
file(WRITE "${packageRoot}/Runtime/Unlisted.dll" "unlisted")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE unlistedRuntimeResult
    OUTPUT_VARIABLE unlistedRuntimeOutput
    ERROR_VARIABLE unlistedRuntimeError
    TIMEOUT 15
)
if(unlistedRuntimeResult EQUAL 0)
    message(FATAL_ERROR
        "Manifest-external Runtime dependency was accepted\n${unlistedRuntimeOutput}\n${unlistedRuntimeError}")
endif()
file(REMOVE_RECURSE "${packageRoot}/Runtime")

file(APPEND "${packageRoot}/Data/CueProject.runtime.json" "tampered")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE tamperedResult
    OUTPUT_VARIABLE tamperedOutput
    ERROR_VARIABLE tamperedError
    TIMEOUT 15
)
if(tamperedResult EQUAL 0)
    message(FATAL_ERROR "Tampered Runtime Data was accepted\n${tamperedOutput}\n${tamperedError}")
endif()

message(STATUS "Relocated Runtime Package discovery, lifecycle, and tamper rejection: passed")

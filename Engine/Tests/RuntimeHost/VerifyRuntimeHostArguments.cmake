cmake_minimum_required(VERSION 4.2.0)

if(NOT DEFINED TEST_EXECUTABLE)
    message(FATAL_ERROR "TEST_EXECUTABLE is required")
endif()

execute_process(
    COMMAND "${TEST_EXECUTABLE}" --package --smoke-test
    RESULT_VARIABLE combinedModeResult
    OUTPUT_VARIABLE combinedModeOutput
    ERROR_VARIABLE combinedModeError
)

if(NOT combinedModeResult EQUAL 64)
    message(
        FATAL_ERROR
        "Runtime Host accepted overlapping package and smoke modes: ${combinedModeResult}\n${combinedModeOutput}\n${combinedModeError}"
    )
endif()

set(combinedOutput "${combinedModeOutput}\n${combinedModeError}")
string(FIND "${combinedOutput}" "Usage: CueRuntimeHost" usagePosition)
if(usagePosition EQUAL -1)
    message(FATAL_ERROR "Runtime Host invalid mode combination did not emit usage")
endif()

message(STATUS "CueRuntimeHost invalid mode combination: passed")

cmake_minimum_required(VERSION 4.2.0)

if(NOT DEFINED TEST_EXECUTABLE)
    message(FATAL_ERROR "TEST_EXECUTABLE is required")
endif()

execute_process(
    COMMAND
        "${TEST_EXECUTABLE}"
        --smoke-test
        --title "CueEngine Runtime Host 日本語"
        --width 640
        --height 360
    RESULT_VARIABLE testResult
    OUTPUT_VARIABLE testOutput
    ERROR_VARIABLE testError
)

if(NOT testResult EQUAL 0)
    message(FATAL_ERROR "Runtime Application smoke exited with ${testResult}\n${testOutput}\n${testError}")
endif()

set(combinedOutput "${testOutput}\n${testError}")
set(previousPosition -1)

foreach(
    requiredMessage
    IN ITEMS
        "Runtime Application Session started: Generation=1, WorldId="
        "Runtime Host Main Loop started"
        "Runtime Application Session stopped: Reason=WindowClosed, FrameCount=0"
        "Runtime Host shutdown completed"
)
    string(FIND "${combinedOutput}" "${requiredMessage}" messagePosition)

    if(messagePosition EQUAL -1)
        message(FATAL_ERROR "Runtime Application smoke output is missing: ${requiredMessage}\n${combinedOutput}")
    endif()

    if(messagePosition LESS_EQUAL previousPosition)
        message(FATAL_ERROR "Runtime Application smoke output is out of order: ${requiredMessage}\n${combinedOutput}")
    endif()

    set(previousPosition ${messagePosition})
endforeach()

string(FIND "${combinedOutput}" "[Error]" errorPosition)

if(NOT errorPosition EQUAL -1)
    message(FATAL_ERROR "Runtime Application smoke emitted an Error\n${combinedOutput}")
endif()

message(STATUS "Runtime Application Window Close smoke: passed")

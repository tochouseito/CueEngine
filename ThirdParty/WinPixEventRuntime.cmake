# 公式配布 Archive の版と Hash を固定し、NuGet restore なしで PIX Event API を利用する
set(CUE_PIX_VERSION "1.0.240308001")
set(CUE_PIX_SHA256 "726acc93d6968e2146261a1e415521747d50ad69894c2b42b5d0d4c29fd66ec4")
set(CUE_PIX_ARCHIVE "${CMAKE_BINARY_DIR}/ThirdParty/WinPixEventRuntime-${CUE_PIX_VERSION}.zip")
set(CUE_PIX_DIRECTORY "${CMAKE_BINARY_DIR}/ThirdParty/WinPixEventRuntime-${CUE_PIX_VERSION}")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/ThirdParty")

# 再構成は検証済み Cache を再利用し、破損・別版は Hash 検証で拒否する
if(NOT EXISTS "${CUE_PIX_ARCHIVE}")
    file(DOWNLOAD
        "https://api.nuget.org/v3-flatcontainer/winpixeventruntime/${CUE_PIX_VERSION}/winpixeventruntime.${CUE_PIX_VERSION}.nupkg"
        "${CUE_PIX_ARCHIVE}"
        EXPECTED_HASH "SHA256=${CUE_PIX_SHA256}"
        TLS_VERIFY ON
        TIMEOUT 60
    )
endif()
file(SHA256 "${CUE_PIX_ARCHIVE}" taskPixHash)
if(NOT taskPixHash STREQUAL CUE_PIX_SHA256)
    message(FATAL_ERROR "WinPixEventRuntime archive hash mismatch: ${CUE_PIX_ARCHIVE}")
endif()
if(NOT EXISTS "${CUE_PIX_DIRECTORY}/Include/WinPixEventRuntime/pix3.h")
    file(ARCHIVE_EXTRACT INPUT "${CUE_PIX_ARCHIVE}" DESTINATION "${CUE_PIX_DIRECTORY}")
endif()

# 現在の Windows Target は x64。ABI の異なる Library を黙って Link しない
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
    message(FATAL_ERROR "CueEngine PIX integration currently requires Windows x64")
endif()
add_library(Cue.WinPixEventRuntime SHARED IMPORTED GLOBAL)
set_target_properties(Cue.WinPixEventRuntime PROPERTIES
    IMPORTED_IMPLIB "${CUE_PIX_DIRECTORY}/bin/x64/WinPixEventRuntime.lib"
    IMPORTED_LOCATION "${CUE_PIX_DIRECTORY}/bin/x64/WinPixEventRuntime.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${CUE_PIX_DIRECTORY}/Include/WinPixEventRuntime"
    INTERFACE_COMPILE_DEFINITIONS "$<$<NOT:$<CONFIG:Release>>:USE_PIX>"
)

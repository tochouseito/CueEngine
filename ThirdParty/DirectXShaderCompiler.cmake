# 公式 Release の対 DLL と License を固定し、Native Build に NuGet Restore を追加しない
set(CUE_DXC_VERSION "1.9.2609")
set(CUE_DXC_SHA256 "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1")
set(taskDxcBundledArchive "${PROJECT_SOURCE_DIR}/ThirdParty/Packages/DXC-${CUE_DXC_VERSION}.zip")
set(taskDxcCacheArchive "${CMAKE_BINARY_DIR}/ThirdParty/DXC-${CUE_DXC_VERSION}.zip")

# 配布 SDK の Archive を優先し、利用者側の Configure で Network を要求しない
if(EXISTS "${taskDxcBundledArchive}")
    set(CUE_DXC_ARCHIVE "${taskDxcBundledArchive}")
elseif(EXISTS "${PROJECT_SOURCE_DIR}/engine.cueengine")
    message(FATAL_ERROR "Engine package is missing its verified DXC archive")
else()
    set(CUE_DXC_ARCHIVE "${taskDxcCacheArchive}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/ThirdParty")
    if(NOT EXISTS "${CUE_DXC_ARCHIVE}")
        # 完全な Download と Hash 検証が終わった Archive だけを Cache として公開する
        set(taskDxcDownload "${CUE_DXC_ARCHIVE}.download")
        file(DOWNLOAD
            "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v${CUE_DXC_VERSION}/dxc_2026_09_29.zip"
            "${taskDxcDownload}"
            EXPECTED_HASH "SHA256=${CUE_DXC_SHA256}"
            STATUS taskDxcStatus
            TLS_VERIFY ON
            TIMEOUT 300
            INACTIVITY_TIMEOUT 30
        )
        list(GET taskDxcStatus 0 taskDxcStatusCode)
        if(NOT taskDxcStatusCode EQUAL 0)
            file(REMOVE "${taskDxcDownload}")
            message(FATAL_ERROR "DirectXShaderCompiler download failed: ${taskDxcStatus}")
        endif()
        file(RENAME "${taskDxcDownload}" "${CUE_DXC_ARCHIVE}")
    endif()
endif()
file(SHA256 "${CUE_DXC_ARCHIVE}" taskDxcArchiveHash)
if(NOT taskDxcArchiveHash STREQUAL CUE_DXC_SHA256)
    message(FATAL_ERROR "DirectXShaderCompiler archive hash mismatch: ${CUE_DXC_ARCHIVE}")
endif()

# SDK bin の最新版を暗黙に選ばず、同じ検証済み Package の x64 Pair だけを利用する
set(CUE_DXC_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/ThirdParty/DirectXShaderCompiler-${CUE_DXC_VERSION}")
if(NOT EXISTS "${CUE_DXC_PACKAGE_DIRECTORY}/.cue-dxc-verified")
    file(ARCHIVE_EXTRACT INPUT "${CUE_DXC_ARCHIVE}" DESTINATION "${CUE_DXC_PACKAGE_DIRECTORY}")
endif()
set(taskDxcFiles
    "bin/x64/dxc.exe"
    "bin/x64/dxcompiler.dll"
    "bin/x64/dxil.dll"
    "LICENSE-MS.txt"
    "LICENSE-LLVM.txt"
    "LICENCE-MIT.txt"
    "inc/hlsl/LICENCE.txt"
)
set(taskDxcHashes
    "aa7965b3763d9c710baaa5a28cedcc633449962a75d1391d161a85304a41b3e2"
    "0689e2fee0a1c9df8662a59680469f24e2a3ad7ce6bbbcc9284f7a83172bfc59"
    "64be2368211d257a038a7d4ea6bb3e24c878eb8d588c7b8414fccf759b69c2ac"
    "734f72f239fe7b07b4c7203f294c1a7ce27095687278bab7e56d630d7c672963"
    "729615317e28dd03907e46f0fc3b5e88f7853cee61d1a1471d2749335516b46f"
    "903df5512f7d02609fed0c780a9b704f5a3eeb6e4d84ebe42a29845c81899a3c"
    "f47b94fc51b04f4354a6c6f41fb84b0bd22a8ebfcde9f93c1b4d9abd7f6d14c2"
)
foreach(taskDxcFile taskDxcExpected IN ZIP_LISTS taskDxcFiles taskDxcHashes)
    if(NOT EXISTS "${CUE_DXC_PACKAGE_DIRECTORY}/${taskDxcFile}")
        message(FATAL_ERROR "DirectXShaderCompiler package file is missing: ${taskDxcFile}")
    endif()
    file(SHA256 "${CUE_DXC_PACKAGE_DIRECTORY}/${taskDxcFile}" taskDxcActual)
    if(NOT taskDxcActual STREQUAL taskDxcExpected)
        message(FATAL_ERROR "DirectXShaderCompiler package file hash mismatch: ${taskDxcFile}")
    endif()
endforeach()
file(WRITE "${CUE_DXC_PACKAGE_DIRECTORY}/.cue-dxc-verified" "${CUE_DXC_SHA256}\n")

# SDK Package 生成側へ元 Archive を渡す。展開済み Cache の未検証 File を再梱包しない
set(CUE_DXC_VERSION "${CUE_DXC_VERSION}" CACHE INTERNAL "Pinned DXC release version" FORCE)
set(CUE_DXC_SHA256 "${CUE_DXC_SHA256}" CACHE INTERNAL "Pinned DXC archive hash" FORCE)
set(CUE_DXC_ARCHIVE "${CUE_DXC_ARCHIVE}" CACHE INTERNAL "Pinned DXC archive for packaging" FORCE)
set(CUE_DXC_PACKAGE_DIRECTORY "${CUE_DXC_PACKAGE_DIRECTORY}"
    CACHE INTERNAL "Verified DXC package directory" FORCE)
set(CUE_DXC_RUNTIME_DIR "${CUE_DXC_PACKAGE_DIRECTORY}/bin/x64")
set(CUE_DXC_EXECUTABLE "${CUE_DXC_RUNTIME_DIR}/dxc.exe")
set_property(GLOBAL PROPERTY CUE_DXC_RUNTIME_DIR "${CUE_DXC_RUNTIME_DIR}")
set_property(GLOBAL PROPERTY CUE_DXC_PACKAGE_DIRECTORY "${CUE_DXC_PACKAGE_DIRECTORY}")

# Header と内蔵 HLSL Header の条件も保持し、実行 File 横へ意味が分かる名前で配置する
set(CUE_DXC_LICENSE_FILES
    DirectXShaderCompiler-MS.txt
    DirectXShaderCompiler-LLVM.txt
    DirectXShaderCompiler-MIT.txt
    DirectXShaderCompiler-HLSL.txt
)
set_property(GLOBAL PROPERTY CUE_DXC_LICENSE_FILES "${CUE_DXC_LICENSE_FILES}")

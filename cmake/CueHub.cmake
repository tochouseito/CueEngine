# Hub は Visual Studio から個別に起動する C# Tool として登録する
# 配布 SDK には Hub の Source を含めず、通常の Native Build に NuGet 復元を混ぜない
if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND
   EXISTS "${PROJECT_SOURCE_DIR}/Tools/CueHub/CueHub.csproj")
    include_external_msproject(CueHub
        "${PROJECT_SOURCE_DIR}/Tools/CueHub/CueHub.csproj"
        TYPE "{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}"
        PLATFORM x64
    )
    set_target_properties(CueHub PROPERTIES
        FOLDER "Tools"
        MAP_IMPORTED_CONFIG_DEVELOPMENT Release
        EXCLUDE_FROM_ALL TRUE
        EXCLUDE_FROM_DEFAULT_BUILD TRUE
    )
endif()

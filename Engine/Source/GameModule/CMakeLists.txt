add_library(Cue.GameModule.Abi INTERFACE)

target_sources(
    Cue.GameModule.Abi
    INTERFACE
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/GameModule/GameModuleAbi.h
)

target_include_directories(Cue.GameModule.Abi INTERFACE Public)

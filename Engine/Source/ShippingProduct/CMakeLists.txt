# Generated ProjectからRelease Monolithic Productに必要なEngine Targetだけを公開する
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Cue Shipping Product initially supports Windows x64 with MSVC only")
endif()

function(cue_add_shipping_engine_module relativePath)
    string(REPLACE "/" "-" binaryName "${relativePath}")
    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/../${relativePath}"
        "${CMAKE_CURRENT_BINARY_DIR}/Engine/${binaryName}"
        EXCLUDE_FROM_ALL
    )
endfunction()

cue_add_shipping_engine_module(Foundation)
cue_add_shipping_engine_module(GameModule)
cue_add_shipping_engine_module(Schema)
cue_add_shipping_engine_module(Math)
cue_add_shipping_engine_module(GameCore)
cue_add_shipping_engine_module(IO)
cue_add_shipping_engine_module(Scene)
cue_add_shipping_engine_module(Platform)
cue_add_shipping_engine_module(Input)
cue_add_shipping_engine_module(Runtime)
cue_add_shipping_engine_module(RHI)

set(CUE_RUNTIME_HOST_BUILD_DYNAMIC OFF)
cue_add_shipping_engine_module(RuntimeHost)

# Add the Engine-owned monolithic product entry around one project Static Game Module.
function(cue_add_shipping_product targetName gameModuleTarget projectId)
    if(NOT CUE_GAME_CONFIGURATION STREQUAL "Release")
        message(FATAL_ERROR "Cue Shipping Product target is Release-only")
    endif()
    if(NOT TARGET "${gameModuleTarget}")
        message(FATAL_ERROR "Cue Shipping Product Game Module target does not exist: ${gameModuleTarget}")
    endif()

    add_executable(
        "${targetName}"
        EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StaticProductMain.cpp"
    )
    target_link_libraries("${targetName}" PRIVATE "${gameModuleTarget}" Cue.RuntimeHost.Static)
    target_compile_features("${targetName}" PRIVATE cxx_std_20)
    target_compile_definitions(
        "${targetName}"
        PRIVATE
            "CUE_GAME_PRODUCT_PROJECT_ID=\"${projectId}\""
    )
    set_target_properties(
        "${targetName}"
        PROPERTIES
            CXX_EXTENSIONS OFF
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
            OUTPUT_NAME "CueGameProduct"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
            PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
    target_compile_options(
        "${targetName}"
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
    )
    target_link_options("${targetName}" PRIVATE /DEPENDENTLOADFLAG:0x800)
endfunction()

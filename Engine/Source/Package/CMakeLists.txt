add_library(Cue.Package STATIC)

target_sources(
    Cue.Package
    PRIVATE
        Private/Error.cpp
        Private/Manifest.cpp
        Private/PeDependency.cpp
        Private/RuntimeData.cpp
        Private/Sha256.cpp
        Private/Sha256.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Package/Error.h
            Public/Cue/Package/Manifest.h
            Public/Cue/Package/RuntimeData.h
)

target_compile_features(Cue.Package PUBLIC cxx_std_20)
target_link_libraries(Cue.Package PUBLIC Cue.Build Cue.Foundation Cue.Project Cue.Scene)
target_include_directories(Cue.Package PUBLIC Public)

set_target_properties(
    Cue.Package
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Package
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Package PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

get_target_property(cuePackageLinks Cue.Package LINK_LIBRARIES)
get_target_property(cuePackageInterfaceLinks Cue.Package INTERFACE_LINK_LIBRARIES)

if(NOT "${cuePackageLinks}" STREQUAL "Cue.Build;Cue.Foundation;Cue.Project;Cue.Scene")
    message(FATAL_ERROR "Cue.Package must directly link only Cue.Build, Cue.Foundation, Cue.Project, and Cue.Scene: ${cuePackageLinks}")
endif()

if(NOT "${cuePackageInterfaceLinks}" STREQUAL "Cue.Build;Cue.Foundation;Cue.Project;Cue.Scene")
    message(FATAL_ERROR "Cue.Package must expose only Cue.Build, Cue.Foundation, Cue.Project, and Cue.Scene: ${cuePackageInterfaceLinks}")
endif()

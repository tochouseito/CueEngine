# Editor に静的にリンクし、Project の Runtime と配布 EXE に依存を伝えない
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH sourcePath
    REPO assimp/assimp
    REF "v${VERSION}"
    SHA512 f3639e3964ea8ef41ce684eb1b764ece79f64a15ecae068846c5bc0853780e39f600776027d8843e6a3f47988daf067a164161a58f76ec6de13027ae1e473bfb
    HEAD_REF master
)

# OBJ と MTL を読み取る経路だけを有効にする
vcpkg_cmake_configure(
    SOURCE_PATH "${sourcePath}"
    OPTIONS
        -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF
        -DASSIMP_BUILD_OBJ_IMPORTER=ON
        -DASSIMP_NO_EXPORT=ON
        -DASSIMP_BUILD_ASSIMP_TOOLS=OFF
        -DASSIMP_BUILD_SAMPLES=OFF
        -DASSIMP_BUILD_TESTS=OFF
        -DASSIMP_BUILD_DOCS=OFF
        -DASSIMP_BUILD_DRACO=OFF
        -DASSIMP_BUILD_ZLIB=ON
        -DASSIMP_WARNINGS_AS_ERRORS=OFF
        -DASSIMP_IGNORE_GIT_HASH=ON
        -DASSIMP_INSTALL_PDB=OFF
    MAYBE_UNUSED_VARIABLES
        ASSIMP_INSTALL_PDB
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/assimp-6.0")
# SDK では CMake Target を使うため、生成時の絶対 Path を持つ pkg-config File は収録しない
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/lib/pkgconfig"
    "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig"
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)
vcpkg_install_copyright(FILE_LIST "${sourcePath}/LICENSE")

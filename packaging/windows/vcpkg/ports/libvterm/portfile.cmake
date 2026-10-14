# libvterm 的头文件没有 dllexport/dllimport 修饰，MSVC 下做成 DLL 不会导出任何符号，只能静态链接。
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_download_distfile(ARCHIVE
    URLS "https://www.leonerd.org.uk/code/libvterm/libvterm-${VERSION}.tar.gz"
    FILENAME "libvterm-${VERSION}.tar.gz"
    SHA512 0117f71988d2c3c1b5e46d0778ce491a8ab9033f3afc9a9905b6d52d0e2adb59ee101667826281470e4c9de5178cbe966f4ecb2c7673e9644c7ca3508491bb49
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")

# 上游只提供 Makefile + libtool。发布包里已带生成好的编码表（src/encoding/*.inc），
# 用一个最小的 CMakeLists 编译 src/*.c 即可，做法与 Neovim 打包 libvterm 时相同。
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-libvterm)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

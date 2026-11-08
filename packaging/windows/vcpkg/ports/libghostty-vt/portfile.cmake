# libghostty-vt 没有 CMake/Makefile 构建，只能用 Zig 0.17.x 编译（上游 PACKAGING-LIBGHOSTTY.md）。
# 本 port 在构建期下载校验过的官方 Zig 工具链，并按上游的离线方式构建：源码包与依赖包同版本配对，
# `--system` 使用已解包的依赖，不在构建中联网拉取 Zig 包。产物是 ghostty-vt.dll + 导入库，
# 并带有 ghostty-vt::ghostty-vt 的 CMake 包配置（lib/cmake/ghostty-vt，相对自身路径，可随前缀搬移）。
#
# 只构建 release：Zig 没有"debug 变体"的概念，上游文档推荐 ReleaseFast。
set(VCPKG_BUILD_TYPE release)
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

# Zig 的 CMake 包配置放在 lib/cmake/ghostty-vt，而 vcpkg 默认要求放在 share/<port>。包配置用自身
# 路径回推前缀，挪走会失效，因此保留原位置，并跳过该项检查；CMAKE_PREFIX_PATH 包含 vcpkg 的安装
# 前缀，find_package(ghostty-vt) 会在 <prefix>/lib/cmake/ghostty-vt 找到它。
# Zig 生成的 DLL 不链接 MSVC CRT，CRT 链接检查对它没有意义。
set(VCPKG_POLICY_SKIP_MISPLACED_CMAKE_FILES_CHECK enabled)
set(VCPKG_POLICY_SKIP_CRT_LINKAGE_CHECK enabled)

vcpkg_download_distfile(SOURCE_ARCHIVE
    URLS "https://release.files.ghostty.org/libghostty-vt/${VERSION}/libghostty-vt-${VERSION}.tar.gz"
    FILENAME "libghostty-vt-${VERSION}.tar.gz"
    SHA512 d8b5fea624debbc43c76d13e6997773f29e1e4f1e5eeae26f759040cb862762ae47fccbe840714656909c42bfaa5180c704dc6e4cd6a5b75c2b69492e0494c56
)
vcpkg_download_distfile(DEPS_ARCHIVE
    URLS "https://release.files.ghostty.org/libghostty-vt/${VERSION}/libghostty-vt-${VERSION}-deps.tar.gz"
    FILENAME "libghostty-vt-${VERSION}-deps.tar.gz"
    SHA512 05856f5abcc6813c9c21c083e4eec250bd4deda576464b9853ed0904ec71f5e3f4ac7aa2ecb60582a49ae619585c6afdf1e3140c957ed6bcb34d104d10649fbc
)

# 1.0.0 的 minimum_zig_version 是 0.17.0：主次版本必须一致，补丁版本可以更新。
set(ZIG_VERSION "0.17.1")
if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
    set(ZIG_ARCH "aarch64")
    set(ZIG_SHA512 4a3434121cb04aef0b9d485d844f76802266ac0c4999a1f144b536825bc247fb15edd118b460a385bbf715e524031effb47b6a49d0c37cb5289c25772f497773)
else()
    set(ZIG_ARCH "x86_64")
    set(ZIG_SHA512 288b10302ea0176b0646fa42784c2850848fd4c71ef80a2d924dd94abc07f8ef3f6d1c70f3c6c276ade396ec8a1417b92b65f66b833e3b002f79c0fbe143997f)
endif()
vcpkg_download_distfile(ZIG_ARCHIVE
    URLS "https://ziglang.org/download/${ZIG_VERSION}/zig-${ZIG_ARCH}-windows-${ZIG_VERSION}.zip"
    FILENAME "zig-${ZIG_ARCH}-windows-${ZIG_VERSION}.zip"
    SHA512 ${ZIG_SHA512}
)

vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${SOURCE_ARCHIVE}")
vcpkg_extract_source_archive(DEPS_PATH ARCHIVE "${DEPS_ARCHIVE}")
vcpkg_extract_source_archive(ZIG_PATH ARCHIVE "${ZIG_ARCHIVE}")

# 缓存目录必须是全新的空目录：Zig 0.17 遇到已有内容的缓存可能崩溃（上游文档的明确要求）。
set(ZIG_CACHE "${CURRENT_BUILDTREES_DIR}/zig-cache-${TARGET_TRIPLET}")
file(REMOVE_RECURSE "${ZIG_CACHE}")
file(MAKE_DIRECTORY "${ZIG_CACHE}")
set(ENV{ZIG_GLOBAL_CACHE_DIR} "${ZIG_CACHE}")

# 官方 Zig 的默认目标是它自己的架构。windows-11-arm 上的 CMake 常常是 x64 仿真进程，
# CMAKE_HOST_SYSTEM_PROCESSOR 会报成 AMD64，于是编出 x64 的 ghostty-vt.lib，
# 链接 ARM64 的 Shellium 时 LNK4272。目标架构以 vcpkg 的 triplet 为准。
# x64 不显式传 -Dtarget：该路径已经在 CI 上通过，保持 Zig 的默认 Windows ABI。
set(ZIG_TARGET_ARGS "")
if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    list(APPEND ZIG_TARGET_ARGS "-Dtarget=aarch64-windows-gnu")
elseif(NOT VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    message(FATAL_ERROR "libghostty-vt 不支持的架构：${VCPKG_TARGET_ARCHITECTURE}")
endif()

vcpkg_execute_required_process(
    COMMAND "${ZIG_PATH}/zig.exe" build
        --prefix "${CURRENT_PACKAGES_DIR}"
        --system "${DEPS_PATH}"
        -Doptimize=ReleaseFast
        -Dcpu=baseline
        ${ZIG_TARGET_ARGS}
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME "build-${TARGET_TRIPLET}"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

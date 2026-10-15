#!/usr/bin/env bash

# Shellium Linux AppImage 打包脚本，支持 x86_64 与 aarch64。
#
# 流程：Release 编译 → 安装到 AppDir → linuxdeploy 收集共享库与 Qt 插件 → 生成 AppImage。
# 目标架构等于本机架构，和 macOS 脚本一样不做交叉编译：发行版的 -dev 包只提供本机架构。
#
# AppImage 只携带 glibc 以外的依赖，因此能运行的最低发行版由构建机的 glibc 决定。CI 在
# Ubuntu 22.04 容器里构建（glibc 2.35）；该版本仓库里的 libssh、libvterm、spdlog 都不够新，需先用
# install_libssh_from_source.sh 与 install_linux_deps_from_source.sh 从源码安装。
#
# 依赖：cmake、ninja、pkg-config、Qt 6.8+（qmake 在 PATH 或通过 QMAKE 指定）、curl。

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGET_ARCH="$(uname -m)"
BUILD_DIR="${SHELLIUM_BUILD_DIR:-${PROJECT_ROOT}/build-release-linux-${TARGET_ARCH}}"
DIST_DIR="${SHELLIUM_DIST_DIR:-${PROJECT_ROOT}/dist}"
PARALLEL_JOBS="${SHELLIUM_BUILD_JOBS:-$(nproc)}"
TOOLS_DIR="${SHELLIUM_TOOLS_DIR:-${BUILD_DIR}/tools}"
PROJECT_VERSION="${SHELLIUM_VERSION:-$(sed -nE 's/^project\(Shellium VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "${PROJECT_ROOT}/CMakeLists.txt" | head -n 1)}"

# linuxdeploy 官方只发布滚动的 continuous 版本。上游变更导致打包失败时，可用这两个环境变量
# 指向已知可用的镜像副本，不必改脚本。
LINUXDEPLOY_URL="${SHELLIUM_LINUXDEPLOY_URL:-https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${TARGET_ARCH}.AppImage}"
LINUXDEPLOY_QT_URL="${SHELLIUM_LINUXDEPLOY_QT_URL:-https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-${TARGET_ARCH}.AppImage}"

log() {
    printf '\n==> %s\n' "$*"
}

fail() {
    printf '\n错误：%s\n' "$*" >&2
    exit 1
}

[[ "$(uname -s)" == "Linux" ]] || fail "该脚本只能在 Linux 上运行。"
[[ "${TARGET_ARCH}" == "x86_64" || "${TARGET_ARCH}" == "aarch64" ]] \
    || fail "不支持的架构：${TARGET_ARCH}"
[[ -n "${PROJECT_VERSION}" ]] || fail "无法从 CMakeLists.txt 读取项目版本。"
for tool in cmake ninja pkg-config curl; do
    command -v "${tool}" >/dev/null 2>&1 || fail "缺少 ${tool}。"
done

# linuxdeploy-plugin-qt 通过 qmake 查询 Qt 的插件与库目录。
QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake || true)}"
[[ -x "${QMAKE}" ]] || fail "找不到 Qt 6 的 qmake，请把 Qt bin 目录加入 PATH 或设置 QMAKE。"
export QMAKE

log "配置 ${TARGET_ARCH} Release 工程"
cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr

log "编译 Shellium"
cmake --build "${BUILD_DIR}" --target Shellium --parallel "${PARALLEL_JOBS}"

APPDIR="${BUILD_DIR}/AppDir"
rm -rf -- "${APPDIR}"
log "安装到 AppDir"
DESTDIR="${APPDIR}" cmake --install "${BUILD_DIR}"
[[ -x "${APPDIR}/usr/bin/Shellium" ]] || fail "安装后未找到 ${APPDIR}/usr/bin/Shellium。"

# 官方 Qt 的 libQt6Gui 依赖 libOpenGL.so.0，它在 linuxdeploy 排除清单里不会被收集，而桌面版
# Ubuntu 默认不装 libopengl0。放一份到不在 rpath 上的备份目录，由 AppRun hook 仅在系统缺失时
# 启用（原因见 hook 文件头）。hook 须在 linuxdeploy 运行前就位，它据此生成 AppRun 包装脚本。
log "放置 libOpenGL 备份与 AppRun hook"
system_libopengl="$( { /sbin/ldconfig -p || ldconfig -p; } 2>/dev/null \
    | sed -nE 's/^[[:space:]]*libOpenGL\.so\.0 .*=> (.*)$/\1/p' | head -n 1)"
[[ -n "${system_libopengl}" && -f "${system_libopengl}" ]] \
    || fail "构建机上找不到 libOpenGL.so.0，请安装 libopengl0。"
mkdir -p "${APPDIR}/usr/lib/opengl-fallback" "${APPDIR}/apprun-hooks"
cp -L -- "${system_libopengl}" "${APPDIR}/usr/lib/opengl-fallback/libOpenGL.so.0"
cp -- "${PROJECT_ROOT}/packaging/linux/apprun-hooks/shellium-opengl-fallback.sh" "${APPDIR}/apprun-hooks/"

mkdir -p "${TOOLS_DIR}"
fetch_tool() {
    local url="$1"
    local target
    target="${TOOLS_DIR}/$(basename "${url}")"
    if [[ ! -x "${target}" ]]; then
        curl -fsSL --retry 3 -o "${target}.part" "${url}"
        chmod +x "${target}.part"
        mv -f -- "${target}.part" "${target}"
    fi
    printf '%s\n' "${target}"
}
LINUXDEPLOY="$(fetch_tool "${LINUXDEPLOY_URL}")"
fetch_tool "${LINUXDEPLOY_QT_URL}" >/dev/null
# linuxdeploy 按文件名在自身目录与 PATH 中查找插件。
export PATH="${TOOLS_DIR}:${PATH}"

# CI 容器和 runner 通常没有 FUSE，让 AppImage 形式的工具先解包再运行。
export APPIMAGE_EXTRACT_AND_RUN=1

# 应用只用 QSQLITE。Qt 安装里的 psql/mysql/odbc 等驱动依赖构建机上没有的客户端库，
# linuxdeploy 解析不到它们会直接失败，而这些驱动打进包也只是死代码。
QT_PLUGIN_DIR="$("${QMAKE}" -query QT_INSTALL_PLUGINS)"
EXCLUDED_SQL_DRIVERS=()
for driver in "${QT_PLUGIN_DIR}"/sqldrivers/*.so; do
    [[ -e "${driver}" ]] || continue
    case "$(basename "${driver}")" in
        libqsqlite.so) ;;
        *) EXCLUDED_SQL_DRIVERS+=("$(basename "${driver}")") ;;
    esac
done
if [[ "${#EXCLUDED_SQL_DRIVERS[@]}" -gt 0 ]]; then
    LINUXDEPLOY_EXCLUDED_LIBRARIES="$(IFS=';'; printf '%s' "${EXCLUDED_SQL_DRIVERS[*]}")"
    export LINUXDEPLOY_EXCLUDED_LIBRARIES
fi

# linuxdeploy 按运行时规则（LD_LIBRARY_PATH，再 ldconfig 缓存）解析 soname。源码安装到
# /usr/local 的 libssh 0.11 与发行版的 0.10 同为 libssh.so.4；aarch64 上 ldconfig 的
# aarch64-linux-gnu.conf 排在提供 /usr/local/lib 的 libc.conf 之前，系统旧版会被打进包，
# 启动即报 LIBSSH_4_10_0 符号版本缺失。把各依赖 pkg-config 给出的 libdir 放到最前，
# 确保收集的就是编译时链接的那一份。
# 按列表顺序拼接（libssh 在最前）：其余依赖多在系统目录，若排在前面又会先命中系统旧版 libssh。
dependency_libdirs=""
for package in libssh vterm libsodium spdlog minizip; do
    package_libdir="$(pkg-config --variable=libdir "${package}" 2>/dev/null || true)"
    if [[ -n "${package_libdir}" && -d "${package_libdir}" ]]; then
        dependency_libdirs="${dependency_libdirs:+${dependency_libdirs}:}${package_libdir}"
    fi
done
export LD_LIBRARY_PATH="${dependency_libdirs}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

mkdir -p "${DIST_DIR}"
OUTPUT_APPIMAGE="${DIST_DIR}/Shellium-${PROJECT_VERSION}-linux-${TARGET_ARCH}.AppImage"
export LDAI_OUTPUT="${OUTPUT_APPIMAGE}"
export ARCH="${TARGET_ARCH}"

log "收集依赖并生成 AppImage"
"${LINUXDEPLOY}" \
    --appdir "${APPDIR}" \
    --executable "${APPDIR}/usr/bin/Shellium" \
    --desktop-file "${PROJECT_ROOT}/packaging/linux/shellium.desktop" \
    --icon-file "${PROJECT_ROOT}/packaging/linux/shellium.png" \
    --plugin qt \
    --output appimage

[[ -x "${OUTPUT_APPIMAGE}" ]] || fail "linuxdeploy 未生成 ${OUTPUT_APPIMAGE}。"
printf '\n构建完成：%s\n' "${OUTPUT_APPIMAGE}"

#!/usr/bin/env bash
set -euo pipefail

# 从源码编译安装 Ubuntu 22.04 仓库版本不够的依赖：libvterm 0.3（仓库 0.1.4）、spdlog 1.12+（仓库 1.9）。
#
# Linux 安装包在 Ubuntu 22.04 上构建，以 glibc 2.35 作为最低运行要求。libssh 0.11 另由
# install_libssh_from_source.sh 安装。spdlog 编成静态库并使用自带的 fmt，不会出现在包的依赖里；
# libvterm 编成共享库，由 linuxdeploy 收集进 AppDir。
#
# 用法：install_linux_deps_from_source.sh <安装前缀，如 /usr/local>
# 依赖：cmake、ninja、make、libtool（libtool-bin）、perl、C/C++ 编译器、curl。

LIBVTERM_VERSION="0.3.3"
LIBVTERM_SHA256="09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0"
SPDLOG_VERSION="1.15.3"
SPDLOG_SHA256="15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67"

prefix="$1"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/shellium-deps.XXXXXX")"
trap 'rm -rf -- "${work_dir}"' EXIT

"${script_dir}/fetch_verified_source.sh" \
    "https://www.leonerd.org.uk/code/libvterm/libvterm-${LIBVTERM_VERSION}.tar.gz" \
    "${LIBVTERM_SHA256}" "${work_dir}/libvterm.tar.gz"
tar -xf "${work_dir}/libvterm.tar.gz" -C "${work_dir}"
make -C "${work_dir}/libvterm-${LIBVTERM_VERSION}" PREFIX="${prefix}" -j "$(nproc)"
make -C "${work_dir}/libvterm-${LIBVTERM_VERSION}" PREFIX="${prefix}" install
printf 'libvterm %s 已安装到 %s\n' "${LIBVTERM_VERSION}" "${prefix}"

"${script_dir}/fetch_verified_source.sh" \
    "https://github.com/gabime/spdlog/archive/refs/tags/v${SPDLOG_VERSION}.tar.gz" \
    "${SPDLOG_SHA256}" "${work_dir}/spdlog.tar.gz"
tar -xf "${work_dir}/spdlog.tar.gz" -C "${work_dir}"
cmake -S "${work_dir}/spdlog-${SPDLOG_VERSION}" -B "${work_dir}/spdlog-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DSPDLOG_BUILD_SHARED=OFF \
    -DSPDLOG_BUILD_EXAMPLE=OFF \
    -DSPDLOG_BUILD_TESTS=OFF \
    -DSPDLOG_BUILD_BENCH=OFF
cmake --build "${work_dir}/spdlog-build" --parallel
cmake --install "${work_dir}/spdlog-build"
printf 'spdlog %s 已安装到 %s\n' "${SPDLOG_VERSION}" "${prefix}"

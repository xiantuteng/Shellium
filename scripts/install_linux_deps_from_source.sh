#!/usr/bin/env bash
set -euo pipefail

# 从源码编译安装 Ubuntu 22.04 仓库里没有或版本不够的依赖：libghostty-vt 1.0（仓库没有）、spdlog 1.12+（仓库 1.9）。
#
# Linux 安装包在 Ubuntu 22.04 上构建，以 glibc 2.35 作为最低运行要求。libssh 0.11 另由
# install_libssh_from_source.sh 安装。spdlog 编成静态库并使用自带的 fmt，不会出现在包的依赖里；
# libghostty-vt 编成共享库（只依赖 libc），由 linuxdeploy 收集进 AppDir。它的构建需要 Zig，
# 见 install_libghostty_vt_from_source.sh（Zig 自动下载到临时目录，用完即删）。
#
# 用法：install_linux_deps_from_source.sh <安装前缀，如 /usr/local>
# 依赖：cmake、ninja、C/C++ 编译器、curl、xz-utils。

SPDLOG_VERSION="1.15.3"
SPDLOG_SHA256="15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67"

prefix="$1"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/shellium-deps.XXXXXX")"
trap 'rm -rf -- "${work_dir}"' EXIT

"${script_dir}/install_libghostty_vt_from_source.sh" "${prefix}"

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

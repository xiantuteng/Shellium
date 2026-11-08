#!/usr/bin/env bash
set -euo pipefail

# 从官方发行包编译安装 libghostty-vt（终端模拟引擎，C ABI，只依赖 libc）。
#
# 上游只发源码，没有预编译库，且需要 Zig 0.17.x 才能构建。本脚本下载官方 Zig 到临时目录
# （构建结束即删除，不污染系统），再按上游 PACKAGING-LIBGHOSTTY.md 的离线方式构建：源码包与
# 依赖包同版本配对，`--system` 使用已解包的依赖，不在构建中联网拉取 Zig 包。
# 安装物：lib/libghostty-vt.{so,dylib,a}、include/ghostty/、lib/pkgconfig/libghostty-vt.pc、
# lib/cmake/ghostty-vt/，CMake 的 find_package(ghostty-vt) 与 pkg-config 都能找到。
#
# 用法：install_libghostty_vt_from_source.sh <安装前缀，如 /usr/local>
# 环境变量：
#   ZIG_TARGET  可选，交叉/指定最低系统版本时的 Zig 目标三元组，例如 aarch64-macos.13.0。
# 依赖：curl、tar、xz、sha256sum 或 shasum。

LIBGHOSTTY_VT_VERSION="1.0.0"
LIBGHOSTTY_VT_SHA256="4a6ef758be721923e1969e7b81b443dcc92d8dd3fb5a39600c03a8817a753676"
LIBGHOSTTY_VT_DEPS_SHA256="4d5ea5e448e4c7ea8fb55ccbaa8668f2a97d44f438cee831e2b7ecd162d85c29"
# 1.0.0 的 minimum_zig_version 是 0.17.0：主次版本必须一致，补丁版本可以更新。
ZIG_VERSION="0.17.1"
ZIG_RELEASE_BASE="https://release.files.ghostty.org/libghostty-vt/${LIBGHOSTTY_VT_VERSION}"

prefix="$1"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/shellium-ghostty-vt.XXXXXX")"
trap 'rm -rf -- "${work_dir}"' EXIT

os="$(uname -s)"
machine="$(uname -m)"
case "${os}" in
    Linux)  zig_os="linux" ;;
    Darwin) zig_os="macos" ;;
    *) printf '不支持的系统：%s（Windows 请使用 vcpkg overlay port）\n' "${os}" >&2; exit 1 ;;
esac
case "${machine}" in
    x86_64|amd64)  zig_arch="x86_64" ;;
    aarch64|arm64) zig_arch="aarch64" ;;
    *) printf '不支持的架构：%s\n' "${machine}" >&2; exit 1 ;;
esac

case "${zig_arch}-${zig_os}" in
    x86_64-linux)   zig_sha256="3581863e3cbcfd0fce0efc682ce7e9e93da96668a977791d356a26d2a7e9b17a" ;;
    aarch64-linux)  zig_sha256="a1546d5646eb184437918d6635ca6b762a0e889eac09cef720ee7a1ed3fe7795" ;;
    aarch64-macos)  zig_sha256="75f1bc7a1e9852f17ec9a0c1895f767c4ec56d46bc8a834920eece1d0359dd8d" ;;
    x86_64-macos)   zig_sha256="9af07b20319fbd0c92ee46ad23e81d6a782b1aaea2cfb8dcbff35f1d2c7ddbb7" ;;
esac

zig_name="zig-${zig_arch}-${zig_os}-${ZIG_VERSION}"
"${script_dir}/fetch_verified_source.sh" \
    "https://ziglang.org/download/${ZIG_VERSION}/${zig_name}.tar.xz" \
    "${zig_sha256}" "${work_dir}/zig.tar.xz"
tar -xf "${work_dir}/zig.tar.xz" -C "${work_dir}"
zig="${work_dir}/${zig_name}/zig"

"${script_dir}/fetch_verified_source.sh" \
    "${ZIG_RELEASE_BASE}/libghostty-vt-${LIBGHOSTTY_VT_VERSION}.tar.gz" \
    "${LIBGHOSTTY_VT_SHA256}" "${work_dir}/libghostty-vt.tar.gz"
"${script_dir}/fetch_verified_source.sh" \
    "${ZIG_RELEASE_BASE}/libghostty-vt-${LIBGHOSTTY_VT_VERSION}-deps.tar.gz" \
    "${LIBGHOSTTY_VT_DEPS_SHA256}" "${work_dir}/libghostty-vt-deps.tar.gz"
tar -xzf "${work_dir}/libghostty-vt.tar.gz" -C "${work_dir}"
tar -xzf "${work_dir}/libghostty-vt-deps.tar.gz" -C "${work_dir}"

source_dir="${work_dir}/libghostty-vt-${LIBGHOSTTY_VT_VERSION}"
deps_dir="${work_dir}/libghostty-vt-${LIBGHOSTTY_VT_VERSION}-deps"
[[ -d "${source_dir}" && -d "${deps_dir}" ]] || {
    printf '解包后缺少目录：%s 或 %s\n' "${source_dir}" "${deps_dir}" >&2
    exit 1
}

# 缓存目录必须是全新的空目录：Zig 0.17 遇到已有内容的缓存可能崩溃（上游文档的明确要求）。
export ZIG_GLOBAL_CACHE_DIR="${work_dir}/zig-cache"
mkdir -p "${ZIG_GLOBAL_CACHE_DIR}"

build_arguments=(
    --prefix "${prefix}"
    --system "${deps_dir}"
    -Doptimize=ReleaseFast
    -Dcpu=baseline
)
if [[ -n "${ZIG_TARGET:-}" ]]; then
    build_arguments+=("-Dtarget=${ZIG_TARGET}")
fi
(cd "${source_dir}" && "${zig}" build "${build_arguments[@]}")
printf 'libghostty-vt %s 已安装到 %s\n' "${LIBGHOSTTY_VT_VERSION}" "${prefix}"

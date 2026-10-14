#!/usr/bin/env bash
set -euo pipefail

# 从源码编译安装固定版本的 libssh。
#
# Shellium 用到 sftp_aio、SSH_OPTIONS_PROXYJUMP、ssh_channel_get_exit_state 等 0.11 才有的接口，
# 而 Ubuntu 24.04 仓库只有 0.10.6。Homebrew 和 MSYS2 已是 0.11+，只有 Linux 构建机需要本脚本。
# 版本与 SHA-256 一起固定，上游文件被替换时直接失败，而不是编进一个来历不明的库。
#
# 用法：install_libssh_from_source.sh <安装前缀，如 /usr/local>
# 依赖：cmake、ninja、C 编译器、OpenSSL 与 zlib 的开发包、curl。

LIBSSH_VERSION="0.11.5"
LIBSSH_SHA256="6898ba9dd836d618b71dc7a4bb786a502c173cef5cafbf20fe5e0567ba4ea30c"

prefix="$1"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/shellium-libssh.XXXXXX")"
trap 'rm -rf -- "${work_dir}"' EXIT

archive="${work_dir}/libssh-${LIBSSH_VERSION}.tar.xz"
curl -fsSL --retry 3 -o "${archive}" \
    "https://www.libssh.org/files/${LIBSSH_VERSION%.*}/libssh-${LIBSSH_VERSION}.tar.xz"

if command -v sha256sum >/dev/null 2>&1; then
    actual_sha256="$(sha256sum "${archive}" | awk '{print $1}')"
else
    actual_sha256="$(shasum -a 256 "${archive}" | awk '{print $1}')"
fi
if [[ "${actual_sha256}" != "${LIBSSH_SHA256}" ]]; then
    printf 'libssh 源码包校验失败：期望 %s，实际 %s\n' "${LIBSSH_SHA256}" "${actual_sha256}" >&2
    exit 1
fi

tar -xf "${archive}" -C "${work_dir}"
cmake -S "${work_dir}/libssh-${LIBSSH_VERSION}" -B "${work_dir}/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DWITH_EXAMPLES=OFF \
    -DUNIT_TESTING=OFF \
    -DCLIENT_TESTING=OFF \
    -DWITH_GSSAPI=OFF
cmake --build "${work_dir}/build" --parallel
cmake --install "${work_dir}/build"
printf 'libssh %s 已安装到 %s\n' "${LIBSSH_VERSION}" "${prefix}"

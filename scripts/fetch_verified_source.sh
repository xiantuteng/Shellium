#!/usr/bin/env bash
set -euo pipefail

# 下载源码包并校验 SHA-256，校验不符直接失败。
#
# 版本与校验值一起固定在调用方：上游文件被替换时宁可构建失败，也不编进一个来历不明的库。
#
# 用法：fetch_verified_source.sh <URL> <SHA-256> <保存路径>

url="$1"
expected_sha256="$2"
target="$3"

curl -fsSL --retry 3 -o "${target}" "${url}"
if command -v sha256sum >/dev/null 2>&1; then
    actual_sha256="$(sha256sum "${target}" | awk '{print $1}')"
else
    actual_sha256="$(shasum -a 256 "${target}" | awk '{print $1}')"
fi
if [[ "${actual_sha256}" != "${expected_sha256}" ]]; then
    printf '%s 校验失败：期望 %s，实际 %s\n' "$(basename "${target}")" "${expected_sha256}" "${actual_sha256}" >&2
    exit 1
fi

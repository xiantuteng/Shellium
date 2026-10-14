#!/usr/bin/env bash

# 确保 NSIS 已安装在指定目录，缺失时用 Chocolatey 安装并重试。
#
# 用法：ensure_nsis.sh <NSIS 目录>
#
# GitHub 的 x64 Windows 镜像不预装 NSIS。Chocolatey 社区源和它背后的下载地址偶尔失败，而
# choco 的非零退出码在 pwsh 步骤里不会中止脚本，结果要到打包脚本找不到 makensis.exe 才报错，
# 看不出真正原因。这里以 makensis.exe 是否真实存在为准，失败时重试几次再明确报错。
# 重试间隔（秒）可用 NSIS_RETRY_DELAY 覆盖，测试用 0。

set -Eeuo pipefail

if [[ $# -ne 1 ]]; then
    printf '用法：%s <NSIS 目录>\n' "$(basename "$0")" >&2
    exit 2
fi

makensis="$1/makensis.exe"
retry_delay="${NSIS_RETRY_DELAY:-20}"
max_attempts=3

if [[ -f "${makensis}" ]]; then
    printf 'NSIS 已安装：%s\n' "${makensis}"
    exit 0
fi

for (( attempt = 1; attempt <= max_attempts; attempt++ )); do
    printf '安装 NSIS（第 %d/%d 次）\n' "${attempt}" "${max_attempts}"
    if choco install nsis -y --no-progress && [[ -f "${makensis}" ]]; then
        printf 'NSIS 已安装：%s\n' "${makensis}"
        exit 0
    fi
    if (( attempt < max_attempts )); then
        sleep $(( retry_delay * attempt ))
    fi
done

printf '错误：%d 次安装后仍找不到 %s\n' "${max_attempts}" "${makensis}" >&2
exit 1

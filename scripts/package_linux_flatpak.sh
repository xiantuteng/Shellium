#!/usr/bin/env bash

# Shellium Flatpak 打包脚本：按 packaging/flatpak 下的清单构建，导出单文件 .flatpak。
#
# 目标架构等于本机架构。运行时与 SDK 从 Flathub 安装到当前用户；bundle 记录了 Flathub 仓库地址，
# 用户安装时会自动补装运行时。
#
# 依赖：flatpak、flatpak-builder。

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

APP_ID="net.shellium.Shellium"
MANIFEST="${PROJECT_ROOT}/packaging/flatpak/${APP_ID}.yml"
FLATHUB_REPO_URL="https://dl.flathub.org/repo/flathub.flatpakrepo"
TARGET_ARCH="$(uname -m)"
WORK_DIR="${SHELLIUM_FLATPAK_DIR:-${PROJECT_ROOT}/build-flatpak}"
DIST_DIR="${SHELLIUM_DIST_DIR:-${PROJECT_ROOT}/dist}"
PROJECT_VERSION="${SHELLIUM_VERSION:-$(sed -nE 's/^project\(Shellium VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "${PROJECT_ROOT}/CMakeLists.txt" | head -n 1)}"

fail() {
    printf '\n错误：%s\n' "$*" >&2
    exit 1
}

[[ "$(uname -s)" == "Linux" ]] || fail "该脚本只能在 Linux 上运行。"
[[ -n "${PROJECT_VERSION}" ]] || fail "无法从 CMakeLists.txt 读取项目版本。"
for tool in flatpak flatpak-builder; do
    command -v "${tool}" >/dev/null 2>&1 || fail "缺少 ${tool}。"
done

flatpak remote-add --user --if-not-exists flathub "${FLATHUB_REPO_URL}"

mkdir -p "${WORK_DIR}" "${DIST_DIR}"
OUTPUT="${DIST_DIR}/Shellium-${PROJECT_VERSION}-linux-${TARGET_ARCH}.flatpak"

# CI runner 没有 FUSE，rofiles-fuse 用不了。
printf '\n==> 构建 Flatpak\n'
flatpak-builder --user --install-deps-from=flathub --force-clean --disable-rofiles-fuse \
    --state-dir="${WORK_DIR}/state" \
    --repo="${WORK_DIR}/repo" \
    "${WORK_DIR}/app" "${MANIFEST}"

printf '\n==> 导出 bundle\n'
flatpak build-bundle --runtime-repo="${FLATHUB_REPO_URL}" \
    "${WORK_DIR}/repo" "${OUTPUT}" "${APP_ID}"

[[ -s "${OUTPUT}" ]] || fail "未生成 ${OUTPUT}。"
printf '\n构建完成：%s\n' "${OUTPUT}"

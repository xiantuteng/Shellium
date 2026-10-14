#!/usr/bin/env bash

# 确保 NSIS 已安装在指定目录：缺失时先用 Chocolatey，失败则直接下载官方便携 ZIP。
#
# 用法：ensure_nsis.sh <NSIS 目录>
#
# GitHub 的 x64 Windows 镜像不预装 NSIS。Chocolatey 社区源会对 runner 的出口 IP 限流，限流
# 期间连续几次 choco install 全部失败，重试救不回来；choco 的非零退出码在 pwsh 步骤里也不会
# 中止脚本，要到打包脚本找不到 makensis.exe 才报错。所以这里以 makensis.exe 是否真实存在
# 为准，choco 失败后改从 SourceForge 下载与 choco 同版本的便携 ZIP，用固定的 SHA256 校验后
# 解压到目标目录（ZIP 根目录的 makensis.exe 会调用 Bin\makensis.exe，目录结构必须保留）。
#
# 下载地址与校验值可用 NSIS_ZIP_URL / NSIS_ZIP_SHA256 覆盖，下载重试间隔（秒）可用
# NSIS_RETRY_DELAY 覆盖，测试用这些变量换成本地文件并去掉等待。

set -Eeuo pipefail

if [[ $# -ne 1 ]]; then
    printf '用法：%s <NSIS 目录>\n' "$(basename "$0")" >&2
    exit 2
fi

nsis_dir="$1"
makensis="${nsis_dir}/makensis.exe"
# SourceForge 公布的该文件 MD5 为 530e3623c077cba879ed2ae6e13be362，与此 SHA256 是同一文件。
zip_url="${NSIS_ZIP_URL:-https://downloads.sourceforge.net/project/nsis/NSIS%203/3.13/nsis-3.13.zip}"
zip_sha256="${NSIS_ZIP_SHA256:-ba63dffc4410ee89193e1cb5a41989991bd77c61068da17e3156d136b7b0b3d8}"
retry_delay="${NSIS_RETRY_DELAY:-15}"

if [[ -f "${makensis}" ]]; then
    printf 'NSIS 已安装：%s\n' "${makensis}"
    exit 0
fi

printf '==> 用 Chocolatey 安装 NSIS\n'
if choco install nsis -y --no-progress && [[ -f "${makensis}" ]]; then
    printf 'NSIS 已安装：%s\n' "${makensis}"
    exit 0
fi
printf '警告：Chocolatey 未能安装 NSIS，改为直接下载 %s\n' "${zip_url}"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/shellium-nsis.XXXXXX")"
trap 'rm -rf -- "${work_dir}"' EXIT
archive="${work_dir}/nsis.zip"

downloaded="false"
for attempt in 1 2 3; do
    if curl -fsSL --connect-timeout 30 -o "${archive}" "${zip_url}"; then
        actual_sha256="$(sha256_of "${archive}")"
        if [[ "${actual_sha256}" == "${zip_sha256}" ]]; then
            downloaded="true"
            break
        fi
        printf '警告：第 %d 次下载的 SHA256 不符：%s\n' "${attempt}" "${actual_sha256}"
    else
        printf '警告：第 %d 次下载失败\n' "${attempt}"
    fi
    (( attempt < 3 )) && sleep $(( retry_delay * attempt ))
done
[[ "${downloaded}" == "true" ]] || { printf '错误：无法下载校验通过的 NSIS 便携包\n' >&2; exit 1; }

# Git Bash 是否带 unzip 取决于版本；runner 预装了 7-Zip，最后再退到 PowerShell。
if command -v unzip >/dev/null 2>&1; then
    unzip -q "${archive}" -d "${work_dir}/extract"
elif command -v 7z >/dev/null 2>&1; then
    7z x -bd -y "-o${work_dir}/extract" "${archive}" >/dev/null
else
    powershell -NoProfile -Command "Expand-Archive -LiteralPath '$(cygpath -w "${archive}")' \
        -DestinationPath '$(cygpath -w "${work_dir}/extract")'"
fi
# ZIP 内只有一个 nsis-<版本> 顶层目录，把其内容整体放到目标目录。
extracted=("${work_dir}/extract"/*)
[[ ${#extracted[@]} -eq 1 && -d "${extracted[0]}" ]] \
    || { printf '错误：NSIS 便携包的目录结构与预期不符\n' >&2; exit 1; }
mkdir -p "${nsis_dir}"
cp -R "${extracted[0]}"/. "${nsis_dir}/"

[[ -f "${makensis}" ]] || { printf '错误：解压后仍找不到 %s\n' "${makensis}" >&2; exit 1; }
printf 'NSIS 已安装：%s\n' "${makensis}"

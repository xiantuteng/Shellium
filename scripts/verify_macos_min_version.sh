#!/usr/bin/env bash

# 检查目录内所有 Mach-O 声明的最低 macOS 版本都不高于发布目标。
#
# 用法：verify_macos_min_version.sh <目录> <最低系统，如 14.0> <架构，如 arm64>
#
# CMAKE_OSX_DEPLOYMENT_TARGET 只约束 Shellium 自己的代码；打进 Bundle 的 Qt 与 Homebrew 动态库
# 带着构建它们的系统版本（Homebrew 瓶子按构建机系统标注）。任何一个库高于目标版本，App 在
# 旧系统上都会在 dyld 阶段直接崩溃，而构建机上的冒烟测试完全看不出来，所以要逐个检查。

set -Eeuo pipefail

if [[ $# -ne 3 ]]; then
    printf '用法：%s <目录> <最低系统> <架构>\n' "$(basename "$0")" >&2
    exit 2
fi

root_dir="$1"
deployment_target="$2"
target_arch="$3"

# 比较两个点分版本号。返回 0 表示参数 1 严格大于参数 2；缺失的分量按 0 处理，因此 14 与 14.0
# 视为相等。
version_greater() {
    local -a left right
    IFS=. read -r -a left <<<"$1"
    IFS=. read -r -a right <<<"$2"
    local index
    for index in 0 1 2; do
        local l="${left[index]:-0}" r="${right[index]:-0}"
        if (( 10#${l} > 10#${r} )); then
            return 0
        elif (( 10#${l} < 10#${r} )); then
            return 1
        fi
    done
    return 1
}

# 输出指定 Mach-O 在目标架构切片上的最低系统。新链接器写 LC_BUILD_VERSION 的 minos，
# 旧链接器（Qt 官方安装器里的部分库）写 LC_VERSION_MIN_MACOSX 的 version，两种都要识别。
min_os_of() {
    otool -arch "${target_arch}" -l "$1" 2>/dev/null | awk '
        $1 == "cmd" { command = $2 }
        command == "LC_BUILD_VERSION" && $1 == "minos" { print $2; exit }
        command == "LC_VERSION_MIN_MACOSX" && $1 == "version" { print $2; exit }
    '
}

violations=()
checked=0
while IFS= read -r -d '' candidate_path; do
    file -b "${candidate_path}" 2>/dev/null | grep -q 'Mach-O' || continue
    checked=$((checked + 1))
    min_os="$(min_os_of "${candidate_path}")"
    if [[ -z "${min_os}" ]]; then
        violations+=("${candidate_path}（无法读取最低系统）")
    elif version_greater "${min_os}" "${deployment_target}"; then
        violations+=("${candidate_path}（要求 macOS ${min_os}）")
    fi
done < <(find "${root_dir}" -type f -print0)

if [[ "${checked}" -eq 0 ]]; then
    printf '错误：%s 内没有找到 Mach-O 文件。\n' "${root_dir}" >&2
    exit 1
fi

if [[ ${#violations[@]} -gt 0 ]]; then
    printf '错误：以下文件的最低系统高于发布目标 macOS %s：\n' "${deployment_target}" >&2
    printf '  %s\n' "${violations[@]}" >&2
    exit 1
fi

printf '已检查 %d 个 Mach-O，最低系统均不高于 macOS %s\n' "${checked}" "${deployment_target}"

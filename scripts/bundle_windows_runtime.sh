#!/usr/bin/env bash
set -euo pipefail

# 在 MSYS2（UCRT64 / CLANGARM64）原生构建后补齐 staging 的运行时 DLL。
#
# windeployqt 只收集 Qt 自身的 DLL 与插件；libssh、libsodium、OpenSSL 以及 MinGW/LLVM 运行时
# 都在 MSYS2 前缀的 bin 目录里，开发机 PATH 上能找到，换到干净 Windows 就启动失败。这里递归
# 扫描 staging 内全部 PE（含 Qt 插件）的导入表，把能在 MSYS2 前缀找到的 DLL 复制到 EXE 旁边，
# 直到不再新增为止；剩下的必须是系统自带 DLL，否则直接失败。
#
# 用法：bundle_windows_runtime.sh <staging 目录> <MSYS2 前缀 bin 目录>
# 环境变量 OBJDUMP 可指定 objdump（默认 objdump；CLANGARM64 下由 llvm-tools 提供同名命令）。

staging="$1"
runtime_dir="$2"
objdump="${OBJDUMP:-objdump}"
# SYSTEMROOT 是 C:\Windows 形式，MSYS2 的 find 需要 /c/Windows 形式。
system_dir="$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")/System32"

test -f "$staging/Shellium.exe"
test -d "$runtime_dir"

# 打印一个 PE 文件导入的全部 DLL 名称。
list_imports() {
  "$objdump" -p "$1" | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p' | tr -d '\r'
}

# 在目录中按 Windows 规则大小写不敏感地查找文件，找到时打印实际路径。
find_case_insensitive() {
  find "$1" -maxdepth 1 -type f -iname "$2" -print -quit
}

# Qt 的 OpenSSL TLS 插件在运行时用 LoadLibrary 加载 libssl，导入表里看不到。只靠导入表扫描会
# 漏掉它，HTTPS 请求就退回或直接失败，所以插件存在时显式带上 libssl。
if [[ -n "$(find "$staging" -type f -iname 'qopensslbackend*.dll' -print -quit)" ]]; then
  while IFS= read -r -d '' ssl_dll; do
    cp -f "$ssl_dll" "$staging/"
    printf '补齐运行时 DLL：%s\n' "$(basename "$ssl_dll")"
  done < <(find "$runtime_dir" -maxdepth 1 -type f -iname 'libssl-*.dll' -print0)
fi

# DLL 名称大小写不敏感；用 nocasematch 而不是 ${var,,}，脚本才能在 macOS 自带的 bash 3.2
# 上跑单元测试。
shopt -s nocasematch

while :; do
  added=0
  missing=()
  while IFS= read -r -d '' pe_file; do
    while IFS= read -r imported; do
      [[ -n "$imported" ]] || continue
      case "$imported" in
        api-ms-win-*|ext-ms-win-*)
          # API Set 是加载器内部的虚拟名称，磁盘上并不存在对应文件。
          continue
          ;;
      esac
      # Windows 加载器先在 EXE 所在目录查找，插件子目录里的 DLL 也依赖这一层。
      if [[ -n "$(find_case_insensitive "$staging" "$imported")" ]]; then
        continue
      fi
      source_dll="$(find_case_insensitive "$runtime_dir" "$imported")"
      if [[ -n "$source_dll" ]]; then
        cp -f "$source_dll" "$staging/"
        printf '补齐运行时 DLL：%s（%s 需要）\n' "$imported" "${pe_file#"$staging"/}"
        added=1
        continue
      fi
      # MSYS2 前缀找不到的才可能是系统 DLL。顺序不能反：前缀里有的必须打进包，哪怕构建机
      # System32 碰巧也有一份（例如 runner 上装过的软件留下的），用户机器上未必有。
      if [[ -n "$(find_case_insensitive "$system_dir" "$imported")" ]]; then
        continue
      fi
      missing+=("${pe_file#"$staging"/} -> $imported")
    done < <(list_imports "$pe_file")
  done < <(find "$staging" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)

  # 新复制的 DLL 自己也有导入表，必须再扫一轮，直到依赖闭包稳定。
  [[ "$added" -eq 1 ]] || break
done

if [[ "${#missing[@]}" -gt 0 ]]; then
  printf '缺少 PE 依赖：%s\n' "${missing[@]}" >&2
  exit 1
fi
printf 'staging 运行时依赖完整：%s\n' "$staging"

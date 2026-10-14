#!/usr/bin/env bash
set -euo pipefail

# Windows 原生构建后补齐 staging 的运行时 DLL。
#
# windeployqt 只收集 Qt 自身的 DLL 与插件；libssh、libsodium、OpenSSL（vcpkg）以及 MSVC 运行时
# （vcruntime/msvcp）分散在各自目录，开发机上靠 PATH 或已装的 VC 运行库能找到，换到干净 Windows
# 就启动失败。这里递归扫描 staging 内全部 PE（含 Qt 插件）的导入表，把能在给定目录中找到的 DLL
# 复制到 EXE 旁边，直到不再新增为止；剩下的必须是系统自带 DLL，否则直接失败。
#
# 用法：bundle_windows_runtime.sh <staging 目录> <DLL 搜索目录>...
#   搜索目录按顺序查找，例如 vcpkg 的 installed/<triplet>/bin、Qt 的 bin、VC 运行库的
#   Redist/MSVC/<版本>/<架构>/Microsoft.VC*.CRT。
# 环境变量 OBJDUMP 指定能输出 “DLL Name:” 导入表的命令（默认 objdump；MSVC 下用
# dumpbin_as_objdump.sh 包装 dumpbin）。在 Git Bash/MSYS2 中运行，需要 cygpath。

staging="$1"
shift
runtime_dirs=("$@")
objdump="${OBJDUMP:-objdump}"
# SYSTEMROOT 是 C:\Windows 形式，Git Bash 的 find 需要 /c/Windows 形式。
system_dir="$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")/System32"

test -f "$staging/Shellium.exe"
[[ "${#runtime_dirs[@]}" -gt 0 ]] || { echo "至少需要一个 DLL 搜索目录" >&2; exit 2; }
for runtime_dir in "${runtime_dirs[@]}"; do
  test -d "$runtime_dir" || { printf 'DLL 搜索目录不存在：%s\n' "$runtime_dir" >&2; exit 2; }
done

# 打印一个 PE 文件导入的全部 DLL 名称。
list_imports() {
  "$objdump" -p "$1" | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p' | tr -d '\r'
}

# 在目录中按 Windows 规则大小写不敏感地查找文件，找到时打印实际路径。
find_case_insensitive() {
  find "$1" -maxdepth 1 -type f -iname "$2" -print -quit
}

# 按顺序在全部搜索目录中查找，返回第一个命中的路径。
find_in_runtime_dirs() {
  local dir
  local found
  for dir in "${runtime_dirs[@]}"; do
    found="$(find_case_insensitive "$dir" "$1")"
    if [[ -n "$found" ]]; then
      printf '%s\n' "$found"
      return
    fi
  done
}

# 应用只用 QSQLITE。windeployqt 会把 Qt 自带的全部 SQL 驱动都部署进来，其中 Firebird、MySQL、
# PostgreSQL 驱动依赖构建机上也没有的客户端 DLL，留着只会让依赖检查失败，而且是死代码。
for driver in "$staging"/plugins/sqldrivers/*.dll; do
  [[ -e "$driver" ]] || continue
  case "$(basename "$driver")" in
    qsqlite.dll) ;;
    *) rm -f -- "$driver" ;;
  esac
done

# Qt 的 OpenSSL TLS 插件在运行时用 LoadLibrary 加载 libssl，导入表里看不到。只靠导入表扫描会
# 漏掉它，HTTPS 请求就退回或直接失败，所以插件存在时显式带上 libssl。
if [[ -n "$(find "$staging" -type f -iname 'qopensslbackend*.dll' -print -quit)" ]]; then
  while IFS= read -r -d '' ssl_dll; do
    cp -f "$ssl_dll" "$staging/"
    printf '补齐运行时 DLL：%s\n' "$(basename "$ssl_dll")"
  done < <(find "${runtime_dirs[@]}" -maxdepth 1 -type f -iname 'libssl-*.dll' -print0)
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
      source_dll="$(find_in_runtime_dirs "$imported")"
      if [[ -n "$source_dll" ]]; then
        cp -f "$source_dll" "$staging/"
        printf '补齐运行时 DLL：%s（%s 需要）\n' "$imported" "${pe_file#"$staging"/}"
        added=1
        continue
      fi
      # 搜索目录里找不到的才可能是系统 DLL。顺序不能反：搜索目录里有的必须打进包，哪怕构建机
      # System32 碰巧也有一份。典型例子是 vcruntime140.dll：runner 装过 VC 运行库，干净的用户
      # 机器上却未必有。
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

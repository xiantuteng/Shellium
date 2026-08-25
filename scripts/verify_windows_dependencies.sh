#!/usr/bin/env bash
set -euo pipefail

# 对 staging 中所有 PE 递归检查 import table。系统 DLL 可忽略，其余名称必须能在包内找到，
# 防止发布结果偷偷依赖 Docker PATH、开发机 Qt 或未复制的 OpenSSL/MinGW runtime。
staging="$1"
test -f "$staging/Shellium.exe"
available_file="$(mktemp)"
trap 'rm -f "$available_file"' EXIT
find "$staging" -type f -iname '*.dll' -printf '%f\n' | tr '[:upper:]' '[:lower:]' \
  | sort -u > "$available_file"

missing=0
while IFS= read -r pe_file; do
  while IFS= read -r imported; do
    normalized="$(printf '%s' "$imported" | tr '[:upper:]' '[:lower:]')"
    case "$normalized" in
      kernel32.dll|user32.dll|advapi32.dll|ws2_32.dll|shell32.dll|ole32.dll|oleaut32.dll|gdi32.dll|\
      comdlg32.dll|comctl32.dll|shlwapi.dll|crypt32.dll|bcrypt.dll|ncrypt.dll|secur32.dll|\
      iphlpapi.dll|dnsapi.dll|userenv.dll|version.dll|winmm.dll|dwmapi.dll|uxtheme.dll|ntdll.dll|\
      msvcrt.dll|rpcrt4.dll|setupapi.dll|imm32.dll|netapi32.dll|api-ms-win-*.dll|ext-ms-win-*.dll)
        continue
        ;;
    esac
    if ! grep -Fxiq "$normalized" "$available_file"; then
      printf '缺少 PE 依赖：%s -> %s\n' "$pe_file" "$imported" >&2
      missing=1
    fi
  done < <(x86_64-w64-mingw32-objdump -p "$pe_file" \
             | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p')
done < <(find "$staging" -type f \( -iname '*.exe' -o -iname '*.dll' \))

test "$missing" -eq 0

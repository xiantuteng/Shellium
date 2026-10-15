#!/usr/bin/env bash

# Shellium Linux deb / rpm 打包脚本，须在 package_linux_appimage.sh 之后运行。
#
# 直接复用 linuxdeploy 已收集好依赖的 AppDir：整体装到 /opt/shellium，再补 /usr/bin 启动脚本、
# 桌面文件与图标。工程要求的 Qt 6.8、libssh 0.11 在发行版仓库里普遍没有，按发行版依赖重新链接
# 不现实；和 AppImage 共用一份 AppDir 也保证三种包的库组合与启动行为（AppRun hook）完全一致。
#
# 包的依赖只声明 AppDir 没有携带的库（linuxdeploy 排除清单里的 glibc、GL、X11 等）：deb 在构建机
# 上用 dpkg -S 反查真实包名，rpm 用发行版无关的 soname 依赖。能运行的最低发行版与 AppImage 相同，
# 由构建机的 glibc 决定。
#
# 依赖：dpkg-deb、rpmbuild（Ubuntu 上的 rpm 包）、readelf、ldconfig。

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGET_ARCH="$(uname -m)"
BUILD_DIR="${SHELLIUM_BUILD_DIR:-${PROJECT_ROOT}/build-release-linux-${TARGET_ARCH}}"
DIST_DIR="${SHELLIUM_DIST_DIR:-${PROJECT_ROOT}/dist}"
APPDIR="${BUILD_DIR}/AppDir"
PROJECT_VERSION="${SHELLIUM_VERSION:-$(sed -nE 's/^project\(Shellium VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "${PROJECT_ROOT}/CMakeLists.txt" | head -n 1)}"

PACKAGE_NAME="shellium"
INSTALL_ROOT="/opt/shellium"
SUMMARY="SSH, SFTP and local terminal client"
HOMEPAGE="https://shellium.net"

log() {
    printf '\n==> %s\n' "$*"
}

fail() {
    printf '\n错误：%s\n' "$*" >&2
    exit 1
}

[[ "$(uname -s)" == "Linux" ]] || fail "该脚本只能在 Linux 上运行。"
[[ -n "${PROJECT_VERSION}" ]] || fail "无法从 CMakeLists.txt 读取项目版本。"
for tool in dpkg dpkg-deb rpmbuild readelf; do
    command -v "${tool}" >/dev/null 2>&1 || fail "缺少 ${tool}。"
done
# AppRun 是 linuxdeploy 生成的包装脚本，负责执行 apprun-hooks；缺了它说明 AppImage 打包没跑完。
[[ -x "${APPDIR}/AppRun" && -x "${APPDIR}/usr/bin/Shellium" ]] \
    || fail "未找到完整的 ${APPDIR}，请先运行 package_linux_appimage.sh。"

case "${TARGET_ARCH}" in
    x86_64) ld_arch_tag='x86-64' ;;
    aarch64) ld_arch_tag='AArch64' ;;
    *) fail "不支持的架构：${TARGET_ARCH}" ;;
esac
LDCONFIG="$(command -v ldconfig || echo /sbin/ldconfig)"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/shellium-pkg.XXXXXX")"
trap 'rm -rf -- "${WORK_DIR}"' EXIT
STAGE="${WORK_DIR}/root"

log "整理安装目录"
mkdir -p "${STAGE}${INSTALL_ROOT}" "${STAGE}/usr/bin" "${STAGE}/usr/share/applications" \
    "${STAGE}/usr/share/icons/hicolor/256x256/apps"
cp -a -- "${APPDIR}/." "${STAGE}${INSTALL_ROOT}/"
# AppRun 用自身所在目录定位 AppDir，/usr/bin 下只能放转发脚本，不能放符号链接。
cat > "${STAGE}/usr/bin/${PACKAGE_NAME}" <<EOF
#!/bin/sh
exec ${INSTALL_ROOT}/AppRun "\$@"
EOF
chmod 0755 "${STAGE}/usr/bin/${PACKAGE_NAME}"
sed -E "s/^Exec=.*/Exec=${PACKAGE_NAME}/" "${PROJECT_ROOT}/packaging/linux/shellium.desktop" \
    > "${STAGE}/usr/share/applications/${PACKAGE_NAME}.desktop"
cp -- "${PROJECT_ROOT}/packaging/linux/shellium.png" \
    "${STAGE}/usr/share/icons/hicolor/256x256/apps/${PACKAGE_NAME}.png"
chmod -R u+rwX,go+rX,go-w "${STAGE}"

log "分析系统库依赖"
elf_files=()
while IFS= read -r -d '' file; do
    if [[ "$(head -c 4 -- "${file}" | od -An -c | tr -d ' ')" == '177ELF' ]]; then
        elf_files+=("${file}")
    fi
done < <(find "${STAGE}${INSTALL_ROOT}" -type f -print0)
[[ "${#elf_files[@]}" -gt 0 ]] || fail "AppDir 中没有找到 ELF 文件。"

# 包内自带的库：usr/lib 下的文件名与 SONAME。opengl-fallback 只在系统缺库时启用，不算自带。
declare -A bundled=()
while IFS= read -r -d '' lib; do
    bundled["$(basename "${lib}")"]=1
    soname="$(readelf -d "${lib}" 2>/dev/null | sed -nE 's/.*\(SONAME\).*\[(.*)\]/\1/p')"
    if [[ -n "${soname}" ]]; then
        bundled["${soname}"]=1
    fi
done < <(find "${STAGE}${INSTALL_ROOT}/usr/lib" -path '*/opengl-fallback' -prune -o \( -type f -o -type l \) -print0)

system_sonames=()
while IFS= read -r soname; do
    if [[ -z "${bundled[${soname}]:-}" ]]; then
        system_sonames+=("${soname}")
    fi
done < <(for file in "${elf_files[@]}"; do
        readelf -d "${file}" 2>/dev/null | sed -nE 's/.*\(NEEDED\).*\[(.*)\]/\1/p'
    done | sort -u)
[[ "${#system_sonames[@]}" -gt 0 ]] || fail "没有分析出任何系统库依赖，结果不可信。"

# soname → 构建机上提供它的 deb 包。usrmerge 后 ldconfig 给出的路径和 dpkg 数据库里登记的路径
# 可能一个带 /usr、一个不带，几种写法都试一遍。
resolve_deb_package() {
    local soname="$1" path candidate package
    path="$("${LDCONFIG}" -p | awk -v name="${soname}" -v tag="${ld_arch_tag}" \
        '$1 == name && index($0, tag) { print $NF; exit }')"
    [[ -n "${path}" ]] || return 1
    for candidate in "${path}" "$(readlink -f -- "${path}")" "${path#/usr}" "/usr${path}"; do
        package="$(dpkg -S -- "${candidate}" 2>/dev/null | grep -v '^diversion' | head -n 1 | cut -d: -f1 || true)"
        if [[ -n "${package}" ]]; then
            printf '%s\n' "${package}"
            return 0
        fi
    done
    return 1
}

glibc_version="$(getconf GNU_LIBC_VERSION | awk '{print $2}')"
declare -A deb_packages=()
rpm_requires=("glibc >= ${glibc_version}")
for soname in "${system_sonames[@]}"; do
    package="$(resolve_deb_package "${soname}")" \
        || fail "无法确定提供 ${soname} 的 deb 包：请在构建机上安装它，或确认它应当被打进 AppDir。"
    deb_packages["${package}"]=1
    rpm_requires+=("${soname}()(64bit)")
    printf '  %-28s %s\n' "${soname}" "${package}"
done
unset 'deb_packages[libc6]'
deb_depends="libc6 (>= ${glibc_version})"
while IFS= read -r package; do
    if [[ -n "${package}" ]]; then
        deb_depends="${deb_depends}, ${package}"
    fi
done < <(printf '%s\n' "${!deb_packages[@]}" | sort)

mkdir -p "${DIST_DIR}"
DEB_OUTPUT="${DIST_DIR}/Shellium-${PROJECT_VERSION}-linux-${TARGET_ARCH}.deb"
RPM_OUTPUT="${DIST_DIR}/Shellium-${PROJECT_VERSION}-linux-${TARGET_ARCH}.rpm"

log "生成 deb"
DEB_ROOT="${WORK_DIR}/deb"
cp -a -- "${STAGE}" "${DEB_ROOT}"
mkdir -p "${DEB_ROOT}/DEBIAN"
cat > "${DEB_ROOT}/DEBIAN/control" <<EOF
Package: ${PACKAGE_NAME}
Version: ${PROJECT_VERSION}
Architecture: $(dpkg --print-architecture)
Maintainer: Shellium contributors
Installed-Size: $(du -sk "${STAGE}" | cut -f1)
Depends: ${deb_depends}
Section: net
Priority: optional
Homepage: ${HOMEPAGE}
Description: ${SUMMARY}
 Shellium is a cross-platform SSH, SFTP and local terminal client built with Qt.
EOF
dpkg-deb --root-owner-group -Zxz --build "${DEB_ROOT}" "${DEB_OUTPUT}"

log "生成 rpm"
RPM_TOP="${WORK_DIR}/rpmbuild"
mkdir -p "${RPM_TOP}/SPECS"
{
    cat <<EOF
# 包内容是预先编好的 AppDir：关闭自动依赖分析（否则会把包内的 Qt 等库声明成对外提供的能力），
# 也不做 strip、debuginfo 与 build-id 链接，保持与 AppImage 中的文件完全一致。
%global debug_package %{nil}
%global __os_install_post %{nil}
%global _build_id_links none

Name:           ${PACKAGE_NAME}
Version:        ${PROJECT_VERSION}
Release:        1
Summary:        ${SUMMARY}
License:        Apache-2.0
URL:            ${HOMEPAGE}
AutoReqProv:    no
EOF
    for requirement in "${rpm_requires[@]}"; do
        printf 'Requires:       %s\n' "${requirement}"
    done
    cat <<EOF

%description
Shellium is a cross-platform SSH, SFTP and local terminal client built with Qt.

%install
cp -a ${STAGE}/. %{buildroot}/

%files
${INSTALL_ROOT}
/usr/bin/${PACKAGE_NAME}
/usr/share/applications/${PACKAGE_NAME}.desktop
/usr/share/icons/hicolor/256x256/apps/${PACKAGE_NAME}.png
EOF
} > "${RPM_TOP}/SPECS/${PACKAGE_NAME}.spec"
# xz 载荷兼容面最广；默认的 zstd 在部分较老的 rpm 上无法解包。
rpmbuild -bb --target "${TARGET_ARCH}" \
    --define "_topdir ${RPM_TOP}" \
    --define "_binary_payload w6.xzdio" \
    "${RPM_TOP}/SPECS/${PACKAGE_NAME}.spec"
built_rpm="$(find "${RPM_TOP}/RPMS" -name '*.rpm' -print -quit)"
[[ -n "${built_rpm}" ]] || fail "rpmbuild 未生成 rpm。"
mv -f -- "${built_rpm}" "${RPM_OUTPUT}"

printf '\n构建完成：\n  %s\n  %s\n' "${DEB_OUTPUT}" "${RPM_OUTPUT}"

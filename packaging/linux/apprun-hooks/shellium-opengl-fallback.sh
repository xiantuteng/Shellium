# shellcheck shell=bash
#
# Shellium AppRun hook：系统缺 libOpenGL.so.0 时改用包内备份。
#
# 官方 Qt 二进制（install-qt-action 安装）的 libQt6Gui 直接链接 GLVND 的 libOpenGL.so.0，但
# AppImage 排除清单不允许把它打进 usr/lib（须与系统的 libGLdispatch 配套）。桌面版 Ubuntu 默认
# 不装 libopengl0，0.3.3 因此在 Ubuntu 26.04 上启动即报 libOpenGL.so.0 缺失。这里只在系统确实
# 没有时才把备份目录加入 LD_LIBRARY_PATH，系统有就始终用系统那份。
#
# 终端里启动的 shell 会继承该变量；目录里只有这一个库，且只在缺库的系统上生效，影响可以接受。
#
# linuxdeploy 生成的 AppRun 以 set -e 方式 source 本文件，并已定义 this_dir（AppDir 根目录）。

case "$(uname -m)" in
    x86_64) shellium_ld_arch='x86-64' ;;
    aarch64) shellium_ld_arch='AArch64' ;;
    *) shellium_ld_arch='' ;;
esac
# Debian 普通用户的 PATH 不含 /sbin，先试绝对路径。多架构系统上只有 32 位条目时不能算有，
# 因此连同架构标记一起匹配。
shellium_ld_cache="$( { /sbin/ldconfig -p || ldconfig -p; } 2>/dev/null || true)"
if ! printf '%s\n' "${shellium_ld_cache}" \
        | grep -Eq "^[[:space:]]*libOpenGL\.so\.0 \(.*${shellium_ld_arch}"; then
    export LD_LIBRARY_PATH="${this_dir}/usr/lib/opengl-fallback${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi
unset shellium_ld_arch shellium_ld_cache

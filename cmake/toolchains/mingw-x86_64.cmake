# Shellium Windows x64 交叉工具链。依赖前缀由调用方显式传入，避免偷偷引用开发机 PATH，
# 从而保证 Docker、Linux CI 与 macOS Docker 使用同一套 Windows ABI 和依赖树。
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

if(NOT DEFINED SHELLIUM_WINDOWS_SYSROOT)
    message(FATAL_ERROR "必须通过 -DSHELLIUM_WINDOWS_SYSROOT=... 指定已锁定的 Windows 依赖前缀")
endif()
set(CMAKE_FIND_ROOT_PATH "${SHELLIUM_WINDOWS_SYSROOT}")

# 程序是构建主机上运行的 moc/uic/rcc，库和头文件则必须来自 Windows sysroot。混用会产生
# “配置成功但最终链接到 Linux/macOS 库”的隐蔽错误，因此三种查找模式必须明确拆开。
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_CXX_FLAGS_INIT "-DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN")

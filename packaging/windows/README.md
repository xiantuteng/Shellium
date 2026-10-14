# Windows 构建与发布

Shellium 第一阶段支持 Windows 10 1809+ / Windows 11 x64，同时继续支持 macOS 与 Linux。
Windows 本地终端使用 ConPTY，SSH 隧道使用指针宽度原生 socket，私钥创建时应用受保护 ACL。

## Windows 原生构建

正式发布包使用 MSVC 2022 + Qt 官方 MSVC 二进制 + vcpkg 依赖，完整步骤见下文“MSVC 原生构建”。
Qt 与第三方库必须同一 ABI；依赖优先从 CMake Config Package 查找，找不到时工程会回退 pkg-config。
下面是不经 vcpkg 清单、依赖已就绪时的最简命令：

```powershell
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 -DBUILD_TESTING=ON
cmake --build build-win --parallel
ctest --test-dir build-win --output-on-failure
.\scripts\package_windows_native.ps1 -BuildDirectory build-win -Configuration Release
```

正式发布前必须在未安装 Qt/MinGW/OpenSSL 的干净 Windows VM 验证 PowerShell、cmd、中文路径、
SSH/SFTP、-L/-R/-D、安装升级和卸载。Docker/Wine 只能验证 PE 依赖与辅助冒烟，不能替代真实
ConPTY、DPI、输入法和 Authenticode 测试。签名密钥不得写入仓库、镜像层或 CMake 缓存。

## Docker 交叉构建接口

使用 `cmake/toolchains/mingw-x86_64.cmake`，并把已锁定版本、已校验 SHA-256 的 Qt 及第三方
Windows 依赖挂载为 `SHELLIUM_WINDOWS_SYSROOT`。Qt 的主机工具必须能在容器 Linux 上运行，
目标库则必须来自该 Windows sysroot。交叉部署不能调用 `windeployqt`，发布流水线应递归扫描
PE import table，复制 Qt 插件与 MinGW runtime，再生成 ZIP、NSIS、许可证和 SHA-256。

当前仓库不内置签名证书或未经校验的第三方二进制依赖；CI/发布环境负责提供锁定 sysroot。
package_windows_native.ps1 会生成便携 ZIP、单用户 NSIS 安装器和 SHA-256 清单。正式发布需
在生成校验和之前对自有 EXE/DLL 和最终安装器签名。

锁定的交叉依赖包需要包含 host 与 target 两个目录。配置 URL 和校验和后，可在 macOS/Linux
执行 bash scripts/build_windows_docker.sh；脚本固定构建 linux/amd64 容器，并在生成 ZIP/NSIS
前递归验证全部 PE 导入依赖。

## MSVC 原生构建（CI 使用）

`.github/workflows/release.yml` 在 GitHub 的 Windows runner 上用 MSVC 2022 构建 x64 与 ARM64：

- Qt 使用官方 MSVC 二进制（`win64_msvc2022_64` / `win64_msvc2022_arm64`，6.8.3）；
- 其余依赖由 vcpkg 按 `packaging/windows/vcpkg/vcpkg.json` 构建，`builtin-baseline` 固定了全部
  依赖版本。vcpkg 官方没有 libvterm port，仓库内的 overlay（`packaging/windows/vcpkg/ports/libvterm`）
  按 Neovim 的做法用最小 CMakeLists 编出静态库，导出 `unofficial::libvterm`；
- windeployqt 只收集 Qt 的 DLL 与插件，且 MSVC 下不再复制 vc_redist 安装程序。vcpkg 的库（含
  OpenSSL）与 vcruntime/msvcp 由 `scripts/bundle_windows_runtime.sh` 递归扫描导入表后以 app-local
  方式放到 EXE 旁边，仍找不到的非系统 DLL 会让构建失败。导入表用 `scripts/dumpbin_as_objdump.sh`
  包装 MSVC 自带的 dumpbin 读取。

本地复现（“x64 Native Tools” 或 “ARM64 Native Tools” 命令行，Git Bash 可用）：

```powershell
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_MANIFEST_DIR=packaging/windows/vcpkg -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build-win --parallel
cmake --install build-win --prefix build-win/windows-staging
```

```bash
# Git Bash，CRT 目录换成本机 VC Redist 下的 Microsoft.VC*.CRT
OBJDUMP="$PWD/scripts/dumpbin_as_objdump.sh" bash scripts/bundle_windows_runtime.sh \
  build-win/windows-staging build-win/vcpkg_installed/x64-windows/bin \
  /c/Qt/6.8.3/msvc2022_64/bin "$(cygpath -u "$VCToolsRedistDir")/x64/Microsoft.VC143.CRT"
```

```powershell
.\scripts\package_windows_native.ps1 -BuildDirectory build-win -Architecture x64 -UseExistingStaging
```

`-Architecture` 只影响文件名：x64 保持 `Shellium-<版本>-win64.zip` / `Shellium-<版本>-setup-x64.exe`，
arm64 为 `Shellium-<版本>-win-arm64.zip` / `Shellium-<版本>-setup-arm64.exe`。NSIS 安装器本身是
x86 程序，在 ARM64 Windows 上经系统转译运行。

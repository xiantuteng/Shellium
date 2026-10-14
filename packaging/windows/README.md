# Windows 构建与发布

Shellium 第一阶段支持 Windows 10 1809+ / Windows 11 x64，同时继续支持 macOS 与 Linux。
Windows 本地终端使用 ConPTY，SSH 隧道使用指针宽度原生 socket，私钥创建时应用受保护 ACL。

## Windows 原生构建

建议使用 Qt 6.8 MinGW x64 与相同 ABI 的 libssh、libvterm、libsodium、spdlog。依赖可由
vcpkg/CMake Config 提供；找不到 Config Package 时工程会回退 pkg-config。

```powershell
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64 -DBUILD_TESTING=ON
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

## MSYS2 原生构建（CI 使用）

`.github/workflows/release.yml` 在 GitHub 的 Windows runner 上用 MSYS2 构建两种架构：x64 用
UCRT64（MINGW64 环境没有 libvterm 包），ARM64 用 CLANGARM64。CLANGARM64 没有 NSIS 包，两种
架构都使用 UCRT64 的 makensis；生成的安装器是 x86 程序，在 ARM64 Windows 上经系统转译运行。

windeployqt 只收集 Qt 自身的 DLL，libssh、OpenSSL 与 MinGW/LLVM 运行时由
`scripts/bundle_windows_runtime.sh` 递归扫描 staging 内全部 PE 导入表后从 MSYS2 前缀补齐，
仍找不到的非系统 DLL 会让构建失败。之后以 `-UseExistingStaging` 调用打包脚本，避免清空补齐结果：

```bash
# MSYS2 UCRT64 / CLANGARM64 shell
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-win --parallel
cmake --install build-win --prefix build-win/windows-staging
bash scripts/bundle_windows_runtime.sh build-win/windows-staging "$MINGW_PREFIX/bin"
```

```powershell
.\scripts\package_windows_native.ps1 -BuildDirectory build-win -Architecture arm64 -UseExistingStaging
```

`-Architecture` 只影响文件名：x64 保持 `Shellium-<版本>-win64.zip` / `Shellium-<版本>-setup-x64.exe`，
arm64 为 `Shellium-<版本>-win-arm64.zip` / `Shellium-<版本>-setup-arm64.exe`。

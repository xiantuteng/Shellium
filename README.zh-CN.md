<div align="center">

<img src="resources/icons/shellium-app-icon.png" alt="Shellium" width="128">

# Shellium

**跨平台 SSH 终端：无论跳到第几层，文件管理器都跟着你走。**

在会话里执行 `ssh`，SFTP 面板会自动切到你当前所在的服务器，并经正确的跳板机隧道连接。

[![Release](https://img.shields.io/github/v/release/xiantuteng/Shellium?include_prereleases&sort=semver)](https://github.com/xiantuteng/Shellium/releases)
[![Build](https://github.com/xiantuteng/Shellium/actions/workflows/release.yml/badge.svg)](https://github.com/xiantuteng/Shellium/actions/workflows/release.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platform-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey)
![Qt](https://img.shields.io/badge/Qt-6.8%2B-41CD52?logo=qt&logoColor=white)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)

[English](README.md) · 简体中文 · [官网](https://shellium.net)

</div>

---

## 为什么选择 Shellium？

大多数 SSH 客户端的文件浏览器只认最初连上的那台主机。一旦从堡垒机 `ssh` 进内网机器，文件浏览器显示的仍然是堡垒机。

Shellium 为每个标签页维护一条从外到内的完整服务器链：

```text
本机 ──ssh──▶ 堡垒机 ──ssh──▶ 应用服务器 ──ssh──▶ 数据库服务器
                                                       ▲
                      SFTP 面板跟到这里，经 堡垒机 → 应用服务器 隧道连接
```

- 在终端里执行 `ssh user@host` 时，Shellium 会接管这一层，建立自己持有的会话。进入、认证成功、退出都是确定的事件，不靠终端标题猜测。
- 远端 Shell 无法集成时，降级为解析 `ssh` 命令。这样识别出的层会在界面和高风险操作中明确标记为**推测**。
- 文件管理器默认指向最内层服务器，也可以手动切到任意外层。内层 SFTP 始终走正确的外层隧道，不会从本机误连同名的内网地址。
- 标签页之间完全隔离：终端、服务器链、SFTP 路径、传输队列和会话凭据都不跨标签共享。

## 功能

**终端**
- 基于 libvterm 的 VT/xterm 仿真：256 色与真彩色、备用屏幕、双宽（中日韩）字符、可配置滚动历史
- 本地 Shell：macOS/Linux 用 PTY，Windows 用 ConPTY；多标签页；工作区内广播输入
- 主题（跟随系统 / 亮色 / 暗色）、内置等宽字体，以及光标、铃声、滚动设置
- 一键重置终端（<kbd>⌘⇧R</kbd> / <kbd>Ctrl+Shift+R</kbd>），误 `cat` 二进制文件导致花屏后可恢复

**SSH 与连接**
- 主机树：嵌套分组、配置继承、标签、筛选、最近连接和连接历史
- 可视化主机链（跳板机），复用已保存的凭据；同时保留手动 ProxyJump 入口
- 本地、远程、动态端口转发（`-L` / `-R` / `-D`），规则持久化、自动启动、流量统计
- 断线后有限次自动重连，失败时回退
- 首次连接必须核对 SHA-256 主机指纹。`hmac-sha1` 等旧算法只有在你确认后才启用，而且只对该主机生效。
- 导入 OpenSSH `config`；Shellium JSON 导入/导出（从不导出凭据）

**远程文件（SFTP）**
- 本地/远程双栏、路径直达、双向拖拽
- 递归传输、断点续传，传输队列可暂停、恢复、取消
- 文本和图片预览、`chmod`、重命名、删除
- 在终端里右键即可上传到当前远程目录

**安全**
- 可选主密码保险箱：Argon2id + XChaCha20-Poly1305（libsodium），闲置自动锁定
- Ed25519 / RSA / ECDSA 密钥生成、已有私钥检查、公钥复制
- 私钥以最小权限写入（POSIX 0600，Windows 受保护 ACL）

**效率**
- 命令片段（Snippet）、带补全的命令历史、多主机执行
- 终端内快捷面板：<kbd>⌘K</kbd> 执行片段，<kbd>⌘J</kbd> 执行历史命令
- 命令面板；工作区保存、校验、恢复
- AI 助手（可选，需自备 OpenAI 兼容接口）。任何会改动系统的命令都必须由你逐条确认后才执行。

**多语言**：界面支持 14 种语言：English、简体中文、繁體中文、Français、Deutsch、Italiano、Español、Português (Brasil)、Română、Русский、Українська、日本語、한국어、العربية。

<details>
<summary><b>快捷键</b>（macOS；Windows/Linux 将 <kbd>⌘</kbd> 换成 <kbd>Ctrl</kbd>）</summary>

| 快捷键 | 作用 |
| --- | --- |
| <kbd>⌘K</kbd> | 在当前终端执行命令片段 |
| <kbd>⌘J</kbd> | 执行历史命令（<kbd>⌘1</kbd>–<kbd>⌘9</kbd> 选择对应条目） |
| <kbd>⇧⌘F</kbd> | 在侧栏搜索主机。<kbd>Enter</kbd> 连接，<kbd>⇧Enter</kbd> 经主机链连接 |
| <kbd>⇧⌘O</kbd> | 显示或隐藏远程文件管理器 |
| <kbd>⇧⌘R</kbd> | 在本地重置终端（不向远端发送命令） |

</details>

## 安装

从 **[Releases](https://github.com/xiantuteng/Shellium/releases)** 下载最新版本。

| 平台 | 架构 | 安装包 | 最低系统 |
| --- | --- | --- | --- |
| macOS | Apple Silicon、Intel | `.dmg` | macOS 14（arm64）、macOS 15（x86_64） |
| Windows | x64、ARM64 | NSIS 安装器、`.zip` | Windows 10 1809 / Windows 11 |
| Linux | x86_64、aarch64 | AppImage、`.deb`、`.rpm` | glibc 2.39（如 Ubuntu 24.04+、Debian 13+、Fedora 40+） |

> [!NOTE]
> 目前发布包尚未做代码签名。CI 构建的 macOS 包只有 ad-hoc 签名，首次启动会被 Gatekeeper 拦截：右键应用选择**打开**，或执行 `xattr -dr com.apple.quarantine /Applications/Shellium.app`。Windows 上 SmartScreen 可能要求确认。每个版本都附带 `SHA256SUMS.txt`，可用来校验下载文件。

## 从源码构建

### 依赖

- CMake 3.21+、Ninja、支持 C++20 的编译器
- Qt 6.8+（Core、Widgets、Concurrent、Network、Sql、Svg、LinguistTools；跑测试还需要 Test）
- libssh 0.11+、libvterm 0.3+、libsodium 1.0.18+、spdlog 1.12+、minizip

<details>
<summary><b>macOS</b>（Homebrew）</summary>

```bash
brew install qtbase qtsvg qttools libssh libvterm libsodium spdlog minizip ninja pkg-config
```

</details>

<details>
<summary><b>Linux</b>（Ubuntu 24.04+）</summary>

Ubuntu 仓库自带的 Qt 是 6.4、libssh 是 0.10，都低于 Shellium 的要求。先用 [Qt 在线安装器](https://www.qt.io/download-qt-installer-oss) 或 [aqtinstall](https://github.com/miurahr/aqtinstall) 安装 Qt 6.8+，然后：

```bash
sudo apt-get install ninja-build pkg-config libssl-dev libvterm-dev libsodium-dev \
    libspdlog-dev libminizip-dev zlib1g-dev libgl1-mesa-dev libxkbcommon-x11-0 libxcb-cursor0
sudo ./scripts/install_libssh_from_source.sh /usr/local   # libssh 0.11
```

</details>

<details>
<summary><b>Windows</b>（MSVC 2022 + vcpkg）</summary>

正式发布包使用 MSVC 2022、Qt 官方 MSVC 二进制，以及 [`packaging/windows/vcpkg`](packaging/windows/vcpkg) 中的 vcpkg 清单。详见 [Windows 构建指南](packaging/windows/README.md)。

</details>

### 构建与测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

在没有显示器的机器上，GUI 测试需加 `QT_QPA_PLATFORM=offscreen`。不要用 `ctest -j` 并行跑，因为这些测试共用同一个显示环境。

macOS 应用包生成在 `build/Shellium.app`。

`ShelliumSshIntegrationTests` 通过 `SHELLIUM_IT_*` 环境变量连接真实的测试服务器，未设置时自动跳过。该套件会故意制造登录失败，不要指向生产服务器，以免触发 `MaxStartups` 或 fail2ban。

### 打包

| 目标 | 脚本 |
| --- | --- |
| macOS DMG（Developer ID 签名 + 公证） | [`scripts/package_macos_dmg.sh`](scripts/package_macos_dmg.sh)，见[指南](docs/MACOS_DMG_RELEASE.md) |
| Windows NSIS / ZIP | [`scripts/package_windows_native.ps1`](scripts/package_windows_native.ps1)，见[指南](packaging/windows/README.md) |
| Linux AppImage | [`scripts/package_linux_appimage.sh`](scripts/package_linux_appimage.sh) |
| Linux deb / rpm（在 AppImage 脚本之后运行） | [`scripts/package_linux_deb_rpm.sh`](scripts/package_linux_deb_rpm.sh) |

[发版工作流](.github/workflows/release.yml)在各架构的原生 runner 上构建全部安装包，并逐个做启动冒烟测试；Linux 的 deb/rpm 还会在干净的 Ubuntu、Debian、Fedora 容器里完成安装、启动与卸载。推送与 `CMakeLists.txt` 中 `project(VERSION)` 一致的 `v*` 标签后，会生成附带 `SHA256SUMS.txt` 的**草稿** Release。

## 路线图

尚未实现：

- 终端鼠标报告、块选择、可点击 URL、自动会话日志
- SFTP 递归删除目录
- 通过外部 SOCKS5/HTTP 代理建立 SSH 连接（这与已支持的 `-D` 动态转发不是一回事）
- 从 Termius / PuTTY / MobaXterm 导入、系统钥匙串集成、云同步

macOS arm64 是主要开发平台。其他平台由 CI 构建并做启动冒烟测试，但实机测试较少，特别欢迎[反馈问题](https://github.com/xiantuteng/Shellium/issues)。

## 参与贡献

欢迎提交 Issue 和 Pull Request。请先阅读 [`AGENTS.md`](AGENTS.md)，里面是每次改动都必须遵守的规则：

- **必须补测试。** 新功能和 BUG 修复都要附带自动化用例。修 BUG 时先临时撤掉修复，确认新用例确实会失败。
- **译文同步。** 改动任何界面可见文字（`tr()` / `QT_TRANSLATE_NOOP`）时，必须在同一次提交里更新其余 13 种语言。见 [`docs/I18N_DESIGN.md`](docs/I18N_DESIGN.md)。
- **第三方许可证。** 新打包进安装包的依赖必须登记到 [`resources/licenses/`](resources/licenses/README.md)。LGPL 组件必须保持动态链接。

改动会话栈、libssh 或终端渲染之前，请先读 [`docs/ENGINEERING_NOTES.md`](docs/ENGINEERING_NOTES.md)。其中记录了架构约束，以及只在第 2 层及以上嵌套才会复现的问题。

## 致谢

Shellium 基于 [Qt](https://www.qt.io/)、[libssh](https://www.libssh.org/)、[libvterm](https://www.leonerd.org.uk/code/libvterm/)、[libsodium](https://libsodium.org/) 和 [spdlog](https://github.com/gabime/spdlog) 构建，并内置了多款开源等宽字体。完整的第三方组件及许可证见应用内**设置 → 关于 → 查看许可证**，以及 [`resources/licenses/`](resources/licenses)。

## 许可证

Copyright © 2026 Shellium contributors.

Shellium 以 [Apache License 2.0](LICENSE) 授权发布。你可以在遵守该许可证的前提下自由使用、修改和分发本项目（包括用于商业用途）；软件按"原样"提供，不附带任何形式的担保。

Shellium 捆绑的第三方组件仍遵循各自的许可证，清单见 [`resources/licenses/`](resources/licenses)。

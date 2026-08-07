# Shellium

Shellium 是基于 Qt 6 和 libssh 开发的跨平台 SSH 终端与远程文件管理工具。

当前首个可运行版本聚焦两项核心能力：

- 针对当前服务器执行 SFTP 路径浏览、上传、下载、重命名和删除；
- 识别终端内继续执行的 `ssh`/`exit`，展示外层到内层服务器链，并允许文件管理器自由切换目标。

当前基础设施还包括：

- libvterm VT220/xterm 状态机、256 色/TrueColor、备用屏幕和 10000 行滚动历史；
- 基于真实 PTY 的本地登录式 zsh，可作为本地终端和自动化回归环境；
- 可移动、关闭、重命名的多标签页，每个标签拥有独立终端与远程文件管理器；
- SQLite WAL 数据层，覆盖主机、凭据、Snippets、历史、转发、工作区、设置和插件状态；
- 基于 Argon2id 与 XChaCha20-Poly1305 的加密保险库核心。

## 构建

需要 CMake 3.21+、Qt 6.8+、libssh 0.10+、pkg-config 和支持 C++20 的编译器。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

macOS 可执行文件位于：

```text
build/Shellium.app/Contents/MacOS/Shellium
```

## 使用要点

1. 点击“连接服务器”，输入主机、用户、端口和认证信息。
2. 需要同时操作多台服务器时，用“新建标签页”开一个新的终端会话。
3. 右侧远程文件管理器默认跟随当前终端最内层服务器。
4. 在终端内输入 `ssh user@host` 后，服务器选择器会自动增加一层；输入 `exit` 或按 `Ctrl+D` 会返回一层。
5. 首次使用 SFTP 连接未知服务器时，必须核对并确认 SHA-256 主机指纹。

如果旧服务器仅支持 `hmac-sha1`，Shellium 会在协商失败后展示安全提示。只有用户明确确认后，
才会为当前目标的终端和 SFTP 连接启用该兼容算法；其他服务器仍保持默认 SHA-2 安全策略。

## 当前边界

- libvterm 状态已接入 Qt 富文本文档渲染；后续仍需把鼠标报告、选区和超大滚动历史优化为专用自绘 Widget。
- 嵌套服务器识别目前基于用户提交的 SSH 命令。复杂 Shell alias、脚本内 SSH 或从历史记录直接执行时，需要后续 Shell Integration 事件协议进一步确认。
- SFTP 删除操作支持文件和空目录；递归目录删除应作为独立高风险功能设计。

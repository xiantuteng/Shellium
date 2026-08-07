# Shellium — SSH 终端软件功能规划

> 基于 Qt 6.8 LTS 开发的跨平台 SSH 终端管理工具，对标 Termius

---

## 〇、实现状态总览

> 本节由代码核对得出，**每次改动功能后必须同步更新**。状态记号：
> ✅ 已实现 ｜ ⚠️ 部分实现（括号内说明缺口）｜ ❌ 未实现
>
> 工程约束、已知陷阱与测试规范见 **[ENGINEERING_NOTES.md](ENGINEERING_NOTES.md)**。
> 接手开发前请先读那一份——其中多数条目是踩过坑之后才总结出来的，重犯代价很高。

### P0 完成度

| 状态 | 数量 | 说明 |
|------|------|------|
| ✅ 已实现 | 约 20 | 终端、嵌套连接链、SFTP 基础与递归传输、持久化读写层 |
| ⚠️ 部分实现 | 约 9 | 多为「内核已就绪，缺 UI 或缺配置项」 |
| ❌ 未实现 | 约 20 | 主机管理 UI、凭据保险库接入、端口转发、效率工具、主题 |

### 已经做扎实的纵深

**终端 → 嵌套连接链 → SFTP** 这条主线完成度最高，也是本项目的差异化所在：

- 每一层嵌套 SSH 都是本程序**亲自持有的会话**（`SshSessionStack` + `SshSessionNode`），而不是对终端输出的推测。层的进入与退出都是确定事件
- 服务器禁用 `AllowTcpForwarding` 时自动降级到 **exec 中继**，功能不受影响
- Shell 集成脚本经独立 exec 通道下发，终端里不留痕迹
- 94 个单元测试（4 个目标）+ 19 个针对真实服务器的集成测试

### 最关键的缺口

1. **凭据保险库未接入**：`Vault` 与 `credentials` 表的读写接口都已就绪，但**没有任何地方调用**。密码目前只活在进程内存里，关掉即丢
2. **主机管理无 UI**：数据层完整（增删改查、分组继承、搜索），但界面上只有一个用完即弃的连接对话框，没有主机树
3. **端口转发整块空白**：`port_forwards` 表已建，无任何代码

---

## 一、项目概述

### 1.1 项目定位

Shellium 是一款现代化的跨平台 SSH 终端软件，旨在提供媲美 Termius 的用户体验，同时保持本地优先、隐私安全的理念。

### 1.2 目标平台

| 平台 | 优先级 | 说明 |
|------|--------|------|
| macOS (arm64 / x86_64) | P0 | 主力开发平台 |
| Linux (x86_64) | P0 | CI/CD 构建验证 |
| Windows (x86_64) | P1 | 后续适配 |

### 1.3 技术栈选型

| 层面 | 选型 | 理由 |
|------|------|------|
| GUI 框架 | Qt 6.8 LTS | 长期支持，API 稳定 |
| 构建系统 | CMake 3.21+ | Qt 官方推荐，CLion 原生支持 |
| 语言标准 | C++20 | 协程、概念、模块支持 |
| SSH 协议库 | libssh 0.10+ | 功能完备：跳板机、Agent 转发、证书认证 |
| 终端模拟 | libvterm + 自绘 Widget | 轻量、可控、不依赖第三方 Qt 终端组件 |
| 本地存储 | SQLite + Qt SQL | 单文件数据库，零配置 |
| 加密存储 | libsodium / OpenSSL | 凭据保险库端到端加密 |
| JSON 处理 | Qt JSON (QJsonDocument) | 原生集成 |
| 日志框架 | spdlog | 轻量高效，异步日志 |

---

## 二、功能模块划分

### 2.1 核心发布门槛

以下两项为 Shellium 的强制 P0 能力，任何 MVP 或正式版本均不得裁剪：

1. 当前服务器必须提供 SFTP 文件管理页面，支持路径直达、上传、下载、重命名和删除。
2. 终端内继续 SSH 到其他服务器时，应用必须维护外层到内层的服务器连接链；文件管理页面既可自动跟随最内层服务器，也可由用户自由选择任意一层服务器。

> **分屏已于 2026-08-07 移除。** 原第 1 项要求同一标签页内可水平/垂直分屏。实际使用中，
> 每个标签页一个终端会话已经够用：需要同时操作多台服务器时新建标签页即可，而分屏树
> （嵌套 QSplitter + 折叠冗余分隔器 + 活动面板跟踪）带来的复杂度并没有换来相应价值。
> 文件管理器因此直接绑定本标签页的终端，不再需要"跟随当前激活分屏"这一层间接。

第二项采用“Shell Integration 事件优先、SSH 命令解析降级、用户手动校正兜底”的识别策略。针对内层服务器的文件管理不会复用交互式终端字节流，而是通过外层连接链建立独立、可验证主机指纹的 SFTP 会话。

远程文件管理器必须位于终端标签页内部，并由该标签页独占实例。不同标签页不得共享当前
路径、服务器选择、传输请求、错误重试状态或会话密码。

### 模块总览

```
Shellium
├── 核心终端模块 (Terminal Core)
├── 连接管理模块 (Connection Manager)
├── SSH 协议模块 (SSH Engine)
├── 文件传输模块 (SFTP)
├── 安全凭据模块 (Vault / Keychain)
├── 高级网络模块 (Port Forwarding / Proxy)
├── 效率工具模块 (Snippets / History / Autocomplete)
├── UI / 主题模块 (Theme / Appearance)
├── 数据持久化模块 (Storage / Sync)
└── 插件系统 (Plugin System) [Phase 3]
```

---

## 三、分阶段功能规划

### Phase 1 — MVP（最小可用版本）

> 目标：能够管理主机列表、建立 SSH 连接、在终端中交互操作

#### 3.1 终端核心

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| 终端模拟器 | 基于 libvterm 的 VT100/xterm 终端模拟，支持 256 色 + TrueColor | P0 | ✅ `VtermBridge` |
| 键盘输入处理 | 普通键、功能键、修饰键、Alt/Meta 转义序列 | P0 | ✅ `TerminalView::keyPressEvent` |
| 鼠标输入处理 | 鼠标报告模式（SGR、utf-8 编码） | P1 | ❌ |
| 选中文本复制 | 鼠标选中文本，复制到剪贴板 | P0 | ⚠️ 走 QPlainTextEdit 文本选择，非终端感知的块选择 |
| 粘贴文本 | 从剪贴板粘贴到终端，支持安全粘贴（过滤控制字符） | P0 | ✅ 含密码提示处的本地捕获粘贴 |
| 终端滚动 | 滚动缓冲区，滚轮 / 快捷键滚动 | P0 | ⚠️ 有 10000 行滚动历史，但无终端级滚轮/快捷键处理 |
| 搜索终端输出 | 在终端输出中搜索文本 | P1 | ❌ |
| 字体渲染 | 等宽字体，支持连字（Nerd Font / Powerline 字符） | P0 | ⚠️ 写死系统等宽 13pt，不可配置 |
| 终端尺寸协商 | 窗口缩放时自动 SIGWINCH 通知远端 | P0 | ✅ |

#### 3.2 标签页

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| 多标签页 | 每个标签一个终端会话，可新建 / 关闭 / 重命名 | P0 | ✅ `MainWindow` |
| 标签页拖拽 | 拖出为独立窗口，拖回合并 | P1 | ⚠️ 可拖动排序，不能拖出为独立窗口 |
| 嵌套服务器链 | 自动识别终端内进入/退出的 SSH 服务器，维护外层到内层连接链 | P0 | ✅ 本项目最核心的能力，见 ENGINEERING_NOTES |

> 分屏（水平 / 垂直 / 比例调整 / 焦点切换）已于 2026-08-07 移除，见 2.1 节说明。

#### 3.3 连接管理

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| 主机配置 | 地址、端口、用户名、认证方式 | P0 | ⚠️ 数据层就绪（`ApplicationDatabase::saveHost`），**缺 UI** |
| 主机分组 | 树形分组结构，支持嵌套 | P0 | ⚠️ 数据层就绪（`HostNodeKind::Folder`），**缺 UI** |
| 分组继承 | 子组 / 主机继承父组的配置（端口、跳板机等） | P0 | ⚠️ 数据层就绪（`resolveHost`），**缺 UI**。注意 `allowLegacyHmacSha1` 刻意不继承 |
| 主机搜索 | 按名称、地址、标签快速搜索 | P0 | ⚠️ 数据层就绪（`searchHosts`/`hostsWithTag`），**缺 UI** |
| 标签系统 | 给主机打标签，按标签筛选 | P1 | ⚠️ 数据层就绪，**缺 UI**；标签存单 TEXT 列，不能含逗号 |
| 主机图标 | 自动识别 OS 类型显示图标，支持自定义 | P2 | ❌ |
| 导入 / 导出 | 支持导入 OpenSSH config、Termius 导出文件 | P1 | ❌ |
| 最近连接 | 记录最近连接的主机，快速重连 | P0 | ⚠️ 数据层就绪（`recentConnections`），**无人调用** |
| 连接历史 | 记录每次连接的时间、时长 | P1 | ⚠️ 数据层就绪（`appendConnectionHistory`），**无人调用** |

#### 3.4 SSH 引擎

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| SSH2 协议 | 支持现代加密算法（aes256-gcm、chacha20-poly1305） | P0 | ✅ libssh 默认策略 |
| 密码认证 | 用户名 + 密码 | P0 | ✅ |
| 公钥认证 | SSH 密钥对认证（RSA、Ed25519、ECDSA） | P0 | ✅ `ssh_userauth_publickey_auto`（本机密钥与 Agent） |
| 密钥生成 | 应用内生成 SSH 密钥对 | P1 | ❌ |
| 密钥导入 | 导入已有密钥文件（~/.ssh/） | P0 | ⚠️ 对话框可填路径，无管理界面 |
| 密钥口令 | 密钥 passphrase 管理，会话内缓存 | P0 | ⚠️ 与密码共用字段，仅进程内缓存 |
| known_hosts | 自动记录和管理 known_hosts，指纹校验 | P0 | ✅ 首次连接需人工核对 SHA-256 指纹 |
| 连接保活 | KeepAlive 心跳，防止连接超时断开 | P0 | ✅ 30 秒空闲逐层发 `ssh_send_ignore` |
| 重连机制 | 网络中断后自动重连 | P1 | ❌ 仅 SFTP 连接池有透明重连 |
| 跳板机 (Jump Host) | 通过跳板机连接目标主机 (ProxyJump) | P1 | ⚠️ 引擎支持 `proxyJumps`，**无 UI 可配置** |
| Agent 转发 | SSH Agent Forwarding | P1 | ❌ 现在只用本机 Agent 认证，不做转发 |
| 连接复用 | ControlMaster 多路复用 | P2 | ❌ |

#### 3.5 安全凭据

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| 本地保险库 | 主密码加密存储所有凭据（AES-256-GCM） | P0 | ⚠️ `Vault`（Argon2id + XChaCha20）与校验器已就绪，**无人调用** |
| 主密码 | 启动时解锁，会话内缓存密钥 | P0 | ❌ 无解锁流程与 UI |
| 系统钥匙串集成 | macOS Keychain / Windows Credential Manager | P1 | ❌ |
| 凭据引用 | 一个凭据可被多个主机引用，一处修改处处生效 | P0 | ⚠️ 表结构支持 `credentialId`，**无人调用** |
| 自动锁定 | 闲置超时自动锁定保险库 | P1 | ❌ |

#### 3.6 基础 UI

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| 主窗口布局 | 左侧主机树 + 右侧终端区域 | P0 | ❌ **没有主机树**；当前为工具栏 + 标签页 + 右侧文件管理 |
| 工具栏 | 快速操作按钮（新建连接、设置等） | P0 | ✅ |
| 状态栏 | 当前连接信息、编码、终端尺寸 | P0 | ✅ |
| 深色 / 浅色主题 | 跟随系统或手动切换 | P0 | ❌ 深色样式硬编码在控件里 |
| 快捷键体系 | 全局快捷键配置（可自定义） | P1 | ⚠️ 快捷键固定，不可自定义 |
| 命令面板 | Ctrl+Shift+P 快速搜索执行命令 | P1 | ❌ |

---

### Phase 2 — 完整功能版

> 目标：补齐 Termius 核心功能，达到日常可用水平

#### 3.7 SFTP 文件传输

> 路径浏览、上传、下载、重命名、删除和嵌套服务器选择属于 MVP 强制 P0 范围；断点续传、预览和队列增强在 Phase 2 完成。

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| SFTP 浏览器 | 双栏文件管理器（本地 / 远程） | P0 | ⚠️ **单栏（仅远程）**，本地侧靠 QFileDialog |
| 上传 / 下载 | 拖拽传输，进度条显示 | P0 | ⚠️ 有按钮与进度条，**无拖拽** |
| 路径直达 | 输入远程绝对路径直接进入目录 | P0 | ✅ 按服务器分别记忆路径，首次进主目录 |
| 重命名 / 删除 | 修改远程文件名，删除文件或目录并进行风险确认 | P0 | ✅ 删除确认含服务器名与推测层警示 |
| 服务器链切换 | 在当前终端的外层和各级内层 SSH 服务器间自由切换 | P0 | ✅ 内层经外层隧道抵达 |
| 目录递归传输 | 递归上传 / 下载整个目录 | P0 | ✅ 含取消、失败清单、符号链接跳过、`..` 逃逸防护 |
| 断点续传 | 大文件传输中断后可续传 | P1 | ❌ |
| 文件预览 | 文本 / 图片文件快速预览 | P1 | ❌ |
| 权限管理 | 修改文件权限（chmod）、属主 | P1 | ❌ 只展示不可改 |
| 队列管理 | 传输队列，暂停 / 恢复 / 取消 | P1 | ⚠️ 递归传输可取消，无队列 |
| 快捷传输 | 终端中右键 "上传到当前目录" | P2 | ❌ |

#### 3.8 端口转发

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| 本地端口转发 | -L local:remote 转发 | P0 | ❌ |
| 远程端口转发 | -R remote:local 反向转发 | P0 | ❌ |
| 动态端口转发 | -D SOCKS5 代理 | P0 | ❌ |
| 转发规则管理 | 保存转发规则，一键启停 | P0 | ❌ `port_forwards` 表已建，无代码 |
| 转发状态监控 | 实时显示转发连接数、流量 | P1 | ❌ |
| 随连接启动 | 连接主机时自动启动预设的转发规则 | P1 | ❌ |

#### 3.9 代理与高级连接

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| SOCKS5 代理 | 通过 SOCKS5 代理建立 SSH 连接 | P0 | ❌ |
| HTTP 代理 | 通过 HTTP CONNECT 代理 | P1 | ❌ |
| Telnet 协议 | Telnet 连接支持 | P2 | ❌ |
| Serial 串口连接 | 串口终端（波特率、数据位配置） | P2 | ❌ |
| 本地终端 | 直接打开本地 Shell | P0 | ✅ `LocalShellWorker`，真实 PTY |
| SSH 断开回退 | 已建立的远程会话结束后，当前终端自动退回本地登录式 zsh | P0 | ✅ |
| Mosh 协议 | Mosh 连接支持（需要远端安装 mosh-server） | P2 | ❌ |

#### 3.10 效率工具

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| Snippets 代码片段 | 保存常用命令，侧栏快速执行 | P0 | ❌ `snippets` 表已建，无代码 |
| Snippet 分组 | 按分组组织代码片段 | P0 | ❌ |
| 多主机执行 | 同一 Snippet 在多个主机上同时执行 | P1 | ❌ |
| 命令历史 | 按主机记录命令历史，跨会话持久化 | P0 | ⚠️ 数据层就绪（`appendCommandHistory`），**无人调用** |
| 自动补全 | 基于命令历史的智能补全 | P1 | ❌ |
| 启动 Snippet | 连接主机后自动执行预设命令 | P1 | ❌ |
| 广播模式 | 向所有打开的终端同时输入 | P1 | ❌ |

#### 3.11 终端增强

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| 主题系统 | 内置多套配色方案 + 自定义 | P0 | ❌ 深色样式硬编码 |
| 字体配置 | 字体族、字号、行高、字间距 | P0 | ❌ |
| 背景设置 | 纯色 / 图片 / 渐变背景，透明度调节 | P1 | ❌ |
| 光标样式 | 块状 / 下划线 / 竖线，闪烁开关 | P0 | ⚠️ 有光标渲染，不可配置 |
| 滚动缓冲区 | 可配置缓冲行数（默认 10000 行） | P0 | ⚠️ 常量 10000，不可配置 |
| 终端铃声 | 视觉铃声 / 声音铃声开关 | P1 | ⚠️ 直接 beep，无开关 |
| 超链接识别 | 自动识别 URL，点击打开 | P1 | ❌ |
| 导出终端输出 | 导出为文本文件 | P1 | ❌ |
| 会话日志 | 自动记录终端输出到日志文件 | P1 | ❌ |

#### 3.12 Workspace 工作区

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| 工作区概念 | 多个终端会话编组为一个工作区 | P1 | ❌ `workspaces` 表已建，无代码 |
| 布局保存 | 保存标签页组合，下次恢复 | P1 | ❌ |
| 广播输入 | 工作区内所有终端同时输入 | P1 | ❌ |
| 工作区模板 | 预设工作区模板，一键打开多个连接 | P2 | ❌ |

---

### Phase 3 — 高级特性

> 目标：差异化竞争力，团队协作能力
>
> **本阶段全部未实现（❌）**，且均非 P0。下面各表不再逐行标注状态。
> `plugin_state` 表已建但无代码。

#### 3.13 云同步与协作

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| 云同步 | 主机、密钥、Snippets 跨设备同步 | P1 |
| 端到端加密 | 同步数据端到端加密，服务端不可见 | P0 |
| 团队保险库 | 共享主机 / 凭据给团队成员 | P2 |
| 实时协作 | 多人同时查看 / 操作同一终端 | P2 |
| 会话录制 | 记录终端会话，回放查看 | P2 |
| SSO 登录 | SAML / OIDC 单点登录 | P2 |

#### 3.14 AI 增强

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| 自然语言转命令 | 描述意图，AI 生成 Shell 命令 | P1 |
| 命令解释 | 选中命令，AI 解释其作用 | P1 |
| 错误诊断 | 命令报错时，AI 分析原因并给出建议 | P1 |
| 智能补全 | AI 上下文感知的命令补全 | P2 |
| CLI Agent 集成 | 集成 Claude Code / Gemini CLI 等 | P2 |

#### 3.15 插件系统

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| 插件 API | 提供扩展接口（终端钩子、UI 扩展） | P2 |
| 脚本引擎 | 嵌入式脚本支持（Lua / JavaScript） | P2 |
| 插件市场 | 社区插件分发 | P3 |

#### 3.16 其他高级功能

| 功能 | 描述 | 优先级 | 状态与落点 |
|------|------|--------|------------|
| SSH 证书认证 | SSH CA 签发证书认证 | P2 |
| FIDO2 硬件密钥 | YubiKey 等硬件安全密钥支持 | P2 |
| 指纹 / 生物识别 | Touch ID / Windows Hello 解锁保险库 | P2 |
| 导入兼容 | 导入 Termius / PuTTY / MobaXterm 配置 | P1 |
| CLI 模式 | 命令行模式快速连接（shellium user@host） | P1 |
| 全局快捷键 | 系统级快捷键唤起窗口 / 快速连接 | P2 |
| 托盘图标 | 系统托盘，后台保持连接 | P1 |

---

## 四、架构设计

### 4.1 分层架构

```
┌─────────────────────────────────────────────┐
│              Presentation Layer              │
│   (Qt Widgets / QML Views, Theme Engine)     │
├─────────────────────────────────────────────┤
│              Application Layer               │
│  (Connection Controller, Session Manager,    │
│   Command Palette, Shortcut Manager)         │
├─────────────────────────────────────────────┤
│               Domain Layer                   │
│  (Host Model, Credential Model, Snippet      │
│   Model, PortForward Model, Workspace)       │
├─────────────────────────────────────────────┤
│           Infrastructure Layer               │
│  (libssh Wrapper, libvterm Wrapper,          │
│   SQLite Storage, Crypto Engine,             │
│   Sync Engine, Plugin Loader)                │
└─────────────────────────────────────────────┘
```

### 4.2 核心类设计

> 下表为**实际代码**，与最初草案已有明显出入。草案里的 `SshSession` / `SshChannel` /
> `ConnectionChain` 最终合并成了 `SshSessionStack` + `SshSessionNode`——因为嵌套层必须
> 由同一个工作线程统一泵送，拆成互相独立的类反而会引入死锁（见 ENGINEERING_NOTES）。

| 实际类 | 文件 | 职责 |
|--------|------|------|
| `SshSessionStack` | `src/ssh/SshSessionStack.h` | 一个终端的 SSH 连接层栈；工作线程，统一泵送所有层 |
| `SshSessionNode` | `src/ssh/SshSessionNode.h` | 一层：libssh 会话 + Shell 通道 + 转发/中继通道；非阻塞状态机 |
| `ShellShim` | `src/ssh/ShellShim.h` | 远端 Shell 集成脚本的生成与 OSC 事件解析 |
| `SshCommandParser` | `src/ssh/SshCommandParser.h` | 降级路径：从终端命令推测嵌套层 |
| `SshTunnelBroker` | `src/ssh/SshTunnelBroker.h` | 把 SFTP 请求路由到正确的会话层 |
| `TerminalPane` | `src/terminal/TerminalPane.h` | 一个标签页的终端；自持层与推测层的分界在此 |
| `TerminalView` | `src/terminal/TerminalView.h` | 输入编码、渲染、本地行捕获（密码提示） |
| `VtermBridge` | `src/terminal/VtermBridge.h` | libvterm ↔ Qt 桥接，含 OSC 回调 |
| `TerminalEchoFilter` | `src/terminal/TerminalEchoFilter.h` | 扣除本程序自己写给远端后被回显的字节 |
| `SftpService` | `src/sftp/SftpService.h` | SFTP 操作 + 按目标缓存的连接池 + 递归传输 |
| `RemoteFileManagerWidget` | `src/sftp/RemoteFileManagerWidget.h` | 标签页内的远程文件管理界面 |
| `ApplicationDatabase` | `src/storage/ApplicationDatabase.h` | SQLite 持久化读写层 |
| `HostRecord` | `src/core/HostRecord.h` | 主机/分组领域记录，含分组继承语义 |
| `Vault` | `src/security/Vault.h` | Argon2id + XChaCha20-Poly1305 凭据加密（**尚未接入**） |

**尚未存在**（规划中但无代码）：`HostModel`、`ConnectionManager`、`SshKeyManager`、
`SshConfigParser`、`PortForwarder`、`SnippetManager`、`CommandHistory`、
`AutocompleteEngine`、`TransferQueue`、`Settings`。

### 4.3 数据流

```
用户输入 → TerminalWidget → VtermBridge → SshChannel → libssh → 网络
                                                              ↓
用户查看 ← TerminalWidget ← VtermBridge ← SshChannel ← libssh ← 网络
```

### 4.4 项目目录结构（建议）

```
Shellium/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── app/                    # 应用入口与全局管理
│   │   ├── Application.cpp
│   │   ├── MainWindow.cpp
│   │   └── Settings.cpp
│   ├── core/                   # 核心领域模型
│   │   ├── host/               # 主机模型与管理
│   │   ├── credential/         # 凭据与保险库
│   │   ├── snippet/            # 代码片段
│   │   └── workspace/          # 工作区
│   ├── ssh/                    # SSH 引擎封装
│   │   ├── SshSession.cpp
│   │   ├── SshChannel.cpp
│   │   ├── SshKeyManager.cpp
│   │   └── PortForwarder.cpp
│   ├── terminal/               # 终端模拟
│   │   ├── TerminalWidget.cpp
│   │   ├── VtermBridge.cpp
│   │   └── TerminalSession.cpp
│   ├── sftp/                   # SFTP 文件传输
│   ├── ui/                     # UI 组件
│   │   ├── widgets/
│   │   ├── dialogs/
│   │   ├── theme/
│   │   └── icons/
│   ├── storage/                # 持久化
│   │   ├── Database.cpp
│   │   └── VaultStorage.cpp
│   └── utils/                  # 工具类
├── resources/                  # 资源文件
│   ├── themes/
│   ├── fonts/
│   ├── icons/
│   └── shaders/
├── tests/                      # 单元测试
├── third_party/                # 第三方依赖
└── docs/                       # 文档
```

---

## 五、第三方依赖

### 5.1 核心依赖

| 库 | 版本 | 用途 | 许可证 |
|----|------|------|--------|
| Qt | 6.8 LTS | GUI 框架 | LGPL / Commercial |
| libssh | 0.10.x | SSH 协议 | LGPL-2.1 |
| libvterm | 0.3+ | 终端模拟 | MIT |
| SQLite | 3.40+ | 本地数据库 | Public Domain |
| libsodium | 1.0.18+ | 加密 | ISC |
| spdlog | 1.12+ | 日志 | MIT |

### 5.2 构建与开发工具

| 工具 | 用途 |
|------|------|
| CMake 3.21+ | 构建系统 |
| Ninja | 构建后端 |
| clang-format | 代码格式化 |
| clang-tidy | 静态分析 |
| Catch2 / GTest | 单元测试 |
| vcpkg / Homebrew | 依赖管理 |

---

## 六、关键设计决策（均已落地）

| 决策 | 结论 | 说明 |
|------|------|------|
| 终端渲染 | libvterm + Qt 富文本文档 | 完全可控、无第三方 Qt 终端依赖。后续若要鼠标报告与块选区，需换成自绘 Widget |
| UI 技术路线 | 纯 QWidget | 与终端渲染一致，性能好 |
| 配置存储 | SQLite（当前 schema v2） | 结构化查询、事务安全；导入导出走 JSON（未实现） |
| 嵌套连接链 | 自持会话栈，而非解析终端输出 | 详见 ENGINEERING_NOTES；这是本项目最重要的架构选择 |
| 分屏 | 已移除 | 2026-08-07，理由见 2.1 节 |

## 七、开发里程碑

| 阶段 | 内容 | 交付物 | 状态 |
|------|------|--------|------|
| M1 | 项目骨架 + 构建系统 + libssh/libvterm 集成 | 能编译运行的空壳 | ✅ |
| M2 | 终端核心 + SSH 基础连接 | 能连上服务器并交互 | ✅ 并额外完成了嵌套连接链 |
| M3 | 连接管理 + 凭据保险库 + 主机分组 | 可管理主机列表 | ⚠️ 数据层完成，**UI 与保险库接入未做** |
| M4 | 标签页 + 主题系统 | 多会话管理 | ⚠️ 标签页完成，主题未做 |
| M5 | SFTP + 端口转发 | 文件传输与网络隧道 | ⚠️ SFTP 完成（含递归），端口转发未做 |
| M6 | Snippets + 命令历史 + 自动补全 | 效率工具 | ❌ |
| M7 | Workspace + 跳板机 + 代理 | 高级连接 | ❌（引擎支持 ProxyJump，缺 UI） |
| M8 | 云同步 + AI 增强 | 差异化能力 | ❌ |
| M9 | 插件系统 + 打包发布 | 正式发布 | ❌ |

---

## 八、下一步工作（接手指南）

按依赖关系排列。**动手前请先读 [ENGINEERING_NOTES.md](ENGINEERING_NOTES.md)**，
并遵守其中的测试规范（改完必须补测试，且必须把修复临时还原以确认该测试真的会失败）。

### 优先级 1 — 凭据保险库接入

**为什么最先做**：密码目前只活在进程内存的 `QHash` 里，关掉即丢。而所有前置件都已就绪，
这是投入产出比最高的一项。

已就绪：
- `Vault::unlock/encrypt/decrypt` + `createVerifier`/`verifyPassword`（`src/security/Vault.h`）
- `ApplicationDatabase::saveCredential/findCredential/...`，字段已能容纳密文与 KDF 参数

要做：
1. 首次启动时创建保险库：提示设置主密码 → `unlock` → `createVerifier` → 把盐与校验块存进 `settings`
2. 后续启动时解锁：读盐 → `unlock` → `verifyPassword` 判定密码是否正确。
   **空校验块必须走创建流程而不是解锁流程**，否则损坏数据会被当成新保险库覆盖掉已有凭据
3. `ConnectionDialog` 保存的密码改为经 `Vault::encrypt` 存入 `credentials` 表
4. 闲置自动锁定（P1）

### 优先级 2 — 主机管理 UI 与主机树

**依赖优先级 1**（主机要能引用凭据）。数据层已完整，纯粹是界面工作。

已就绪：`saveHost`/`findHost`/`removeHost`/`listHostsInGroup`/`searchHosts`/`resolveHost`。
分组继承语义已在 `HostRecord` 中定义（可继承字段用 `std::optional`；
`allowLegacyHmacSha1` 刻意不继承）。

要做：左侧主机树（`QAbstractItemModel` 或 `QTreeWidget`）、增删改分组与主机、搜索框、
双击连接、最近连接列表（`recentConnections` 已就绪但无人调用）。

### 优先级 3 — 端口转发

整块空白，`port_forwards` 表已建。注意本项目已有成熟的非阻塞通道机制可复用
（`SshSessionNode::beginForward` / `pumpForwards`），**不要另起炉灶**。

### 优先级 4 — 终端增强与主题

主题、字体、光标、滚动行数目前全是硬编码常量。做之前需要先有设置持久化
（`ApplicationDatabase` 的 `setSetting`/`setting` 已就绪）。

### 已知技术债（可随时穿插处理）

- 保活发送失败后每 10ms 重试一次，无退避；建议加 1 秒最小间隔
- `tags` / `proxy_jumps` 存单个 TEXT 列逗号分隔：标签不能含逗号、无法建索引。规模上来要拆关联表
- `resolveHost` 逐层查父节点，深树上是 N+1（已有环检测）
- 递归传输取消只在 64 KiB 分块边界生效；单次 `sftp_read` 网络卡死时要等它返回
- 递归传输独占连接锁期间，同服务器的目录浏览会挂起
- 终端选中复制走的是 QPlainTextEdit 文本选择，不是终端感知的块选择

---

## 九、待确认事项

1. **项目名称**：Shellium 是否作为正式名称？
2. **云同步策略**：是否需要自建后端？还是仅支持本地？
3. **许可证**：开源（GPL/MIT）还是闭源商业？
4. **国际化**：初期是否需要多语言支持？（当前界面文案全部硬编码中文，未用 tr() 之外的机制）
5. **自动更新**：是否需要内置自动更新机制？
6. **分组继承范围**：`allowLegacyHmacSha1` 目前刻意不从父组继承——降低 MAC 安全基线被认为
   必须是针对单台服务器的知情决定。若产品意图是全字段继承，此处需要推翻。

### 已确认的决策（不再讨论）

- **UI 技术路线**：纯 QWidget（已落地）
- **终端渲染**：libvterm + Qt 富文本文档（已落地）
- **配置存储**：SQLite（已落地，schema v2）
- **分屏**：已于 2026-08-07 移除，见 2.1 节

---

*文档版本: v0.3 | 更新日期: 2026-08-08 | 状态标注由代码核对得出*

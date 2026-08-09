# Shellium — 工程约束与已知陷阱

> **接手本项目前请完整读一遍。**
>
> 下面每一条都是踩过坑之后才总结出来的。它们的共同特点是：**违反后不会立刻报错**，
> 而是表现为难以定位的偶发故障——静默吞掉用户输入、只在第二层以上复现的死锁、
> 看起来完全无关的功能突然失效。重犯的排查代价远高于遵守的代价。

---

## 一、最重要的架构决策：自持会话栈

嵌套 SSH（用户在终端里继续 `ssh` 到另一台服务器）**不是**通过解析终端输出来识别的，
而是由本程序亲自建立每一层会话：

```
Shellium ──libssh S1─────────────────────> 第 1 层
         ──S1 的通道──> libssh S2────────> 第 2 层
         ──S2 的通道──> libssh S3────────> 第 3 层

终端 PTY  ← 绑定栈顶层
SFTP      ← 绑定用户选中的那一层
```

**为什么这样做**：观察式识别（读键盘缓冲、匹配屏幕正则）永远存在观察不到的状态。
早期版本因此产生过大量"幽灵层"——失败的 `ssh` 留下一个用户从未进入过的层，而它常与
真实层同名，界面上分辨不出来，用户会在错误的服务器上删文件。自持之后，层的进入与
退出都是确定事件（通道 EOF），这类漂移从根上消失。

### 两类层必须严格隔离

| | 来源 | `confidence` | 出栈依据 |
|---|---|---|---|
| **自持层** `m_ownedTargets` | `SshSessionStack` 亲自建立 | `Confirmed` | Shell 通道 EOF，**确定事件** |
| **推测层** `m_inferredTargets` | 用户绕过接管、手打 `ssh` 被解析 | `Inferred` | 命令解析，仍是猜测 |

规则：
- `layersChanged` 到达时**整体替换**自持层并**清空**推测层——底座变了，叠在上面的推测就没了依据
- 解析器的 `LeaveServer` **只弹推测层**，绝不能碰自持层
- 密码嗅探只作用于推测层：自持层由程序自己认证，永远不需要偷听终端输入
- 任何展示与高危操作都要读 `confidence`：下拉框标 `⚠ 推测`，删除确认框必须写明服务器

### `ConnectionTarget::confidence` 默认取 `Inferred`

新代码路径若忘记标注来源，应当落到需要用户确认的保守分支，而不是被当作已验证事实。

---

## 二、非阻塞约束（违反会死锁，且只在第 2 层以上复现）

第 N+1 层的传输**架在第 N 层的通道之上**，而该通道的数据搬运由同一个工作线程完成。
因此建立第 N+1 层时的任何阻塞等待都会饿死第 N 层 → 死锁。

这不是性能问题，是**正确性**问题。本地单层测试完全看不出来。

已改造的三处，新增代码必须遵守同样模式：

1. `SshSessionNode::advanceEstablish()` —— 握手/认证/开通道全是状态机，收到 `SSH_AGAIN` 立即返回
2. `SshSessionNode::beginForward()` / `pumpForwards()` —— 通道建立跨轮次推进，不在原地自旋
3. 主机指纹确认用 `HostKeyDecision` 枚举轮询，**不用 `QWaitCondition` 阻塞**

配套注意：
- `libssh` 在非阻塞模式下要求 `SSH_AUTH_AGAIN` 之后用**完全相同的参数**重试，
  所以密码与 PTY 模式字节串都缓存成成员（`m_secret` / `m_terminalModes`），不能每轮重新构造
- 非阻塞下 `SSH_OPTIONS_TIMEOUT` 不再保护握手，必须自带计时器
- `openForward` 的 `nonBlockingCaller` 参数两条路径需求**相反**：嵌套层的 fd 必须非阻塞，
  SFTP 的同步会话必须阻塞

---

## 三、libssh：只 `free`，绝不先 `close`

**非阻塞会话上 `ssh_channel_close()` 返回 `SSH_AGAIN` 而不完成**，留下一个半写的
CHANNEL_CLOSE 包，之后整条 SSH 流失步——**同一会话上其他完全无关的 channel
（比如交互式 Shell）会一起静默失效**。

对一个 `ssh_channel_open_forward` 失败、从未真正打开的 channel 调用 close 更糟：
远端根本没有对应的 channel id。

`ssh_channel_free()` 自身会在需要时关闭，并正确处理"未打开"与"非阻塞待关闭"两种情况。

当前 `SshSessionNode` 的交互 Shell、后台 exec、待建立转发和活动转发均已统一为 **free-only**
释放路径；`requestPopLayer()` 也会立即 free 并置空栈顶 Shell，不再发送 EOF 后调用 close。
后续新增通道时必须继续执行这一约束，不能因为“正常退出更礼貌”而重新引入显式 close。

> 症状极具迷惑性：转发失败之后终端"莫名其妙"不再响应，看起来像终端的问题而非转发的问题。

---

## 四、终端与远端交互

### 4.1 长内容绝不经交互式终端下发

Shell 集成脚本（约 700 字符）经**独立 exec 通道**写入远端临时文件，
交互式终端里只出现一条约 21 字符的 ` . /tmp/.shm-xxxxxxxx`，其回显由
`TerminalEchoFilter` 精确扣掉；脚本末尾自删。

**为什么**：凡是写给远端的字节都会被 PTY 回显回来。短行 readline 逐字回显，可按字节
精确扣除；**超宽行 readline 会按自己的方式重绘**，回显不再与发出的字节一一对应，
无论怎么匹配都扣不干净。

曾试过"抢在远端 Shell 接管之前发送"（那时 tty 还是规范模式，内核逐字回显）——
**这个竞争赢不了**：sshd 请求 shell 后几乎立刻打印 motd 并交给 bash，任何一次网络往返
都晚于它。第 1 层偶尔侥幸成功、第 2 层必然失败，正是这个竞争的表现。

新增任何需要下发到远端的内容，先问它是否必须经过终端。只有必须在交互式 Shell **进程内**
生效的东西（函数定义、变量）才需要经终端，并且要压到一行以内。

### 4.2 远端关回显的两条路都已否决

- `read -s` 并非 POSIX，不支持时返回非零，用 `||` 回退会**吃掉用户的下一行输入**
- `stty -echo` 多数实现走 `TCSAFLUSH`，会**丢弃已在缓冲区的输入**，回执若先于 stty
  抵达就被吞掉，shim 永久阻塞

宁可留一行噪音，也不能冒丢输入或死锁的风险。回显抑制一律在**本地**做。

### 4.3 OSC 载荷禁用 C0 控制字符

libvterm 的字符串解析器遇到 C0 控制字符会**中止**当前 OSC，把已累积的部分当作完整载荷
交出来。所以 OSC 6773 的 argv 用 `0x1E` 连接后**再 base64**。

> 阴险之处：单参数用例会**碰巧通过**（截断点恰好在唯一参数之后）。必须用多参数用例验证。

远端没有 `base64` 时 shim 直接执行原生 ssh，**绝不发出无人应答的 OSC**——那会让 shim
永远阻塞在 `read` 上，把用户终端挂死。

### 4.4 libvterm 双宽字符的右半格

CJK、全角标点、部分表情占两格，libvterm 把**右半格**标记为 `chars[0] == 0xFFFFFFFF`
而不是 0。只判 `!= 0` 会把这个哨兵当成有效码位，`QString::fromUcs4` 转成 U+FFFD，
于是每个汉字后面跟一个 `�`，列计数也整体错位。`cell.width` 对右半格仍是 1，靠 width 拦不住。

处理在 `VtermBridge::convertCell`：命中哨兵就把 `width` 置 0 并返回。
任何新增的终端渲染路径都要复用 `convertCell`，不要另写一份码位转换。

---

## 五、输入处理

### 5.1 拦截键盘的分支必须显式处理粘贴

macOS 上 Qt 把 Command 映射为 `Qt::ControlModifier`，所以 `Cmd+V` 携带 ControlModifier。
"追加普通字符"的分支通常写成 `!text.isEmpty() && !Control && !Meta`，粘贴因此不匹配
任何分支被**静默吞掉**。

> 症状极具误导性：**手打密码一切正常，粘贴则永远连不上**，日志里只看到"提交了空凭据"。
> 这个 bug 曾导致长时间的错误排查方向。

同理，`QTest` 也不产生**自动重复**事件（`event->isAutoRepeat()`）。真实键盘上按住回车
稍久，重复的回车会落在密码捕获刚开启的瞬间，提交空行。

### 5.2 空行提交与主动取消必须区分

在密码提示处直接回车，OpenSSH 的行为是**重新提示**而不是中止。把两者混为一谈会让
任何一次输入丢失都升级成不可恢复的连接失败。只有 `Ctrl+C` 才算放弃。

### 5.3 接管认证必须在终端内联索要密码

用户刚敲完 `ssh`，预期的是终端里的 `password:` 提示。弹对话框会彻底打断这个心智模型。
实现在 `TerminalView::beginLocalLineCapture()`：按键不发往远端、不回显——因为那一层的
SSH 会话尚未认证完成，远端还不存在。

拿到的密码要**写回目标**并随快照交给 SFTP，否则文件管理器会再弹一次密码框。

### 5.4 层未就绪前绝不发 `layersChanged`

曾经在隧道一通就 `push_back` 并立刻发信号，而此时该层只完成了选项配置。文件管理器因此
对未认证的层发起 SFTP，弹出本不该出现的密码框。层就绪的通知统一由
`advanceEstablishingLayers` 在 `step.ok` 时发出。

---

## 六、SFTP

- 连接**按目标池化**（`stableKey()` 已含跳板链，不同环境的同名内网主机不会串用）。
  同一连接上的操作由 mutex 串行化，不同目标并行
- **libssh 的 SFTP API 是同步的**，与非阻塞会话不兼容；且即便兼容，也不能跑在终端的
  泵送线程上——一次大文件传输会冻住终端。所以 SFTP 走独立线程 + 独立连接，
  **不复用会话栈里的 libssh 会话**
- 第 2 层起的 SFTP 必须经**外层隧道**抵达。缺少 `tunnelRouteId` 时会退化成从本机直连内网
  地址——而本机很可能就在同一私有网段上，于是连到局域网里另一台碰巧同 IP 的机器，
  **在错误的服务器上列目录甚至删文件**
- 要抵达第 N 层，转发必须由第 **N−1** 层发起（`SshSessionStack::originLayerFor`）

---

## 七、服务器策略降级

`AllowTcpForwarding no` 只禁用 `direct-tcpip` 通道类型，**不影响 session（shell/exec）通道**。
收到 `SSH_OPEN_ADMINISTRATIVELY_PROHIBITED`（错误文本含 `error (1)`）时自动改用
**exec 中继**：在远端跑一条中继命令，用它的标准输入输出当字节管道
（`bash /dev/tcp` → `nc` → `socat` 依次尝试）。

两个实现细节：
- 用 `{ ...; } 3<>/dev/tcp/...` 复合重定向而**不是** `exec 3<>...`——后者在非交互 shell 里
  重定向失败会直接终止整个 shell，回退链就没机会执行
- `trap "kill $p" EXIT` 保证通道关闭时后台的 `cat` 一并退出，不在服务器上留下游荡进程

接管失败时**必须把用户原本的 ssh 命令交还给远端执行**，否则用户敲下的命令凭空消失，
重试多少次都一样（每次都走同一条失败路径）。

---

## 八、测试规范（强制）

**新增功能或修复 BUG，修好之后必须补一条自动化测试用例，随代码一起提交。**

### 测试目标

每个测试类**一个独立可执行文件**（Qt 惯用做法）。共用 `main` 会让按名过滤测试时
另一套报 "Unknown test function" 并返回非零，破坏单独复现某个用例的工作流。

| 目标 | 内容 | 依赖 |
|------|------|------|
| `ShelliumCoreTests` | 命令解析、终端视图、设置、Vault | GUI + Sql + libssh + libvterm + libsodium |
| `ShelliumKeepAliveTests` | 保活判定纯逻辑 | Core + libssh |
| `ShelliumStorageTests` | 持久化读写 | Core + Sql |
| `ShelliumSftpRecursiveTests` | 递归传输纯逻辑 | Core |
| `ShelliumConnectionToolsTests` | 配置导入导出、SSH 密钥、SOCKS5 协议 | Core + Network + Sql + libssh |
| `ShelliumProductivityTests` | Snippet、历史、补全、多目标与广播接线 | GUI + Sql + SSH/终端依赖 |
| `ShelliumSettingsUiTests` | 设置界面自绘控件接线（步进器、开关、配色色板、分类导航） | GUI + Sql |
| `ShelliumSshIntegrationTests` | **真实服务器**连接路径 | 全部；缺 `SHELLIUM_IT_*` 时自动跳过 |

### 必须验证测试真的抓得住

把修复**临时改回有缺陷的版本**，确认用例失败，再恢复。不做这一步不能声称"已覆盖"。

> 曾有一条用例，去掉修复后照样通过——说明它根本没抓住目标，等于白写。
> 也曾有一条只测"payload 为空"，而 Qt 把空 `QByteArray` 绑成 NULL 被 `NOT NULL` 约束
> 顺手挡下，测试通过得毫无意义。

### 断言可观察行为，不要只断言字段

`tunnelRouteId` 非空不代表隧道通；必须真的走一次 SFTP 列目录、真的读到对端 SSH 横幅、
真的看到命令回显。

### 注意 QTest 覆盖不到的输入路径

`QTest::keyClicks` 只模拟逐字键入，**测不到粘贴**（用 `QTest::keySequence` + 剪贴板），
也**不产生自动重复事件**。这两条都真实漏过过 bug。

### 集成测试对真实服务器的影响

集成套件里有几条**故意制造认证失败**的用例。反复运行曾触发目标服务器的
`MaxStartups` 限流或 fail2ban 封禁，症状是 TCP 连得上但握手前即断开
（`kex_exchange_identification: Connection closed by remote host`）。
针对生产服务器运行时请注意频率。

---

## 九、凭据处理

- 测试凭据存放在项目根的 `AGENT.md`，**已被 `.gitignore` 排除**，不得提交
- 诊断日志只记录长度，**绝不记录字符内容**（见 `shellium.auth` 日志分类，
  用 `QT_LOGGING_RULES="shellium.auth.debug=true"` 打开）
- `Vault` 的空校验块**必须**判定为失败：那代表保险库尚未初始化，调用方应走创建流程。
  把两者混为一谈会让损坏数据被静默当成新保险库，**覆盖掉用户已有的凭据**
- 数据库里**只存密文**；`saveCredential` 的明文防护是启发式的（只能验证"有密文 + 有算法
  标识"），真正的保证要靠调用路径收口

---

## 十、Shell 集成对远端环境的影响

注入的 shim 会：

| 改动 | 范围 |
|---|---|
| `/tmp/.shm-xxxxxxxx` 约 700 字节临时文件 | 脚本末尾 `rm -f` 自删 |
| 定义 `ssh()` 函数 | 仅当前交互式 shell 进程，内存中 |
| 给 Bash 的 `HISTCONTROL` 追加 `ignorespace` | 保留原有 ignoredups/erasedups；已有 ignoreboth 时不改动 |

不写入任何 rc 文件，断开即消失。函数只在交互式 shell 生效（`case "$-" in *i*)` 护栏），
`command ssh` 可绕过。

安装命令会在独立 exec 通道检查登录 Shell，仅 bash/zsh 才写入权限为 0600 的临时脚本；
后台 exec 必须明确返回退出码 0 才会向交互终端发送 source。失败原因（通道错误、退出码与
受限长度的 stderr）通过 `shellIntegrationFailed` 上报，SSH 会话本身继续可用并降级到命令
解析路径。原先从未被读取的 `m_shellIntegrationEnabled` 已删除，避免出现“界面以为关闭、
工作线程仍继续注入”的伪开关。

exec 中继在 `ssh_channel_request_exec()` 成功后还会保留一个短暂确认窗口：远端缺少
`/dev/tcp`、`nc`、`socat` 或命令立即退出时，错误会作为转发失败返回，而不是先交付一个
随后立刻 EOF 的伪成功 socket；交付后的异步 stderr 则写入 `shellium.ssh` 日志分类。

---

## 十一、端口转发的 fd 所有权

本地转发（-L）、远程转发（-R）和动态转发（-D）均绑定一条已经认证的**根 SSH 会话**。
规则运行时不允许直接从自己的网络线程调用 libssh，所有通道操作都必须排队回
`SshSessionStack` 所属工作线程。

远程转发尤其要遵守“一个 fd 只有一个所有者”：

1. `SshSessionNode` 接受 `forwarded-tcpip` 后只向上交付一次 fd；
2. `PortForwardManagerWidget::dispatchRemoteConnection` 按准确的 sessionStack 与 ruleId
   唯一路由；
3. 命中运行时后由 `PortForwardRuntime::acceptRemoteConnection` 接管；
4. 规则已停止、标签已切换或找不到运行时时，管理器必须立即关闭孤儿 fd。

不能让每个规则分别订阅同一个“新连接”信号。那会形成多个接收者同时认为自己拥有 fd 的
双重关闭/重复读写竞态，通常只在快速停启规则或同一主机打开多个标签时出现。

终端断开时必须先 `requestStop()`，再发送会话即将关闭信号，让所有运行时停止后才释放
`SshSessionStack`。断开清理期间 `TerminalPane::forwardingSessionStack()` 仍应返回尚存活的
原对象，不能仅因 `m_shellReady` 已变为 false 就返回空指针，否则管理器无法精确注销。

---

## 十二、自动重连不是无限重试

自动重连只针对“曾经成功建立、随后没有远端正常退出码”的异常断线。以下情况不能自动重试：

- 用户主动关闭、切换或正常执行 exit；
- 首次主机指纹、认证或算法协商失败；
- 服务端给出正常退出状态；
- 用户把最大重试次数设为 0。

每轮异常断线有独立 token，默认最多 3 次；延迟按首轮秒数指数增长并封顶 60 秒。新连接成功、
用户手动发起连接或标签销毁都会使旧 token 失效，排队中的 QTimer 回调必须再次核对 token，
不能让上一台主机的延迟任务覆盖用户刚打开的新会话。

每次自动尝试都应写入保存主机的连接历史；临时连接没有持久 hostId，不得制造无法归属的
孤立历史行。重试耗尽后回到本地 Shell，使标签保持可操作。

---

## 十三、Qt 空字符串与 SQLite NOT NULL

Qt 的默认 `QString` 可处于“长度为 0 且 `isNull()` 为 true”的状态。把这种值直接交给
`QSqlQuery::bindValue` 会绑定成 SQL NULL，而不是空 TEXT。设置表的 value 是 NOT NULL，
而空字体族和用户主动取消的快捷键又确实需要保存为空文本。

因此 `ApplicationDatabase::setSetting` 必须在数据库边界把 null QString 规范化为非 null
空串。不要要求每个调用方分别修补，也不要放宽表约束；后者会让“缺失值”和“业务空值”
再次混在一起。

---

## 十四、工作区恢复必须先验证、后破坏

打开保存工作区会关闭当前全部连接，是不可轻易回滚的 UI 操作。恢复流程必须先完成：

1. JSON 语法、版本、标签数量和字段类型校验；
2. 所有非空 hostId 的存在性与 Host 类型校验；
3. 构造完整的待恢复快照；
4. 向用户确认替换当前标签。

只有全部成功后才能移除当前标签并新建工作区。逐个“关一个、解析一个”会让中途损坏的
工作区把用户现有会话破坏到一半。临时连接也不能被静默保存成本地 Shell：没有持久主机记录，
下次无法可靠恢复地址、跳板链与凭据引用，应在保存阶段整体拒绝并提示先保存主机。

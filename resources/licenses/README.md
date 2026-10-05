# 第三方开源软件许可证

本目录的 `*.txt` 由 `shellium_add_license_notices()` 编译进 Qt 资源 `:/licenses/`，在
设置 →「关于」→「开源软件」→「查看许可证」对话框中展示。内置字体的许可证仍放在
`resources/fonts/licenses/`，映射为 `:/licenses/fonts/`。组件清单（名称、SPDX 标识、主页、
署名语句、对应文件）维护在 `src/ui/OpenSourceLicensesDialog.cpp` 的
`OpenSourceNotices::components()`。

## 为什么必须随程序附带

安装包（macOS DMG 的 `Contents/Frameworks`、Windows ZIP/NSIS）直接分发了下列第三方二进制，
它们的许可证都要求随二进制附带版权声明和许可证原文：

- **LGPL**：Qt（LGPL-3.0，需同时附 GPL-3.0 全文）、libssh、GLib、GNU gettext 的 libintl
  （LGPL-2.1+）。全部以动态库形式链接，用户可以替换；对话框顶部的声明给出了源代码获取方式。
  **不得改为静态链接**，否则需要额外提供可重新链接的目标文件。
- **署名条款**：FreeType（FTL 第 3 节）和 libjpeg-turbo（IJG）要求在文档中出现固定的署名
  语句，随各自组件的详情页展示（`attribution` 字段），保持英文原文。
- **BSD / MIT / ISC / Zlib / Apache-2.0 / Unicode / OFL** 等：要求保留版权声明与许可证文本。
- D-Bus 选用 AFL-2.1，Graphite 选用 MIT（两者都是多许可证任选其一）。

## 文本来源

| 文件 | 来源 |
| --- | --- |
| `LGPL-3.0.txt` / `GPL-3.0.txt` / `LGPL-2.1.txt` | gnu.org 官方全文（LGPL-2.1 取自 GLib 发行包） |
| `libssh-COPYING.txt` | libssh 0.12 发行包（含 OpenSSL 链接例外） |
| `FTL.txt` | FreeType 仓库 `docs/FTL.TXT` |
| `AFL-2.1.txt` | D-Bus 仓库 `LICENSES/AFL-2.1.txt` |
| `libjpeg-turbo-README.ijg.txt` | libjpeg-turbo 仓库 `README.ijg`（IJG 许可证条款所在） |
| `winpthreads-COPYING.txt` | mingw-w64 仓库 `mingw-w64-libraries/winpthreads/COPYING` |
| `GCC-RUNTIME-EXCEPTION.txt` | GCC 发行包 `COPYING.RUNTIME` |
| `OFL-1.1.txt` / `Apache-2.0.txt` | SPDX license-list-data |
| 其余 `<组件>-LICENSE*.txt` | 对应 Homebrew 发行包中的上游 LICENSE/COPYING 原文 |

## 新增或替换依赖时

1. 用 `otool -L`（macOS）或 `scripts/verify_windows_dependencies.sh`（Windows）确认安装包里
   新增了哪些动态库，包括被依赖间接带入的库。
2. 把上游 LICENSE/COPYING 原文放进本目录（必须是 `.txt`，CMake 按通配符收集）。
3. 在 `OpenSourceNotices::components()` 里加一条；LGPL 组件设 `lgpl = true`。
4. 跑 `ShelliumSettingsUiTests`：`openSourceNoticesCoverEveryDistributedComponent` 会检查每条
   都能读到许可证、直接依赖与内置字体没有漏项、资源里没有无人引用的文件。

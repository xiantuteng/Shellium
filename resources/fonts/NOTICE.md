# 内置终端字体

这些 TTF 会被 `qt_add_resources` 编译进可执行文件，由 `BundledFonts::registerAll()` 在启动时
注册进 `QFontDatabase`。设置界面的“内置字体”分组只列这几款，因此无论用户机器上装没装字体，
终端外观都完全一致。

| 字体族 | 文件 | 上游 | 许可证 |
| --- | --- | --- | --- |
| Source Code Pro | SourceCodePro-Regular.ttf | adobe-fonts/source-code-pro | SIL OFL 1.1 |
| Source Code Pro Medium | SourceCodeProMedium-Regular.ttf | 同上（Medium 字重，已改族名） | SIL OFL 1.1 |
| Fira Mono | FiraMono-Regular.ttf | mozilla/Fira | SIL OFL 1.1 |
| Fira Mono Medium | FiraMonoMedium-Regular.ttf | 同上（Medium 字重，已改族名） | SIL OFL 1.1 |
| Inconsolata-g | Inconsolata-g.ttf | chrissimpkins/codeface | SIL OFL 1.1 |
| Anonymous Pro | AnonymousPro-Regular.ttf | google/fonts | SIL OFL 1.1 |
| Ubuntu Mono | UbuntuMono-Regular.ttf | google/fonts | Ubuntu Font Licence 1.0 |
| Droid Sans Mono | DroidSansMono.ttf | chrissimpkins/codeface | Apache License 2.0 |
| DejaVu Sans Mono | DejaVuSansMono.ttf | dejavu-fonts 2.37 | Bitstream Vera / 公有领域 |
| PT Mono | PTMono-Regular.ttf | google/fonts（上游文件名 PTM55FT.ttf） | SIL OFL 1.1 |
| Cascadia Code | CascadiaCode-Regular.ttf | microsoft/cascadia-code v2407.24（ttf/static） | SIL OFL 1.1（保留字体名） |
| Fira Code | FiraCode-Regular.ttf | tonsky/FiraCode 6.2（ttf，非可变字体） | SIL OFL 1.1 |
| JetBrains Mono | JetBrainsMono-Regular.ttf | JetBrains/JetBrainsMono v2.304 | SIL OFL 1.1 |
| Meslo LG S | MesloLGS-Regular.ttf | andreberg/Meslo-Font v1.2.1 | Apache License 2.0 |

许可证原文见 `licenses/`。

Cascadia Code 的 OFL 带保留字体名（RFN），只能原样分发，不能像下面两个 Medium 那样改族名后
再发布。Fira Code、JetBrains Mono 取的都是静态 Regular 而非
可变字体（VF）：可变字体在 Qt 里只会暴露默认实例，界面上多出来的字重根本选不到，白白多占
体积。Meslo 上游同时提供 LG L/M/S 三档行距，这里只内置行距最紧的 S，它的族名是 "Meslo LG S"。

## 为什么两个 Medium 要改族名

上游的 `SourceCodePro-Medium.ttf` 与 `FiraMono-Medium.ttf` 在 name 表里仍然声明自己属于
"Source Code Pro" / "Fira Mono" 族，只是 subfamily 为 Medium。Qt 因此把它们并进基础族，
`QFontDatabase::applicationFontFamilies()` 返回的还是基础族名，设置界面里就永远只会出现一
项，选不到中等字重。而本项目的字体设置只持久化一个族名字符串，没有单独的字重字段。

因此这两个文件用 `scripts/rename_font_family.py` 把 name ID 1/2/4/6 改写成独立族名，并删掉
排版族名（ID 16/17，否则 Qt 会按它重新并回原族）和失效的 DSIG 签名。两款字体均为 SIL OFL
且没有保留字体名（RFN），改名后分发是被许可的。重新制作命令见该脚本头部注释。

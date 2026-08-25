# 发行版图标来源说明

本目录下的 SVG 文件取自 [Simple Icons](https://simpleicons.org/) 项目
（<https://github.com/simple-icons/simple-icons>），该项目以 CC0-1.0
协议发布其收录的单色矢量图标重绘版本。文件按品牌名小写命名，与
`HostDistributionIcon.h` 中 `Family` 枚举一一对应。

各发行版名称、标志与品牌配色仍是对应项目/公司的商标；本程序仅将其作为
"识别远端系统类型"的图形标识使用（与 neofetch、Ventoy 等工具的用法一致），
不代表任何形式的官方认可或关联。

图标运行时由 `HostDistributionIcon::iconFor()` 加载，统一转换为白色描边
并叠加在按品牌色绘制的圆底上——原始文件本身不含颜色信息，颜色由代码在
渲染时按 `HostDistributionIcon.h` 中维护的品牌色表指定。

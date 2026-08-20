#pragma once

#include <QStringList>

/**
 * @brief 随可执行文件一起分发的终端等宽字体。
 *
 * 终端外观必须在任何一台机器上都一致：只要依赖系统安装的字体，用户换台电脑就可能因为缺字体
 * 被静默替换成别的字形，行高与字宽随之改变，终端网格对不齐。因此这几款字体直接编译进 Qt
 * 资源，启动时注册进 QFontDatabase，设置界面单独成组展示。
 */
namespace BundledFonts {

/**
 * @brief 返回内置字体的族名列表，顺序即设置界面“内置字体”分组的展示顺序。
 *
 * 这里返回的是注册后 QFontDatabase 实际会给出的族名，不是文件名；界面与持久化都只认族名。
 */
[[nodiscard]] QStringList familyNames();

/**
 * @brief 把全部内置字体注册进 QFontDatabase。
 * @return 实际注册成功的族名；顺序与 familyNames() 一致。
 *
 * 幂等：重复调用不会重复注册，也不会重复报错，因此测试可以在每个用例开头安全调用。必须在
 * 构造 QGuiApplication 之后调用——Qt 的字体数据库依赖已初始化的平台插件。
 */
QStringList registerAll();

} // namespace BundledFonts

#pragma once

#include <QtGlobal>

class QWidget;

/**
 * @brief 将 Qt 顶层窗口接入 macOS 的全尺寸透明标题栏。
 * @param window 已创建原生窗口句柄的 Qt 顶层控件；传入 nullptr 会安全失败。
 * @return macOS 下成功取得并配置 NSWindow 时返回 true；原生句柄尚不可用时返回 false，
 *         调用方可在后续 showEvent 再次尝试。非 macOS 平台无需配置并始终返回 true。
 *
 * Qt 的 setUnifiedTitleAndToolBarOnMac() 只会统一原生工具栏纹理，无法让普通 QWidget 真正
 * 进入交通灯所在标题区。本函数仅调整 NSWindow 的展示掩码，保留系统关闭、最小化、缩放
 * 按钮及其标准行为，不接触会话数据，也不替换系统级窗口安全边界。
 */
#ifdef Q_OS_MACOS
[[nodiscard]] bool enableMacWindowChrome(QWidget *window);
#else
[[nodiscard]] inline bool enableMacWindowChrome(QWidget *window)
{
    Q_UNUSED(window)
    return true;
}
#endif

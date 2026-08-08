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

/**
 * @brief 使用当前 macOS 左键事件开始拖动原生窗口。
 * @param window 需要移动的 Qt 顶层窗口；传入子控件或 nullptr 会安全失败。
 * @return 成功取得 NSWindow 与当前左键事件并交给 AppKit 后返回 true，否则返回 false。
 *
 * QWindow::startSystemMove() 在全尺寸客户区中并不总能还原原生标题栏拖动，尤其当事件最初
 * 由 QTabBar 等子控件接收时。这里直接调用 NSWindow 的 performWindowDragWithEvent，使多屏、
 * Spaces、贴边与窗口吸附继续遵循系统行为，而不是自行计算窗口坐标。
 */
[[nodiscard]] bool beginMacWindowDrag(QWidget *window);

/**
 * @brief 切换 macOS 原生窗口的标准缩放状态。
 * @param window 需要缩放或恢复的 Qt 顶层窗口；传入无效对象会安全失败。
 * @return 已将操作交给 NSWindow 时返回 true；原生窗口尚不可用时返回 false。
 *
 * 使用 NSWindow::zoom: 而不是手工设置几何尺寸，能够保留 macOS 的可用屏幕边界、动画以及
 * 再次双击恢复旧尺寸的标准语义。
 */
[[nodiscard]] bool toggleMacWindowZoom(QWidget *window);

/**
 * @brief 将 macOS 红黄绿窗口按钮垂直对齐到自定义标题栏中心。
 * @param window 已启用全尺寸客户区的 Qt 顶层窗口。
 * @param titleBarHeight 自定义标题栏的逻辑高度，单位为 macOS point/Qt 逻辑像素。
 *
 * 全尺寸客户区只改变内容延伸范围，不会自动把系统按钮从默认短标题栏中心移到应用自定义
 * 标题栏中心。本函数保留按钮的系统尺寸和水平间距，仅换算并更新纵坐标；窗口状态切换后
 * AppKit 可能重新布局按钮，因此允许调用方重复执行。
 */
void alignMacWindowButtons(QWidget *window, qreal titleBarHeight);
#else
[[nodiscard]] inline bool enableMacWindowChrome(QWidget *window)
{
    Q_UNUSED(window)
    return true;
}

/** @brief 非 macOS 平台没有 AppKit 拖动路径，返回 false 以便调用方使用 Qt 回退实现。 */
[[nodiscard]] inline bool beginMacWindowDrag(QWidget *window)
{
    Q_UNUSED(window)
    return false;
}

/** @brief 非 macOS 平台没有 NSWindow 缩放路径，返回 false 以便调用方使用 Qt 回退实现。 */
[[nodiscard]] inline bool toggleMacWindowZoom(QWidget *window)
{
    Q_UNUSED(window)
    return false;
}

/** @brief 非 macOS 平台不存在交通灯按钮，因此无需执行任何对齐操作。 */
inline void alignMacWindowButtons(QWidget *window, qreal titleBarHeight)
{
    Q_UNUSED(window)
    Q_UNUSED(titleBarHeight)
}
#endif

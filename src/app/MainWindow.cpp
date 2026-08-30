#include "app/MainWindow.h"

#include "ai/AiAssistantWidget.h"
#include "ai/AiSettingsService.h"
#include "core/ActivityLog.h"
#include "forwarding/PortForwardManagerWidget.h"
#include "i18n/LocalizationService.h"
#include "productivity/ProductivityModel.h"
#include "productivity/ProductivityWidget.h"
#include "security/Vault.h"
#include "settings/TerminalSettings.h"
#include "storage/ApplicationDatabase.h"
#include "terminal/TerminalPane.h"
#include "terminal/TerminalWorkspace.h"
#include "ui/ConnectionDialog.h"
#include "ui/HostManagerWidget.h"
#include "ui/MacWindowChrome.h"
#include "ui/SessionTabBar.h"
#include "ui/SettingsDialog.h"
#include "ui/SettingsWidgets.h"
#include "ui/SshKeyManagerDialog.h"
#include "ui/UiIcons.h"
#include "ui/VaultDialog.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>
#include <QUuid>

#include <algorithm>
#include <optional>
#include <utility>

namespace {

/** @brief 一体化标题栏的固定逻辑高度，同时作为 macOS 交通灯垂直对齐的唯一基准。 */
constexpr int kWindowChromeHeight = 52;

/** @brief Windows/Linux 无边框窗口边缘的逻辑像素命中宽度。 */
constexpr int kWindowResizeBorderWidth = 7;

/**
 * @brief 计算全局指针位置命中的无边框窗口缩放边缘。
 * @param window 待检测的顶层窗口。
 * @param globalPosition 指针的全局逻辑坐标。
 * @return 可交给 QWindow::startSystemResize() 的边缘组合；不在边缘或窗口不可缩放时返回空。
 *
 * 使用 Qt 的系统缩放入口而不是手工修改 geometry，桌面环境才能继续提供 Windows Snap、
 * Wayland 合成器约束、多屏 DPI 与最小尺寸限制。最大化/全屏窗口没有可拖动边缘，必须直接
 * 返回空，避免顶边操作意外把窗口恢复。
 */
[[nodiscard]] Qt::Edges resizeEdgesAt(const QWidget *window, const QPointF &globalPosition)
{
    if (window == nullptr || window->isMaximized() || window->isFullScreen()) {
        return {};
    }

    const QRect globalRect(window->mapToGlobal(QPoint(0, 0)), window->size());
    const QPoint position = globalPosition.toPoint();
    Qt::Edges edges;
    if (position.x() >= globalRect.left()
        && position.x() < globalRect.left() + kWindowResizeBorderWidth) {
        edges |= Qt::LeftEdge;
    } else if (position.x() <= globalRect.right()
               && position.x() > globalRect.right() - kWindowResizeBorderWidth) {
        edges |= Qt::RightEdge;
    }
    if (position.y() >= globalRect.top()
        && position.y() < globalRect.top() + kWindowResizeBorderWidth) {
        edges |= Qt::TopEdge;
    } else if (position.y() <= globalRect.bottom()
               && position.y() > globalRect.bottom() - kWindowResizeBorderWidth) {
        edges |= Qt::BottomEdge;
    }
    return edges;
}

/**
 * @brief 把窗口缩放边缘组合转换为用户熟悉的指针形状。
 * @param edges 当前命中的一个边或一组对角边。
 * @return 对应横向、纵向或对角缩放的 Qt 光标；空边缘返回普通箭头。
 */
[[nodiscard]] Qt::CursorShape resizeCursorForEdges(const Qt::Edges edges)
{
    if (edges == (Qt::LeftEdge | Qt::TopEdge)
        || edges == (Qt::RightEdge | Qt::BottomEdge)) {
        return Qt::SizeFDiagCursor;
    }
    if (edges == (Qt::RightEdge | Qt::TopEdge)
        || edges == (Qt::LeftEdge | Qt::BottomEdge)) {
        return Qt::SizeBDiagCursor;
    }
    if (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge)) {
        return Qt::SizeHorCursor;
    }
    if (edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge)) {
        return Qt::SizeVerCursor;
    }
    return Qt::ArrowCursor;
}

/**
 * @brief 共享侧栏允许缩小到的宽度。
 *
 * 三个页面均已改成单列与可换行布局，因此 260px 仍可完整操作；继续限制为 320px 会让
 * 小屏幕用户无法把更多宽度让给终端工作区，也与设计稿的拖动范围不一致。
 */
constexpr int kSidebarMinimumWidth = 260;

/** @brief 参考设计的共享侧栏默认宽度，首次打开时在信息密度和终端空间之间取平衡。 */
constexpr int kSidebarDefaultWidth = 320;

/** @brief 终端工作区的最小宽度，确保侧栏拖动后终端仍可进行基本交互。 */
constexpr int kWorkspaceMinimumWidth = 420;

/**
 * @brief 侧栏宽度允许恢复到的上限。
 *
 * 用户可能在超宽显示器上把侧栏拖得很宽，再换到笔记本屏幕启动。上限保证恢复出来的侧栏
 * 不会挤掉整个终端区，同时仍远大于默认宽度，正常的加宽习惯不会被悄悄改小。
 */
constexpr int kSidebarMaximumWidth = 900;

/** @brief 侧栏宽度的设置键；数值以像素为单位。 */
constexpr auto kSidebarWidthSettingKey = "layout/sidebar_width";

/** @brief 拖动停止后延迟落库的毫秒数，用于合并一次拖动中的大量 splitterMoved。 */
constexpr int kSidebarWidthPersistDelayMs = 300;

/**
 * @brief 从标题区事件源启动操作系统级窗口移动。
 * @param eventSource 实际接收鼠标事件的标题区控件。
 * @return 原生或 Qt 系统移动接口成功接管事件时返回 true，否则返回 false。
 *
 * macOS 全尺寸客户区优先使用当前 NSEvent 驱动 NSWindow；其余平台或原生事件暂不可用时
 * 回退 QWindow::startSystemMove。两条路径都由窗口系统处理多屏和缩放，避免手算坐标漂移。
 */
[[nodiscard]] bool beginWindowChromeMove(QWidget *eventSource)
{
    QWidget *topLevelWindow = eventSource != nullptr ? eventSource->window() : nullptr;
    if (topLevelWindow == nullptr) {
        return false;
    }
    if (beginMacWindowDrag(topLevelWindow)) {
        return true;
    }
    return topLevelWindow->windowHandle() != nullptr
        && topLevelWindow->windowHandle()->startSystemMove();
}

/**
 * @brief 按平台标准语义在窗口放大状态与先前尺寸之间切换。
 * @param eventSource 实际接收双击事件的标题区控件。
 *
 * macOS 的 zoom: 会记住用户原始 frame；Qt 回退仅用于其他平台或 Cocoa 窗口尚不可用时。
 */
void toggleWindowChromeZoom(QWidget *eventSource)
{
    QWidget *topLevelWindow = eventSource != nullptr ? eventSource->window() : nullptr;
    if (topLevelWindow == nullptr || toggleMacWindowZoom(topLevelWindow)) {
        return;
    }
    topLevelWindow->isMaximized() ? topLevelWindow->showNormal() : topLevelWindow->showMaximized();
}

/**
 * @brief 作为原生标题栏背景的可拖动 Qt 容器。
 *
 * macOS 的全尺寸内容模式会把该控件绘制到交通灯按钮所在区域，但普通 QWidget 并不会
 * 自动获得拖动窗口能力。本类只在用户按住“空白背景”时调用平台窗口移动接口；标签和
 * 按钮等子控件会先接收自己的鼠标事件，因此不会误触发窗口移动。
 */
class WindowChromeWidget final : public QWidget {
public:
    /** @brief 创建标题背景控件。 */
    explicit WindowChromeWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("WindowChrome"));
    }

protected:
    /** @brief 左键按住标题空白处时请求操作系统开始窗口拖动。 */
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && beginWindowChromeMove(this)) {
            // 交由操作系统执行移动可正确处理多屏缩放、全屏空间和贴边行为；手工修改窗口
            // 坐标会在 Retina 与普通屏幕之间产生跳动，也无法保持 macOS 的系统动画。
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    /** @brief 双击标题空白处在最大化和普通状态之间切换。 */
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && window() != nullptr) {
            toggleWindowChromeZoom(this);
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }
};

/**
 * @brief 绘制参考设计中克制的细线型窗口控制按钮。
 *
 * QStyle::SP_TitleBar* 会随 Windows 主题、Linux 图标主题和 Qt 样式产生明显不同的尺寸、
 * 填充与立体边框，无法形成稳定的产品标题栏。本控件仍复用 QToolButton 的悬停、按下、
 * 焦点和辅助功能行为，只接管中央符号绘制，从而让三个平台版本的视觉密度保持一致。
 */
class WindowControlButton final : public QToolButton {
public:
    /** @brief 按钮中央需要绘制的窗口操作符号。 */
    enum class Glyph {
        Minimize,
        Maximize,
        Close
    };

    /**
     * @brief 创建指定符号的窗口控制按钮。
     * @param glyph 最小化、最大化或关闭符号。
     * @param parent 按钮所属的标题栏容器。
     */
    explicit WindowControlButton(const Glyph glyph, QWidget *parent = nullptr)
        : QToolButton(parent)
        , m_glyph(glyph)
    {
        setCursor(Qt::ArrowCursor);
        setFocusPolicy(Qt::NoFocus);
    }

    /**
     * @brief 切换最大化按钮显示“最大化”还是“还原”双框符号。
     * @param restoreMode true 表示窗口已最大化，应绘制还原符号。
     *
     * 该状态只对 Maximize 类型生效；比较后再更新可避免 resize/状态事件连续到达时重复重绘。
     */
    void setRestoreMode(const bool restoreMode)
    {
        if (m_restoreMode == restoreMode) {
            return;
        }
        m_restoreMode = restoreMode;
        update();
    }

protected:
    /** @brief 先让 QToolButton/QSS 绘制交互背景，再在中心叠加高 DPI 细线符号。 */
    void paintEvent(QPaintEvent *event) override
    {
        QToolButton::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor stroke = isEnabled()
            ? (underMouse() ? QColor(QStringLiteral("#f1f3f8"))
                            : QColor(QStringLiteral("#9ca4b8")))
            : QColor(QStringLiteral("#686f82"));
        painter.setPen(QPen(stroke, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);

        const QPointF center = rect().center();
        switch (m_glyph) {
        case Glyph::Minimize:
            // 参考图中的最小化符号位于视觉中心略下方，避免一条短线看起来向上漂浮。
            painter.drawLine(center + QPointF(-5.0, 3.0), center + QPointF(5.0, 3.0));
            break;
        case Glyph::Maximize:
            if (m_restoreMode) {
                // 两个错位方框表达“还原”，后框只绘制未被前框覆盖的轮廓，保持图标清爽。
                painter.drawRoundedRect(QRectF(center.x() - 3.0,
                                               center.y() - 5.0,
                                               8.0,
                                               8.0),
                                        0.8,
                                        0.8);
                painter.drawRoundedRect(QRectF(center.x() - 5.0,
                                               center.y() - 3.0,
                                               8.0,
                                               8.0),
                                        0.8,
                                        0.8);
            } else {
                painter.drawRoundedRect(QRectF(center.x() - 5.0,
                                               center.y() - 5.0,
                                               10.0,
                                               10.0),
                                        1.0,
                                        1.0);
            }
            break;
        case Glyph::Close:
            painter.drawLine(center + QPointF(-4.5, -4.5), center + QPointF(4.5, 4.5));
            painter.drawLine(center + QPointF(4.5, -4.5), center + QPointF(-4.5, 4.5));
            break;
        }
    }

private:
    /** @brief 构造时固定的基础符号类型。 */
    Glyph m_glyph;
    /** @brief 最大化按钮是否应显示双框还原符号。 */
    bool m_restoreMode = false;
};

/** @brief 侧栏页面的稳定索引，必须与 QStackedWidget 添加顺序保持一致。 */
enum SidebarPageIndex {
    HostsPage = 0,
    ProductivityPage = 1,
    ForwardingPage = 2,
    AiPage = 3
};

} // namespace

MainWindow::MainWindow(ApplicationDatabase *database,
                       CredentialVault *credentialVault,
                       LocalizationService *localization,
                       QWidget *parent)
    : QMainWindow(parent)
    , m_tabs(new QTabWidget(this))
    , m_database(database)
    , m_credentialVault(credentialVault)
    , m_vaultIdleTimer(new QTimer(this))
    , m_localization(localization)
{
    Q_ASSERT(m_database != nullptr);
    Q_ASSERT(m_credentialVault != nullptr);

    // 设置服务要在创建终端控件前加载并绑定应用主题，避免窗口先按默认深色绘制一帧再跳到
    // 用户配置；此时界面尚未建立，但 showTransientMessage 只写日志，不依赖任何控件。
    m_settingsService = new TerminalSettingsService(m_database, this);
    m_settingsService->bindApplicationTheme(qApp);
    connect(m_settingsService,
            &TerminalSettingsService::warningOccurred,
            this,
            [this](const QString &message) { showTransientMessage(message, 10000); });
    QString settingsLoadError;
    if (!m_settingsService->load(&settingsLoadError)) {
        showTransientMessage(tr("Failed to read settings: %1").arg(settingsLoadError), 0);
    }
    m_credentialVault->setPasswordProtectionEnabled(
        m_settingsService->currentSettings().vaultPasswordEnabled);
    m_productivityModel = new ProductivityModel(m_database, this);

// Qt 6.8 尚不能在 Windows/Linux 上把普通 QWidget 扩展进系统标题栏。使用无边框窗口后，
// 下方的 WindowChromeWidget 才能成为唯一标题行；移动和缩放仍交给 QWindow 系统接口，
// 不会牺牲贴边、多屏或桌面合成器行为。macOS 继续走原生全尺寸内容标题栏。
#ifndef Q_OS_MACOS
    setWindowFlag(Qt::FramelessWindowHint, true);
#endif

    // Qt 6.9 起 ExpandedClientAreaHint 会在平台窗口创建前通知 Cocoa：客户区需要覆盖标题栏，
    // NoTitleBarBackgroundHint 同时移除系统白色背景。只调用 AppKit 的 FullSizeContentView 太晚，
    // QMainWindow 已按旧安全区完成布局，正是此前截图中仍残留一条白色标题栏的根因。
#if defined(Q_OS_MACOS) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    setWindowFlag(Qt::ExpandedClientAreaHint, true);
    setWindowFlag(Qt::NoTitleBarBackgroundHint, true);
    // 默认 QWidget 布局会主动避让 QWindow::safeAreaMargins()。标题区已经在布局内部为交通灯
    // 留出 76 像素横向空间，因此必须关闭自动避让，才能让第一行真正从窗口 y=0 开始绘制。
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
#endif

    setWindowTitle(tr("Shellium"));
    resize(1380, 860);
    setMinimumSize(980, 620);

    // QTabWidget 只负责页面生命周期和工作区序列化；其传统页签完全隐藏，真正可见的会话
    // 标签由标题区 SessionTabBar 镜像。这样不会为了视觉效果重写可靠的页面管理逻辑。
    m_tabs->setObjectName(QStringLiteral("TerminalPages"));
    m_tabs->setDocumentMode(true);
    m_tabs->setMovable(true);
    m_tabs->setTabsClosable(false);
    m_tabs->tabBar()->hide();

    // 整个应用只保留一个中央根布局：标题区、左侧导航轨、单一侧栏与工作区构成稳定网格。
    // 不再使用可浮动 Dock，因为 Dock 自带标题、拖动边框和标签组，会破坏现代客户端要求
    // 的连续色块、固定对齐和可预测收缩行为。
    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("ShelliumRoot"));
#if defined(Q_OS_MACOS) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    root->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
#endif
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_titleChrome = new WindowChromeWidget(root);
    m_titleChrome->setFixedHeight(kWindowChromeHeight);
    auto *titleLayout = new QHBoxLayout(m_titleChrome);
#ifdef Q_OS_MACOS
    // 为原生关闭、最小化和缩放按钮保留命中区域；该留白只在 macOS 全尺寸标题模式需要。
    titleLayout->setContentsMargins(76, 5, 10, 5);
#else
    titleLayout->setContentsMargins(10, 5, 10, 5);
#endif
    titleLayout->setSpacing(4);
    rootLayout->addWidget(m_titleChrome);

    auto *workspaceBody = new QWidget(root);
    workspaceBody->setObjectName(QStringLiteral("WorkspaceBody"));
    auto *bodyLayout = new QHBoxLayout(workspaceBody);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    m_navigationRail = new QWidget(workspaceBody);
    m_navigationRail->setObjectName(QStringLiteral("NavigationRail"));
    m_navigationRail->setFixedWidth(72);
    auto *railLayout = new QVBoxLayout(m_navigationRail);
    // 顶部留白原为 20px，比侧栏标题行的实际视觉顶部低了一截——标题行的高度由里面的按钮
    // （QPushButton min-height 28 + padding）撑起，比纯文字行高，图标却仍按纯文字的位置
    // 起算，看起来像是往下错位。收紧到 12px，让导航图标与"主机"标题顶部对齐。
    railLayout->setContentsMargins(10, 12, 10, 12);
    railLayout->setSpacing(8);

    auto *navigationGroup = new QButtonGroup(this);
    navigationGroup->setExclusive(true);
    const QStringList navigationTooltips = {
        tr("Hosts"), tr("Productivity"), tr("Port forwarding"), tr("AI Assistant")};
    for (int pageIndex = HostsPage; pageIndex <= AiPage; ++pageIndex) {
        auto *button = new QToolButton(m_navigationRail);
        button->setProperty("navButton", true);
        button->setCheckable(true);
        button->setAutoRaise(false);
        button->setFixedSize(44, 44);
        button->setToolTip(navigationTooltips.at(pageIndex));
        button->setAccessibleName(navigationTooltips.at(pageIndex));
        navigationGroup->addButton(button, pageIndex);
        railLayout->addWidget(button, 0, Qt::AlignHCenter);
        m_navigationButtons.append(button);
        connect(button,
                &QToolButton::toggled,
                this,
                [this, pageIndex](const bool checked) {
                    if (checked) {
                        // macOS Accessibility 可能直接设置 checked 而不派发 clicked；监听
                        // 状态变化可让鼠标、键盘和辅助技术共享同一页面路由。
                        showSidebarPage(pageIndex);
                    }
                });
    }
    m_navigationButtons.constFirst()->setChecked(true);
    connect(navigationGroup,
            &QButtonGroup::idClicked,
            this,
            [this](const int pageIndex) {
                // 用户在侧栏已收起时再次点击当前导航项，checked 不会变化，toggled 因而不
                // 会发出；点击信号负责这一条“用当前图标重新展开”的交互路径。
                if (m_sidebarPanel == nullptr
                    || !m_sidebarPanel->isVisible()
                    || m_activeSidebarPage != pageIndex) {
                    showSidebarPage(pageIndex);
                }
            });
    railLayout->addStretch(1);

    // 侧栏和终端区使用水平分隔器承载：把手位于侧栏最右边框，用户可直接拖动调整宽度。
    // 导航轨不放进分隔器，避免拖动侧栏时改变其固定宽度或图标的稳定屏幕位置。
    m_sidebarSplitter = new QSplitter(Qt::Horizontal, workspaceBody);
    m_sidebarSplitter->setObjectName(QStringLiteral("SidebarSplitter"));
    m_sidebarSplitter->setChildrenCollapsible(false);
    m_sidebarSplitter->setHandleWidth(6);
    m_sidebarSplitter->setOpaqueResize(true);

    m_sidebarPanel = new QWidget(m_sidebarSplitter);
    m_sidebarPanel->setObjectName(QStringLiteral("SidebarPanel"));
    m_sidebarPanel->setMinimumWidth(kSidebarMinimumWidth);
    auto *sidebarLayout = new QVBoxLayout(m_sidebarPanel);
    // 四个侧栏页面共享同一容器和同一条标题栏；AI 助手页只在标题栏下方额外插入一条自己的
    // 目标与会话动作行，不再重复渲染页面标题本身。
    sidebarLayout->setContentsMargins(10, 10, 10, 8);
    sidebarLayout->setSpacing(8);
    m_sidebarTitle = new QLabel(tr("Hosts"), m_sidebarPanel);
    m_sidebarTitle->setObjectName(QStringLiteral("SidebarHeader"));
    m_hostCountLabel = new QLabel(tr("0 hosts"), m_sidebarPanel);
    m_hostCountLabel->setObjectName(QStringLiteral("HostCountLabel"));
    auto *sidebarHeaderLayout = new QHBoxLayout;
    sidebarHeaderLayout->setContentsMargins(0, 0, 0, 0);
    sidebarHeaderLayout->setSpacing(6);
    sidebarHeaderLayout->addWidget(m_sidebarTitle);
    sidebarHeaderLayout->addWidget(m_hostCountLabel, 0, Qt::AlignVCenter);
    sidebarHeaderLayout->addStretch(1);
    sidebarLayout->addLayout(sidebarHeaderLayout);

    m_sidebarStack = new QStackedWidget(m_sidebarPanel);
    m_sidebarStack->setObjectName(QStringLiteral("SidebarStack"));
    // 三个页面共享应用级数据库/模型，但在侧栏中互斥展示。页面不会因切换被销毁，因此
    // 主机选择、Snippet 编辑内容和转发统计都能原样保留。
    m_hostManager = new HostManagerWidget(m_database, m_credentialVault, m_sidebarStack);
    m_productivityWidget = new ProductivityWidget(m_productivityModel, m_sidebarStack);
    m_portForwardManager = new PortForwardManagerWidget(m_database, m_sidebarStack);
    m_aiSettingsService = new AiSettingsService(m_database, m_credentialVault, this);
    QString aiSettingsLoadError;
    if (!m_aiSettingsService->load(&aiSettingsLoadError)) {
        showTransientMessage(tr("Failed to read the AI assistant configuration: %1").arg(aiSettingsLoadError), 0);
    }
    // AI 页本身是一个与终端标签索引同步的内部栈，每个标签稍后在 createWorkspaceTab() 中
    // 创建独立 AiAssistantWidget。不能只创建一个全局控件再替换 m_history：旧标签的异步
    // 模型回复和终端捕获仍可能落入新标签，独立 QObject/AiClient 才能彻底隔离状态。
    m_aiAssistantStack = new QStackedWidget(m_sidebarStack);
    m_aiAssistantStack->setObjectName(QStringLiteral("AiAssistantStack"));
    m_sidebarStack->addWidget(m_hostManager);
    m_sidebarStack->addWidget(m_productivityWidget);
    m_sidebarStack->addWidget(m_portForwardManager);
    m_sidebarStack->addWidget(m_aiAssistantStack);
    sidebarLayout->addWidget(m_sidebarStack, 1);

    // HostManagerWidget 在构造末尾已完成首次刷新，该次信号早于 MainWindow 的连接，因此先
    // 读取快照初始化标签；之后的增删、导入和刷新再由信号增量同步。
    m_hostCountLabel->setText(tr("%1 hosts").arg(m_hostManager->hostCount()));
    connect(m_hostManager,
            &HostManagerWidget::hostCountChanged,
            this,
            [this](const int count) {
                if (m_hostCountLabel != nullptr) {
                    m_hostCountLabel->setText(tr("%1 hosts").arg(count));
                }
            });

    // 主机页的高频动作与侧栏标题同排右对齐。标题归 MainWindow 所有，因此动作条由主机页
    // 提供、在这里改挂到标题行；切到其他页面时隐藏，避免动作与页面内容对不上。
    m_hostHeaderActions = m_hostManager->headerActionsWidget();
    sidebarHeaderLayout->addWidget(m_hostHeaderActions);

    // 终端区保留最小操作宽度；侧栏不参与弹性扩张，只有用户拖动把手时才改变其宽度。
    // 初始尺寸中的较大第二项只是表达“剩余空间归终端”，QSplitter 会按窗口实际宽度缩放。
    // 空态页与标签页同处一个栈，因此最小宽度设在栈上；两种状态占据完全相同的位置，
    // 用户新建第一个标签时不会看到终端区宽度跳变。
    m_workspaceStack = new QStackedWidget(this);
    m_workspaceStack->setObjectName(QStringLiteral("WorkspaceStack"));
    m_workspacePlaceholder = createWorkspacePlaceholder();
    m_workspaceStack->addWidget(m_workspacePlaceholder);
    m_workspaceStack->addWidget(m_tabs);
    m_workspaceStack->setMinimumWidth(kWorkspaceMinimumWidth);
    m_sidebarSplitter->addWidget(m_sidebarPanel);
    m_sidebarSplitter->addWidget(m_workspaceStack);
    m_sidebarSplitter->setStretchFactor(0, 0);
    m_sidebarSplitter->setStretchFactor(1, 1);
    restoreSidebarWidth();

    // 拖动本身实时生效，落库延后合并：定时器在拖动停止后写一次，析构时再补写未触发的一次。
    m_sidebarWidthPersistTimer = new QTimer(this);
    m_sidebarWidthPersistTimer->setSingleShot(true);
    m_sidebarWidthPersistTimer->setInterval(kSidebarWidthPersistDelayMs);
    connect(m_sidebarWidthPersistTimer,
            &QTimer::timeout,
            this,
            &MainWindow::persistSidebarWidth);
    connect(m_sidebarSplitter,
            &QSplitter::splitterMoved,
            this,
            [this]() { m_sidebarWidthPersistTimer->start(); });

    bodyLayout->addWidget(m_navigationRail);
    bodyLayout->addWidget(m_sidebarSplitter, 1);
    rootLayout->addWidget(workspaceBody, 1);
    setCentralWidget(root);

    // 启动时刻意不创建任何标签页：默认状态是品牌空态页，用户按"+"或从主机树连接时
    // 才会产生第一个会话。预置一个空标签既不代表任何服务器，也会让"新建标签"这一步
    // 显得可有可无。
    updateWorkspacePlaceholder();

    auto *connectAction = new QAction(this);
    registerTranslatable(connectAction,
                         QT_TRANSLATE_NOOP("MainWindow", "Connect to Server"),
                         QT_TRANSLATE_NOOP("MainWindow", "Connect to an SSH server in the current terminal pane"));
    connectAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));

    auto *nestedConnectAction = new QAction(this);
    registerTranslatable(nestedConnectAction,
                         QT_TRANSLATE_NOOP("MainWindow", "Nested Connect to…"),
                         QT_TRANSLATE_NOOP("MainWindow",
                             "Connects to an internal host through the current server. The app maintains the layers itself, so the file manager can switch accurately between them."));

    // 菜单使用平台中立名称：Windows 会启动 PowerShell/cmd，macOS 与 Linux 默认启动 zsh。
    auto *localShellAction = new QAction(this);
    registerTranslatable(localShellAction,
                         QT_TRANSLATE_NOOP("MainWindow", "Local terminal"),
                         QT_TRANSLATE_NOOP("MainWindow", "Start a local login zsh in the current terminal pane"));
    localShellAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")));

    auto *newTabAction = new QAction(this);
    registerTranslatable(newTabAction, QT_TRANSLATE_NOOP("MainWindow", "New tab"));
    newTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));

    auto *closeTabAction = new QAction(this);
    registerTranslatable(closeTabAction, QT_TRANSLATE_NOOP("MainWindow", "Close tab"));
    closeTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));

    auto *renameTabAction = new QAction(this);
    registerTranslatable(renameTabAction, QT_TRANSLATE_NOOP("MainWindow", "Rename tab"));

    auto *toggleFilesAction = new QAction(this);
    registerTranslatable(toggleFilesAction,
                         QT_TRANSLATE_NOOP("MainWindow", "Toggle remote file manager"),
                         QT_TRANSLATE_NOOP("MainWindow",
                             "Show or hide the remote file manager area inside the current terminal tab"));
    toggleFilesAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));

    auto *searchTerminalAction = new QAction(this);
    registerTranslatable(searchTerminalAction, QT_TRANSLATE_NOOP("MainWindow", "Search Terminal Output…"));
    auto *exportTerminalAction = new QAction(this);
    registerTranslatable(exportTerminalAction, QT_TRANSLATE_NOOP("MainWindow", "Export Terminal Output…"));
    auto *settingsAction = new QAction(this);
    registerTranslatable(settingsAction, QT_TRANSLATE_NOOP("MainWindow", "Settings…"));
    auto *commandPaletteAction = new QAction(this);
    registerTranslatable(commandPaletteAction, QT_TRANSLATE_NOOP("MainWindow", "Command Palette…"));
    commandPaletteAction->setObjectName(QStringLiteral("CommandPaletteAction"));
    auto *saveWorkspaceAction = new QAction(this);
    registerTranslatable(saveWorkspaceAction, QT_TRANSLATE_NOOP("MainWindow", "Save Current Workspace…"));
    auto *openWorkspaceAction = new QAction(this);
    registerTranslatable(openWorkspaceAction, QT_TRANSLATE_NOOP("MainWindow", "Open Workspace…"));
    auto *removeWorkspaceAction = new QAction(this);
    registerTranslatable(removeWorkspaceAction, QT_TRANSLATE_NOOP("MainWindow", "Delete Workspace…"));

    // 动作图标同时服务菜单、命令面板和标题栏；优先使用桌面主题的语义图标，缺失时由
    // UiIcons 回退到 Qt 标准资源，避免在 macOS/Linux 之间出现空白动作。
    connectAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Connect, this->palette().color(QPalette::WindowText)));
    nestedConnectAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Connect, this->palette().color(QPalette::WindowText)));
    localShellAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Terminal, this->palette().color(QPalette::WindowText)));
    newTabAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)));
    closeTabAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Close, this->palette().color(QPalette::WindowText)));
    renameTabAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)));
    toggleFilesAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Folder, this->palette().color(QPalette::WindowText)));
    searchTerminalAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Search, this->palette().color(QPalette::WindowText)));
    exportTerminalAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Download, this->palette().color(QPalette::WindowText)));
    settingsAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Settings, this->palette().color(QPalette::WindowText)));
    commandPaletteAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Command, this->palette().color(QPalette::WindowText)));
    saveWorkspaceAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Download, this->palette().color(QPalette::WindowText)));
    openWorkspaceAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Upload, this->palette().color(QPalette::WindowText)));
    removeWorkspaceAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)));

    // 标题区不是 QToolBar，而是中央内容树的第一行。macOS 原生标题栏透明后，这一行会
    // 真正延伸到交通灯背后；因此会话标签与窗口标题只占一次高度，不会再上下叠两层。
    m_sidebarToggleAction = new QAction(this);
    registerTranslatable(m_sidebarToggleAction,
                         QT_TRANSLATE_NOOP("MainWindow", "Show sidebar"),
                         QT_TRANSLATE_NOOP("MainWindow", "Expand or collapse the Hosts, Productivity, and Port Forwarding areas on the left"));
    m_sidebarToggleAction->setCheckable(true);
    m_sidebarToggleAction->setChecked(true);

    QMenuBar *applicationMenuBar = nullptr;
#ifdef Q_OS_MACOS
    // macOS 菜单属于屏幕顶部的系统菜单栏，不应塞进窗口标题行；保持原生菜单还能保留系统
    // 统一的键盘导航、应用菜单与 VoiceOver 语义。
    applicationMenuBar = menuBar();
    applicationMenuBar->setNativeMenuBar(true);
#else
    // Windows/Linux 的标题栏按产品视觉只显示 Shellium 名称，不再平铺传统菜单项。仍保留
    // 一个不可见的菜单模型承载菜单对象和 QAction 关系，使已有快捷键、命令面板与动作更新
    // 逻辑不需要复制；它不进入布局，也不会占用标题栏或中央区域的任何高度。
    applicationMenuBar = new QMenuBar(this);
    applicationMenuBar->setObjectName(QStringLiteral("ApplicationMenuModel"));
    applicationMenuBar->setNativeMenuBar(false);
    applicationMenuBar->hide();
#endif

    auto *sidebarChromeButton = new QToolButton(m_titleChrome);
    sidebarChromeButton->setProperty("chromeButton", true);
    sidebarChromeButton->setProperty("chromeRole", QStringLiteral("sidebar"));
    sidebarChromeButton->setDefaultAction(m_sidebarToggleAction);
    sidebarChromeButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    sidebarChromeButton->setFixedSize(38, 38);
    titleLayout->addWidget(sidebarChromeButton);
    m_chromeButtons.append(sidebarChromeButton);

#ifndef Q_OS_MACOS
    // 产品名称紧跟侧栏按钮，位置与参考图中的主产品入口一致。使用 QLabel 而不是可点击按钮，
    // 明确表达这里是品牌标识而非被移到另一种外观下的传统菜单。
    auto *productNameLabel = new QLabel(QStringLiteral("Shellium"), m_titleChrome);
    productNameLabel->setObjectName(QStringLiteral("WindowProductName"));
    productNameLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    productNameLabel->setAccessibleName(tr("Shellium product name"));
    titleLayout->addWidget(productNameLabel, 0, Qt::AlignVCenter);
#endif

    // 标签滚动箭头分置标签栏两侧，与标签栏、"新建标签"按钮同为标题行的布局项。用布局
    // 分配空间而不是把按钮浮动叠在标签栏之上，标签再多也不会与"+"互相遮挡。
    auto *scrollLeftButton = new QToolButton(m_titleChrome);
    scrollLeftButton->setProperty("chromeButton", true);
    scrollLeftButton->setProperty("chromeRole", QStringLiteral("scrollLeft"));
    scrollLeftButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    scrollLeftButton->setFixedSize(18, 38);
    scrollLeftButton->setToolTip(tr("Scroll tabs left"));
    scrollLeftButton->setAccessibleName(tr("Scroll tabs left"));
    // 显隐若改变布局宽度，就会反过来改变标签是否溢出的判定，形成显示→不溢出→隐藏→
    // 溢出的抖动死循环。保留隐藏时的占位可彻底切断这条反馈路径。
    QSizePolicy scrollPolicy = scrollLeftButton->sizePolicy();
    scrollPolicy.setRetainSizeWhenHidden(true);
    scrollLeftButton->setSizePolicy(scrollPolicy);
    scrollLeftButton->hide();
    m_tabScrollLeftButton = scrollLeftButton;
    titleLayout->addWidget(scrollLeftButton);
    m_chromeButtons.append(scrollLeftButton);

    m_titleTabBar = new SessionTabBar(m_titleChrome);
    m_titleTabBar->setObjectName(QStringLiteral("SessionTitleTabs"));
    m_titleTabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    // 标签栏只占标签自身需要的宽度，"+"因此紧贴最后一个标签的右侧，而不是被推到窗口
    // 最右端。Maximum 策略允许在标签溢出时继续压缩标签栏，此时"+"自然停在标题行末尾，
    // 不需要为"固定到最右边"再写一条特殊分支。
    m_titleTabBar->setMinimumWidth(0);
    m_titleTabBar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    for (int index = 0; index < m_tabs->count(); ++index) {
        m_titleTabBar->addTab(m_tabs->tabIcon(index), m_tabs->tabText(index));
    }
    m_titleTabBar->setCurrentIndex(m_tabs->currentIndex());
    titleLayout->addWidget(m_titleTabBar);

    auto *scrollRightButton = new QToolButton(m_titleChrome);
    scrollRightButton->setProperty("chromeButton", true);
    scrollRightButton->setProperty("chromeRole", QStringLiteral("scrollRight"));
    scrollRightButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    scrollRightButton->setFixedSize(18, 38);
    scrollRightButton->setToolTip(tr("Scroll tabs right"));
    scrollRightButton->setAccessibleName(tr("Scroll tabs right"));
    scrollRightButton->setSizePolicy(scrollPolicy);
    scrollRightButton->hide();
    m_tabScrollRightButton = scrollRightButton;
    titleLayout->addWidget(scrollRightButton);
    m_chromeButtons.append(scrollRightButton);

    auto *newTabButton = new QToolButton(m_titleChrome);
    newTabButton->setObjectName(QStringLiteral("TitleNewTabButton"));
    newTabButton->setProperty("chromeButton", true);
    newTabButton->setProperty("chromeRole", QStringLiteral("newTab"));
    newTabButton->setDefaultAction(newTabAction);
    newTabButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    newTabButton->setFixedSize(38, 38);
    titleLayout->addWidget(newTabButton);
    m_chromeButtons.append(newTabButton);
    // 剩余空白全部交给"+"右侧的弹性项：标签少时右边是留白，标签多到溢出时弹性项收缩为
    // 零，"+"随之固定在标题行最右端。
    titleLayout->addStretch(1);

#ifndef Q_OS_MACOS
    // 无边框窗口不会再获得系统标题按钮，因此在同一标题行最右侧提供等价操作。按钮触发的
    // 仍是 QWidget 标准窗口状态 API，任务栏、桌面快捷键和窗口管理器可以继续感知状态。
    auto *minimizeButton = new WindowControlButton(
        WindowControlButton::Glyph::Minimize, m_titleChrome);
    minimizeButton->setObjectName(QStringLiteral("WindowMinimizeButton"));
    minimizeButton->setProperty("windowControl", true);
    minimizeButton->setProperty("windowControlRole", QStringLiteral("minimize"));
    minimizeButton->setToolTip(tr("Minimize"));
    minimizeButton->setAccessibleName(tr("Minimize window"));
    minimizeButton->setFixedSize(48, 42);
    titleLayout->addWidget(minimizeButton);
    connect(minimizeButton, &QToolButton::clicked, this, &QWidget::showMinimized);

    m_windowMaximizeButton = new WindowControlButton(
        WindowControlButton::Glyph::Maximize, m_titleChrome);
    m_windowMaximizeButton->setObjectName(QStringLiteral("WindowMaximizeButton"));
    m_windowMaximizeButton->setProperty("windowControl", true);
    m_windowMaximizeButton->setProperty("windowControlRole", QStringLiteral("maximize"));
    m_windowMaximizeButton->setFixedSize(48, 42);
    titleLayout->addWidget(m_windowMaximizeButton);
    connect(m_windowMaximizeButton, &QToolButton::clicked, this, [this]() {
        isMaximized() ? showNormal() : showMaximized();
    });
    refreshWindowMaximizeIcon();

    auto *closeButton = new WindowControlButton(
        WindowControlButton::Glyph::Close, m_titleChrome);
    closeButton->setObjectName(QStringLiteral("WindowCloseButton"));
    closeButton->setProperty("windowControl", true);
    closeButton->setProperty("windowControlRole", QStringLiteral("close"));
    closeButton->setToolTip(tr("Close"));
    closeButton->setAccessibleName(tr("Close window"));
    closeButton->setFixedSize(48, 42);
    titleLayout->addWidget(closeButton);
    connect(closeButton, &QToolButton::clicked, this, &QWidget::close);
#endif

    connect(scrollLeftButton, &QToolButton::clicked,
            m_titleTabBar, &SessionTabBar::scrollTabsLeft);
    connect(scrollRightButton, &QToolButton::clicked,
            m_titleTabBar, &SessionTabBar::scrollTabsRight);
    connect(m_titleTabBar,
            &SessionTabBar::scrollStateChanged,
            this,
            [scrollLeftButton, scrollRightButton](const bool scrollable,
                                                  const bool canScrollLeft,
                                                  const bool canScrollRight) {
                scrollLeftButton->setVisible(scrollable);
                scrollRightButton->setVisible(scrollable);
                scrollLeftButton->setEnabled(canScrollLeft);
                scrollRightButton->setEnabled(canScrollRight);
            });

    // 标题行的三个标签控件此刻才存在；构造早期那次调用只切换了工作区栈。再同步一次，
    // 空态窗口才会真的把标签栏与滚动箭头的宽度让给"+"。
    updateWorkspacePlaceholder();

    // 标题栏不再放置瞬时消息标签：它会遮挡标签页、在用户读完之前消失，而这些消息真正
    // 的用途是事后排查。全部改由 ActivityLog 落盘，界面不为其保留位置。

    // 命令面板与设置属于全局工具，归入导航轨底部；标题行因此只保留与当前会话直接相关
    // 的标签、滚动和新建入口，符合"上方是内容域、下方是全局工具"的稳定空间模型。
    auto *commandButton = new QToolButton(m_navigationRail);
    commandButton->setProperty("navButton", true);
    commandButton->setProperty("chromeRole", QStringLiteral("command"));
    commandButton->setDefaultAction(commandPaletteAction);
    commandButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    commandButton->setFixedSize(44, 44);
    railLayout->addWidget(commandButton, 0, Qt::AlignHCenter);
    m_chromeButtons.append(commandButton);

    auto *settingsButton = new QToolButton(m_navigationRail);
    settingsButton->setProperty("navButton", true);
    settingsButton->setProperty("chromeRole", QStringLiteral("settings"));
    settingsButton->setDefaultAction(settingsAction);
    settingsButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    settingsButton->setFixedSize(44, 44);
    railLayout->addWidget(settingsButton, 0, Qt::AlignHCenter);
    m_chromeButtons.append(settingsButton);

    QMenu *connectionMenu = applicationMenuBar->addMenu(QString());
    registerTranslatable(connectionMenu, QT_TRANSLATE_NOOP("MainWindow", "Connection"));
    connectionMenu->addAction(connectAction);
    connectionMenu->addAction(nestedConnectAction);
    connectionMenu->addAction(localShellAction);
    connectionMenu->addAction(newTabAction);
    connectionMenu->addAction(closeTabAction);
    connectionMenu->addAction(renameTabAction);

    QMenu *workspaceMenu = applicationMenuBar->addMenu(QString());
    registerTranslatable(workspaceMenu, QT_TRANSLATE_NOOP("MainWindow", "Workspace"));
    workspaceMenu->addAction(saveWorkspaceAction);
    workspaceMenu->addAction(openWorkspaceAction);
    workspaceMenu->addSeparator();
    workspaceMenu->addAction(removeWorkspaceAction);

    QMenu *viewMenu = applicationMenuBar->addMenu(QString());
    registerTranslatable(viewMenu, QT_TRANSLATE_NOOP("MainWindow", "View"));
    viewMenu->addAction(m_sidebarToggleAction);
    viewMenu->addSeparator();
    QAction *showHostsAction = viewMenu->addAction(
        UiIcons::symbol(this, UiIcons::Symbol::Hosts, this->palette().color(QPalette::WindowText)),
        QString());
    registerTranslatable(showHostsAction, QT_TRANSLATE_NOOP("MainWindow", "Hosts"));
    QAction *showProductivityAction = viewMenu->addAction(
        UiIcons::symbol(this, UiIcons::Symbol::Command, this->palette().color(QPalette::WindowText)),
        QString());
    registerTranslatable(showProductivityAction, QT_TRANSLATE_NOOP("MainWindow", "Productivity"));
    QAction *showForwardingAction = viewMenu->addAction(
        UiIcons::symbol(this, UiIcons::Symbol::Forwarding, this->palette().color(QPalette::WindowText)),
        QString());
    registerTranslatable(showForwardingAction, QT_TRANSLATE_NOOP("MainWindow", "Port forwarding"));
    QAction *showAiAction = viewMenu->addAction(
        UiIcons::symbol(this, UiIcons::Symbol::Sparkle, this->palette().color(QPalette::WindowText)),
        QString());
    registerTranslatable(showAiAction, QT_TRANSLATE_NOOP("MainWindow", "AI Assistant"));
    // 视图菜单与导航轨都只切换同一个 QStackedWidget，防止出现菜单显示某个 Dock、导航轨
    // 却高亮另一个页面的双重状态。
    connect(showHostsAction,
            &QAction::triggered,
            this,
            [this]() { showSidebarPage(HostsPage); });
    connect(showProductivityAction,
            &QAction::triggered,
            this,
            [this]() { showSidebarPage(ProductivityPage); });
    connect(showForwardingAction,
            &QAction::triggered,
            this,
            [this]() { showSidebarPage(ForwardingPage); });
    connect(showAiAction,
            &QAction::triggered,
            this,
            [this]() { showSidebarPage(AiPage); });

    QMenu *terminalMenu = applicationMenuBar->addMenu(QString());
    registerTranslatable(terminalMenu, QT_TRANSLATE_NOOP("MainWindow", "Terminal"));
    terminalMenu->addAction(searchTerminalAction);
    terminalMenu->addAction(exportTerminalAction);
    terminalMenu->addSeparator();
    terminalMenu->addAction(commandPaletteAction);
    terminalMenu->addAction(settingsAction);

    auto *unlockVaultAction = new QAction(this);
    registerTranslatable(unlockVaultAction, QT_TRANSLATE_NOOP("MainWindow", "Unlock Credential Vault…"));
    auto *lockVaultAction = new QAction(this);
    registerTranslatable(lockVaultAction, QT_TRANSLATE_NOOP("MainWindow", "Lock credential vault"));
    auto *sshKeyManagerAction = new QAction(this);
    registerTranslatable(sshKeyManagerAction, QT_TRANSLATE_NOOP("MainWindow", "SSH Key Management…"));
    unlockVaultAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Check, this->palette().color(QPalette::WindowText)));
    lockVaultAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Lock, this->palette().color(QPalette::WindowText)));
    sshKeyManagerAction->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Key, this->palette().color(QPalette::WindowText)));
    lockVaultAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+K")));
    unlockVaultAction->setEnabled(
        m_settingsService->currentSettings().vaultPasswordEnabled);
    lockVaultAction->setEnabled(
        m_settingsService->currentSettings().vaultPasswordEnabled);
    QMenu *securityMenu = applicationMenuBar->addMenu(QString());
    registerTranslatable(securityMenu, QT_TRANSLATE_NOOP("MainWindow", "Security"));
    securityMenu->addAction(unlockVaultAction);
    securityMenu->addAction(lockVaultAction);
    securityMenu->addSeparator();
    securityMenu->addAction(sshKeyManagerAction);

    m_configurableActions = {
        {QStringLiteral("connection.new"), connectAction},
        {QStringLiteral("terminal.local"), localShellAction},
        {QStringLiteral("tab.new"), newTabAction},
        {QStringLiteral("tab.close"), closeTabAction},
        {QStringLiteral("files.toggle"), toggleFilesAction},
        {QStringLiteral("vault.lock"), lockVaultAction},
        {QStringLiteral("terminal.search"), searchTerminalAction},
        {QStringLiteral("terminal.export"), exportTerminalAction},
        {QStringLiteral("command.palette"), commandPaletteAction},
        {QStringLiteral("settings.open"), settingsAction},
    };
    applyConfiguredShortcuts();
    applyModernAppearance(m_settingsService->currentSettings());

    // 底部状态栏与标题栏消息条都已移除：服务器、编码和尺寸由每个 TerminalPane 自己的
    // 顶部状态栏持久展示，其余运行信息只进日志文件，不再抢占任何界面位置。

    connect(connectAction, &QAction::triggered, this, &MainWindow::connectCurrentPane);
    connect(nestedConnectAction, &QAction::triggered,
            this, &MainWindow::connectNestedFromCurrentPane);
    connect(localShellAction, &QAction::triggered, this, [this]() {
        TerminalWorkspace *workspace = activeWorkspace();
        TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
        if (pane != nullptr) {
            pane->openLocalShell();
        }
    });
    connect(newTabAction, &QAction::triggered, this, [this]() {
        TerminalWorkspace *workspace = createWorkspaceTab(
            tr("Terminal %1").arg(m_tabs->count() + 1));
        m_tabs->setCurrentWidget(workspace);
        workspace->activePane()->openLocalShell();
    });
    connect(closeTabAction, &QAction::triggered, this, [this]() {
        closeWorkspaceTab(m_tabs->currentIndex());
    });
    connect(renameTabAction, &QAction::triggered, this, [this]() {
        const int index = m_tabs->currentIndex();
        if (index < 0) {
            return;
        }
        bool accepted = false;
        const QString name = QInputDialog::getText(this,
                                                    tr("Rename tab"),
                                                    tr("Tag name:"),
                                                    QLineEdit::Normal,
                                                    m_tabs->tabText(index),
                                                    &accepted).trimmed();
        if (accepted && !name.isEmpty()) {
            setWorkspaceTabText(index, name);
        }
    });
    connect(saveWorkspaceAction, &QAction::triggered,
            this, &MainWindow::saveCurrentWorkspace);
    connect(openWorkspaceAction, &QAction::triggered,
            this, &MainWindow::openSavedWorkspace);
    connect(removeWorkspaceAction, &QAction::triggered,
            this, &MainWindow::removeSavedWorkspace);
    connect(m_titleTabBar,
            &QTabBar::tabCloseRequested,
            this,
            [this](const int index) { closeWorkspaceTab(index); });
    connect(m_titleTabBar,
            &QTabBar::currentChanged,
            this,
            [this](const int index) {
                if (index >= 0 && index < m_tabs->count()
                    && m_tabs->currentIndex() != index) {
                    m_tabs->setCurrentIndex(index);
                }
            });
    connect(m_titleTabBar,
            &QTabBar::tabMoved,
            this,
            [this](const int from, const int to) {
                if (from < 0 || to < 0 || from >= m_tabs->count() || to >= m_tabs->count()) {
                    return;
                }
                // 外部标题栏已经完成自身移动；同步移动 QTabWidget 的隐藏标签会由 Qt 原生
                // 逻辑连同页面一起重排，避免手动移除页面导致会话短暂失去父对象。
                m_tabs->tabBar()->moveTab(from, to);
                if (m_aiAssistantStack != nullptr
                    && from < m_aiAssistantStack->count()
                    && to < m_aiAssistantStack->count()) {
                    // QStackedWidget 没有 moveWidget API；先移除再按目标索引插入不会销毁页面，
                    // 与终端标签执行完全相同的移动后，两套索引仍一一对应。
                    QWidget *assistantPage = m_aiAssistantStack->widget(from);
                    m_aiAssistantStack->removeWidget(assistantPage);
                    m_aiAssistantStack->insertWidget(to, assistantPage);
                }
                m_tabs->setCurrentIndex(m_titleTabBar->currentIndex());
                if (m_aiAssistantStack != nullptr) {
                    m_aiAssistantStack->setCurrentIndex(m_tabs->currentIndex());
                }
            });
    connect(m_titleTabBar,
            &QWidget::customContextMenuRequested,
            this,
            &MainWindow::showSessionTabContextMenu);
    connect(m_tabs,
            &QTabWidget::currentChanged,
            this,
            [this](const int index) {
        m_workspace = activeWorkspace();
        // 标题栏是 QTabWidget 的镜像；工作区恢复和程序化切换都从这里回写索引，保证标签
        // 高亮始终指向实际接收键盘输入的终端。
        if (m_titleTabBar != nullptr && m_titleTabBar->currentIndex() != index) {
            m_titleTabBar->setCurrentIndex(index);
        }
        refreshProductivityTargets();
    });
    connect(toggleFilesAction, &QAction::triggered, this, [this]() {
        TerminalWorkspace *workspace = activeWorkspace();
        if (workspace != nullptr) {
            // 快捷键动作不再拥有独立勾选状态，直接反转当前会话；可见开关只保留在该会话
            // 顶部状态栏右侧，避免工具栏、菜单和实际文件区出现三份状态源。
            workspace->setFileManagerVisible(!workspace->isFileManagerVisible());
        }
    });
    connect(m_sidebarToggleAction,
            &QAction::toggled,
            this,
            &MainWindow::setSidebarVisible);
    connect(m_hostManager,
            &HostManagerWidget::hostConnectionRequested,
            this,
            &MainWindow::connectSavedHost);
    connect(m_hostManager,
            &HostManagerWidget::hostsChanged,
            m_portForwardManager,
            &PortForwardManagerWidget::refresh);
    connect(m_portForwardManager,
            &PortForwardManagerWidget::statusMessage,
            this,
            [this](const QString &message) { showTransientMessage(message, 8000); });
    connect(lockVaultAction, &QAction::triggered, this, [this]() {
        m_credentialVault->lock();
        showTransientMessage(tr("The credential vault is locked"), 5000);
    });
    connect(unlockVaultAction, &QAction::triggered, this, [this]() {
        if (ensureVaultUnlocked()) {
            showTransientMessage(tr("The credential vault is unlocked"), 5000);
        }
    });
    connect(sshKeyManagerAction, &QAction::triggered, this, [this]() {
        SshKeyManagerDialog dialog(this);
        dialog.exec();
    });
    connect(searchTerminalAction, &QAction::triggered,
            this, &MainWindow::searchCurrentTerminal);
    connect(exportTerminalAction, &QAction::triggered,
            this, &MainWindow::exportCurrentTerminal);
    connect(commandPaletteAction, &QAction::triggered,
            this, &MainWindow::openCommandPalette);
    connect(settingsAction, &QAction::triggered, this, [this]() {
        SettingsDialog dialog(m_settingsService, m_credentialVault, m_aiSettingsService, this);
        dialog.exec();
    });
    connect(m_settingsService,
            &TerminalSettingsService::settingsChanged,
            this,
            [this, unlockVaultAction, lockVaultAction](const TerminalSettings &settings) {
                applyLanguageSetting(settings.language);
                // 对已打开标签逐个应用，保证设置对话框的实时预览和系统主题切换不会只影响
                // 新建终端；动作快捷键同样以这一份快照为唯一来源。
                for (TerminalPane *pane : allTerminalPanes()) {
                    pane->applyTerminalSettings(settings);
                }
                applyConfiguredShortcuts();
                applyModernAppearance(settings);
                // 凭据保存策略与设置快照使用同一事实来源；实时同步只改变后续保存模式，已有
                // 凭据仍按各自元数据读取，因此取消设置窗口也能安全恢复原状态。
                m_credentialVault->setPasswordProtectionEnabled(
                    settings.vaultPasswordEnabled);
                // 关闭保险箱密码后透明本机模式无需也不能手工锁定/解锁；同步禁用菜单动作和
                // 快捷键，避免用户收到“已锁定”提示却仍能自动读取本机凭据的矛盾反馈。
                unlockVaultAction->setEnabled(settings.vaultPasswordEnabled);
                lockVaultAction->setEnabled(settings.vaultPasswordEnabled);
            });
    connect(m_credentialVault,
            &CredentialVault::vaultReset,
            this,
            [this]() {
                // 重置可能来自主机编辑器、连接前解锁或设置窗口里的嵌套密码框，不能要求每个
                // 调用点都各自同步设置服务。以保险箱信号为唯一收口，将运行态和 SQLite 开关
                // 统一为关闭；settingsChanged 随后会刷新锁定菜单与所有相关界面。
                TerminalSettings settings = m_settingsService->currentSettings();
                settings.vaultPasswordEnabled = false;
                QString settingsError;
                if (!m_settingsService->save(settings, &settingsError)) {
                    // 核心重置事务已经把数据库开关写为 0，此处失败只影响当前服务快照。仍先
                    // 修正运行态，避免后续普通设置保存把旧 true 写回，再用持久提示暴露异常。
                    m_settingsService->applyTransient(settings);
                    showTransientMessage(
                        tr("The vault was reset, but syncing the settings state failed: %1").arg(settingsError), 0);
                } else {
                    showTransientMessage(
                        tr("The vault has been reset; host configurations are kept. Re-enter the credentials you need."), 10000);
                }
                m_hostManager->refresh();
                refreshProductivityTargets();
            });

    connect(m_productivityWidget,
            &ProductivityWidget::statusMessage,
            this,
            [this](const QString &message) { showTransientMessage(message, 8000); });
    connect(m_productivityWidget,
            &ProductivityWidget::commandExecutionRequested,
            this,
            &MainWindow::executeOnTargets);
    connect(m_productivityWidget,
            &ProductivityWidget::broadcastInputRequested,
            this,
            [this](const QByteArray &bytes, const QStringList &targetIds) {
                sendBytesToTargets(bytes, targetIds);
            });
    connect(m_productivityWidget,
            &ProductivityWidget::broadcastModeChanged,
            this,
            [this](const bool enabled, const QStringList &targetIds) {
                m_broadcastEnabled = enabled && !targetIds.isEmpty();
                m_broadcastTargetIds = m_broadcastEnabled ? targetIds : QStringList{};
                showTransientMessage(
                    m_broadcastEnabled
                        ? tr("Live broadcast turned on: %1 targets").arg(m_broadcastTargetIds.size())
                        : tr("Live broadcast turned off"),
                    5000);
            });
    refreshProductivityTargets();

    // 默认使用 CredentialVault 的 15 分钟阈值。每 15 秒检查一次只决定锁定延迟上界，
    // 不持有敏感数据，也不会阻塞终端或 SFTP 工作线程。
    m_vaultIdleTimer->setInterval(15000);
    connect(m_vaultIdleTimer, &QTimer::timeout, this, [this]() {
        if (m_credentialVault->passwordProtectionEnabled()
            && m_credentialVault->lockIfIdle()) {
            showTransientMessage(tr("The credential vault was locked automatically due to inactivity"), 5000);
        }
    });
    m_vaultIdleTimer->start();
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow()
{
    // 显式移除可以保证析构期间后续控件事件不会回调一个已开始释放成员的 MainWindow。
    qApp->removeEventFilter(this);

    // 拖动后不足防抖间隔就退出应用时，定时器永远等不到超时。这里补写最后一次宽度，
    // 否则"拖完立刻关窗"这条最常见的路径恰好是唯一丢失设置的路径。
    if (m_sidebarWidthPersistTimer != nullptr && m_sidebarWidthPersistTimer->isActive()) {
        m_sidebarWidthPersistTimer->stop();
        persistSidebarWidth();
    }

    // 标签页是本窗口的子控件，直到基类 ~QWidget 阶段才被删除——那时本类的成员（连接历史
    // 哈希、数据库指针）已经析构完毕。面板此刻发出的 QObject::destroyed 仍会命中登记在
    // MainWindow 上的槽，槽再去访问已释放的容器，进程就在退出的最后一步崩溃。断开以本窗口
    // 为接收者的连接必须发生在成员失效之前，也就是这里。
    if (m_tabs != nullptr) {
        for (int index = 0; index < m_tabs->count(); ++index) {
            auto *workspace = qobject_cast<TerminalWorkspace *>(m_tabs->widget(index));
            if (workspace == nullptr) {
                continue;
            }
            if (TerminalPane *pane = workspace->activePane(); pane != nullptr) {
                QObject::disconnect(pane, nullptr, this, nullptr);
            }
            QObject::disconnect(workspace, nullptr, this, nullptr);
        }
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
#ifndef Q_OS_MACOS
    // 应用级过滤器能在事件送达菜单、标签、分隔器或终端等任意子控件前检查窗口边缘，
    // 因而无需在每个控件里重复实现无边框缩放。只处理当前主窗口的控件，弹窗和菜单窗口
    // 拥有自己的顶层 window()，不会被这里错误接管。
    auto *eventWidget = qobject_cast<QWidget *>(watched);
    const bool belongsToThisWindow = eventWidget != nullptr && eventWidget->window() == this;
    if (belongsToThisWindow && event->type() == QEvent::MouseMove) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const Qt::Edges edges = resizeEdgesAt(this, mouseEvent->globalPosition());
        if (mouseEvent->buttons() == Qt::NoButton) {
            edges == Qt::Edges{} ? unsetCursor() : setCursor(resizeCursorForEdges(edges));
        }
    } else if (belongsToThisWindow && event->type() == QEvent::MouseButtonPress) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            const Qt::Edges edges = resizeEdgesAt(this, mouseEvent->globalPosition());
            if (edges != Qt::Edges{} && windowHandle() != nullptr
                && windowHandle()->startSystemResize(edges)) {
                // 系统已接管后必须消费原始按下事件，否则边缘下方的按钮、标签或分隔器还会
                // 同时开始自己的操作，造成一次拖动触发两套状态机。
                mouseEvent->accept();
                m_credentialVault->recordActivity();
                return true;
            }
        }
    }
#else
    Q_UNUSED(watched)
#endif

    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
        m_credentialVault->recordActivity();
        break;
    default:
        break;
    }
    return false;
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (!m_macWindowChromeConfigured) {
        // enableMacWindowChrome 在非 macOS 平台是无操作实现；在 macOS 原生句柄尚未附着时
        // 返回 false，保留下次 showEvent 重试机会，避免用定时器轮询窗口系统状态。
        m_macWindowChromeConfigured = enableMacWindowChrome(this);
    }
    if (m_macWindowChromeConfigured) {
        alignMacWindowButtons(this, kWindowChromeHeight);
        QTimer::singleShot(0, this, [this]() {
            // showEvent 返回后 AppKit 仍可能执行一次标准标题栏布局。下一轮事件循环再次对齐
            // 可消除首次启动时按钮先居中、随后又跳回默认短标题栏位置的竞态。
            alignMacWindowButtons(this, kWindowChromeHeight);
        });
    }
}

void MainWindow::registerTranslatable(QAction *action,
                                     const char *sourceText,
                                     const char *sourceToolTip)
{
    if (action == nullptr) {
        return;
    }
    m_translatableUi.append({action, nullptr, sourceText, sourceToolTip});
    action->setText(QCoreApplication::translate("MainWindow", sourceText));
    if (sourceToolTip != nullptr) {
        action->setToolTip(QCoreApplication::translate("MainWindow", sourceToolTip));
    }
}

void MainWindow::registerTranslatable(QMenu *menu, const char *sourceTitle)
{
    if (menu == nullptr) {
        return;
    }
    m_translatableUi.append({nullptr, menu, sourceTitle, nullptr});
    menu->setTitle(QCoreApplication::translate("MainWindow", sourceTitle));
}

void MainWindow::retranslateUi()
{
    for (const TranslatableUiEntry &entry : std::as_const(m_translatableUi)) {
        if (entry.menu != nullptr) {
            entry.menu->setTitle(QCoreApplication::translate("MainWindow", entry.text));
            continue;
        }
        if (entry.action == nullptr) {
            continue;
        }
        entry.action->setText(QCoreApplication::translate("MainWindow", entry.text));
        if (entry.toolTip != nullptr) {
            entry.action->setToolTip(
                QCoreApplication::translate("MainWindow", entry.toolTip));
        }
    }
    setWindowTitle(tr("Shellium"));

    if (m_sidebarTitle != nullptr) {
        m_sidebarTitle->setText(tr("Hosts"));
    }
    if (m_hostCountLabel != nullptr && m_hostManager != nullptr) {
        m_hostCountLabel->setText(tr("%1 hosts").arg(m_hostManager->hostCount()));
    }
    if (auto *hint = findChild<QLabel *>(QStringLiteral("WorkspacePlaceholderHint"))) {
        hint->setText(tr("Press + in the title bar to open a new terminal tab, or connect "
                         "to a server from the host list on the left."));
    }
}

void MainWindow::applyLanguageSetting(const QString &storedLanguage)
{
    if (m_localization == nullptr) {
        return;
    }
    const QString resolved = m_localization->resolve(storedLanguage);
    if (resolved == m_localization->currentLanguage()) {
        return;
    }
    if (!m_localization->apply(resolved)) {
        showTransientMessage(tr("Could not switch to the selected language; check that the language pack is complete"), 8000);
        return;
    }
    // 界面已随 QEvent::LanguageChange 就地切换。已经滚动过的终端输出和历史日志沿用原文，
    // 属于内容而非界面文案，不再提示重启。
    showTransientMessage(tr("Interface language changed"), 4000);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);

    if (event->type() == QEvent::LanguageChange) {
        // 安装/卸载翻译器后 Qt 会向本窗口投递该事件。菜单栏常驻，必须就地重译；按需对话框
        // 不在此处理，它们下次打开会以新语言重新构造。
        retranslateUi();
        return;
    }

    if (event->type() != QEvent::WindowStateChange) {
        return;
    }

    // Windows/Linux 的最大化按钮必须随所有窗口状态入口更新，而不是只响应按钮自己的点击。
    refreshWindowMaximizeIcon();
    if (!m_macWindowChromeConfigured) {
        return;
    }

    QTimer::singleShot(0, this, [this]() {
        // 最大化、恢复和全屏转换结束后，AppKit 才能给出最终标题按钮容器坐标。
        alignMacWindowButtons(this, kWindowChromeHeight);
    });
}

void MainWindow::refreshWindowMaximizeIcon()
{
    if (m_windowMaximizeButton == nullptr) {
        return;
    }

    const bool restoreMode = isMaximized() || isFullScreen();
    // Windows/Linux 的按钮使用本文件内自绘控件；dynamic_cast 同时保护未来可能替换为普通
    // QToolButton 的平台实现，此时文字提示仍会正确更新，不会因类型假设导致崩溃。
    if (auto *controlButton = dynamic_cast<WindowControlButton *>(m_windowMaximizeButton)) {
        controlButton->setRestoreMode(restoreMode);
    }
    m_windowMaximizeButton->setToolTip(restoreMode ? tr("Restore") : tr("Maximize"));
    m_windowMaximizeButton->setAccessibleName(
        restoreMode ? tr("Restore window") : tr("Maximize window"));
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (m_macWindowChromeConfigured) {
        // 对齐函数只调整三个按钮的纵坐标且具有幂等性，可安全覆盖原生 zoom 动画产生的
        // 连续 resizeEvent；这也避免等待动画结束时按钮短暂贴在标题栏上沿。
        alignMacWindowButtons(this, kWindowChromeHeight);
    }
}

void MainWindow::closeWorkspaceTab(const int index, const bool allowLast)
{
    if (index < 0 || index >= m_tabs->count()) {
        return;
    }
    Q_UNUSED(allowLast);

    QWidget *page = m_tabs->widget(index);
    QWidget *assistantPage = m_aiAssistantStack != nullptr
        && index < m_aiAssistantStack->count()
        ? m_aiAssistantStack->widget(index) : nullptr;
    if (m_titleTabBar != nullptr && index < m_titleTabBar->count()) {
        // 删除镜像标签时临时阻断 currentChanged；随后 QTabWidget 会选出真实的新当前页面，
        // 它的 currentChanged 回调再统一更新标题栏，避免中间索引落到另一个会话。
        const QSignalBlocker blocker(m_titleTabBar);
        m_titleTabBar->removeTab(index);
    }
    m_tabs->removeTab(index);
    if (assistantPage != nullptr) {
        // AI 页面与终端页面共享同一索引生命周期；关闭标签后立即从可见栈移除，再延迟析构
        // 其 AiClient 和历史记录，确保新当前索引不会短暂显示已关闭标签的会话。
        m_aiAssistantStack->removeWidget(assistantPage);
        assistantPage->deleteLater();
    }
    if (m_titleTabBar != nullptr) {
        m_titleTabBar->setCurrentIndex(m_tabs->currentIndex());
    }
    if (m_aiAssistantStack != nullptr) {
        m_aiAssistantStack->setCurrentIndex(m_tabs->currentIndex());
    }

    // 会话清理可能等待 SSH/SFTP/转发线程；deleteLater 先让当前菜单或快捷键回调返回，页面
    // 又已从两个标签模型移除，不会在清理期间继续接收新的用户操作。
    page->deleteLater();
    updateWorkspacePlaceholder();
}

void MainWindow::setWorkspaceTabText(const int index, const QString &title)
{
    if (index < 0 || index >= m_tabs->count() || title.trimmed().isEmpty()) {
        return;
    }
    m_tabs->setTabText(index, title);
    if (m_titleTabBar != nullptr && index < m_titleTabBar->count()) {
        m_titleTabBar->setTabText(index, title);
        m_titleTabBar->setTabToolTip(index, title);
    }
}

void MainWindow::restoreSidebarWidth()
{
    int sidebarWidth = kSidebarDefaultWidth;
    QString error;
    if (const std::optional<int> storedWidth =
            m_database->intSetting(QString::fromLatin1(kSidebarWidthSettingKey), &error);
        storedWidth.has_value()) {
        sidebarWidth = qBound(kSidebarMinimumWidth, *storedWidth, kSidebarMaximumWidth);
    } else if (!error.isEmpty()) {
        // 读取失败与"用户从未拖动过"必须区分：前者说明磁盘上的值不可信，提示一次比静默
        // 套用默认宽度更诚实，用户至少知道这次布局没有按上次的来。
        showTransientMessage(tr("Failed to read the sidebar width: %1").arg(error), 10000);
    }

    // 第二项只表达"剩余空间归终端"，QSplitter 会按窗口实际宽度整体缩放这组比例。
    m_sidebarSplitter->setSizes({sidebarWidth, 1000});
}

void MainWindow::persistSidebarWidth()
{
    if (m_sidebarSplitter == nullptr
        || m_sidebarPanel == nullptr
        || !m_sidebarPanel->isVisible()) {
        return;
    }

    const QList<int> sizes = m_sidebarSplitter->sizes();
    if (sizes.isEmpty()) {
        return;
    }
    const int sidebarWidth = sizes.constFirst();
    if (sidebarWidth < kSidebarMinimumWidth) {
        // 收起动画或窗口过窄导致的临时挤压不代表用户意图，保存它会让下次启动的侧栏
        // 停在一个用户没选过的宽度上。
        return;
    }

    QString error;
    if (!m_database->setIntSetting(QString::fromLatin1(kSidebarWidthSettingKey),
                                   qMin(sidebarWidth, kSidebarMaximumWidth),
                                   &error)) {
        showTransientMessage(tr("Failed to save sidebar width: %1").arg(error), 10000);
    }
}

void MainWindow::setSidebarVisible(const bool visible)
{
    if (m_sidebarPanel != nullptr) {
        m_sidebarPanel->setVisible(visible);
    }

    if (m_sidebarToggleAction != nullptr
        && m_sidebarToggleAction->isChecked() != visible) {
        const QSignalBlocker blocker(m_sidebarToggleAction);
        m_sidebarToggleAction->setChecked(visible);
    }
}

void MainWindow::showSidebarPage(const int pageIndex)
{
    if (m_sidebarStack == nullptr
        || pageIndex < 0
        || pageIndex >= m_sidebarStack->count()) {
        return;
    }

    static const QStringList pageTitles = {
        tr("Hosts"), tr("Productivity"), tr("Port forwarding"), tr("AI Assistant")};
    m_activeSidebarPage = pageIndex;
    m_sidebarStack->setCurrentIndex(pageIndex);
    if (m_sidebarTitle != nullptr) {
        m_sidebarTitle->setText(pageTitles.value(pageIndex));
    }
    if (m_hostCountLabel != nullptr) {
        // 数量摘要只解释“主机”这一页；放在效率工具或端口转发标题后会被误读成相应页面的
        // 条目数量，因此切页时与主机动作条使用同一可见性条件。
        m_hostCountLabel->setVisible(pageIndex == HostsPage);
    }
    if (m_hostHeaderActions != nullptr) {
        m_hostHeaderActions->setVisible(pageIndex == HostsPage);
    }

    // 四个侧栏页面共用同一条标题栏和外边距——AI 助手不再自带独立顶栏，题图要求的正是
    // "顶部标题栏与主机侧边栏风格保持一致"，与其在页面内部复刻一份相近的样式，不如直接
    // 复用同一个 SidebarHeader 标签，保证像素级一致且未来主题调整只需改一处。

    // QButtonGroup 已设置互斥，但菜单触发并不会自动点击按钮，因此这里显式同步选中态。
    // 使用信号阻断避免 setChecked 再次触发页面切换，保持状态流只有一个入口。
    for (int index = 0; index < m_navigationButtons.size(); ++index) {
        QToolButton *button = m_navigationButtons.at(index);
        if (button != nullptr && button->isChecked() != (index == pageIndex)) {
            const QSignalBlocker blocker(button);
            button->setChecked(index == pageIndex);
        }
    }
    setSidebarVisible(true);
}

void MainWindow::applyModernAppearance(const TerminalSettings &settings)
{
    const bool dark = settings.effectiveThemeMode() == ThemeMode::Dark;

    // 标题区保持稳定深色，可以在深浅主题间提供连续的窗口身份；工作区颜色则跟随用户
    // 主题。所有颜色在此集中声明，避免各子控件出现大量“近似但不一致”的硬编码灰色。
    const QString workspaceBackground = dark ? QStringLiteral("#101525")
                                             : QStringLiteral("#edf1f4");
    const QString navigationBackground = dark ? QStringLiteral("#151a2b")
                                              : QStringLiteral("#e8edf0");
    const QString sidebarBackground = dark ? QStringLiteral("#191f31")
                                           : QStringLiteral("#f5f7f8");
    const QString surfaceBackground = dark ? QStringLiteral("#21283b")
                                           : QStringLiteral("#ffffff");
    const QString raisedBackground = dark ? QStringLiteral("#293147")
                                          : QStringLiteral("#f0f3f5");
    // 主机树位于接近白色的侧栏上，通用 RAISED 只比背景深几个灰阶，鼠标移入时几乎不可见。
    // 为高频主机选择单独提供更深的悬停色，避免加深全局按钮、标签页和其他列表的悬停状态。
    const QString hostTreeHoverBackground = dark ? QStringLiteral("#30394f")
                                                 : QStringLiteral("#e1e7eb");
    const QString borderColor = dark ? QStringLiteral("#30384f")
                                     : QStringLiteral("#d9e0e4");
    const QString textColor = dark ? QStringLiteral("#f0f3fa")
                                   : QStringLiteral("#1b2230");
    const QString mutedTextColor = dark ? QStringLiteral("#929bb3")
                                        : QStringLiteral("#71808d");
    const QString accentColor = QStringLiteral("#2bd981");
    const QString dangerColor = dark ? QStringLiteral("#ef776e")
                                     : QStringLiteral("#d84a41");
    const QString dangerTintColor = dark ? QStringLiteral("#3a1f22")
                                         : QStringLiteral("#fbe7e5");
    const QString warningColor = dark ? QStringLiteral("#e2a94f")
                                      : QStringLiteral("#b8791f");
    const QString warningTintColor = dark ? QStringLiteral("#3a2f18")
                                          : QStringLiteral("#faefdc");
    const QString selectionBackground = dark ? QStringLiteral("#24413d")
                                             : QStringLiteral("#dff6ec");
    // 导航入口、主机和远程文件都是用户持续关注的“当前对象”，需要比文本选区、按钮按下态
    // 更明确的视觉反馈。单独提供高对比选中色，可以加深这三处而不让全局交互控件显得过重。
    const QString prominentSelectionBackground = dark ? QStringLiteral("#2f6256")
                                                       : QStringLiteral("#b7e6d0");

    QString styleSheet = QStringLiteral(R"QSS(
        QWidget#ShelliumRoot,
        QWidget#WorkspaceBody {
            background: @WORKSPACE@;
            color: @TEXT@;
        }
        QWidget#WindowChrome {
            background: #292d42;
            border: 0;
        }
        QLabel#WindowProductName {
            color: #f8f9fd;
            background: transparent;
            border: 0;
            padding: 0 10px 0 4px;
            font-size: 15px;
            font-weight: 700;
        }
        QToolButton[windowControl="true"] {
            background: transparent;
            border: 0;
            border-radius: 7px;
            padding: 0;
        }
        QToolButton[windowControl="true"]:hover {
            background: #383d55;
        }
        QToolButton[windowControl="true"]:pressed {
            background: #454b65;
        }
        QToolButton[windowControlRole="close"]:hover {
            background: #c42b1c;
        }
        QToolButton[windowControlRole="close"]:pressed {
            background: #a52216;
        }
        QWidget#NavigationRail {
            background: @NAVIGATION@;
            border-right: 1px solid @BORDER@;
        }
        QWidget#SidebarPanel {
            background: @SIDEBAR@;
            border: 0;
        }
        QSplitter#SidebarSplitter::handle:horizontal {
            background: transparent;
            border-left: 1px solid @BORDER@;
        }
        QSplitter#SidebarSplitter::handle:horizontal:hover,
        QSplitter#SidebarSplitter::handle:horizontal:pressed {
            background: @SELECTION@;
            border-left: 1px solid @ACCENT@;
        }
        QLabel#SidebarHeader {
            color: @TEXT@;
            font-size: 18px;
            font-weight: 400;
            padding: 2px 0 7px 0;
        }
        QLabel#HostCountLabel {
            color: @MUTED@;
            font-size: 13px;
            font-weight: 600;
            padding: 3px 0 7px 0;
        }
        QFrame#HostHeaderActionDivider {
            color: @BORDER@;
            background: @BORDER@;
            border: 0;
        }
        QToolButton[chromeButton="true"] {
            background: transparent;
            border: 0;
            border-radius: 9px;
            padding: 7px;
        }
        QToolButton[chromeButton="true"]:hover {
            background: rgba(255, 255, 255, 22);
        }
        QToolButton[chromeButton="true"]:pressed {
            background: rgba(255, 255, 255, 34);
        }
        QToolButton[chromeRole="scrollLeft"]:hover,
        QToolButton[chromeRole="scrollRight"]:hover {
            background: #3a4260;
        }
        QToolButton[chromeRole="scrollLeft"]:pressed,
        QToolButton[chromeRole="scrollRight"]:pressed {
            background: #464f70;
        }
        QToolButton[chromeRole="scrollLeft"]:disabled,
        QToolButton[chromeRole="scrollRight"]:disabled {
            background: transparent;
        }
        QToolButton[navButton="true"] {
            background: transparent;
            border: 0;
            border-radius: 12px;
            padding: 10px;
        }
        QToolButton[navButton="true"]:hover {
            background: @RAISED@;
        }
        QToolButton[navButton="true"]:checked {
            background: @PROMINENT_SELECTION@;
        }
        QToolButton[compactIconButton="true"] {
            color: @TEXT@;
            background: transparent;
            border: 0;
            border-radius: 8px;
            padding: 7px;
        }
        QToolButton[compactIconButton="true"]:hover {
            background: @RAISED@;
        }
        QToolButton[compactIconButton="true"]:pressed {
            background: @SELECTION@;
        }
        QToolButton[compactIconButton="true"]:disabled {
            color: @MUTED@;
            background: transparent;
        }
        QTabBar#SessionTitleTabs {
            background: transparent;
            border: 0;
        }
        QTabWidget#TerminalPages::pane,
        QTabWidget#productivityTabs::pane,
        QStackedWidget#SidebarStack,
        QStackedWidget#WorkspaceStack,
        QWidget#WorkspacePlaceholder {
            border: 0;
            background: transparent;
        }
        QLabel#WorkspacePlaceholderTitle {
            color: @TEXT@;
            font-size: 30px;
            font-weight: 600;
        }
        QLabel#WorkspacePlaceholderHint {
            color: @MUTED@;
            font-size: 13px;
        }
        QFrame#HostSearchBar {
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 10px;
            min-height: 34px;
            max-height: 34px;
        }
        QLineEdit#HostSearchEdit,
        QLineEdit#HostSearchEdit:focus {
            color: @TEXT@;
            placeholder-text-color: @MUTED@;
            background: transparent;
            border: 0;
            border-radius: 0;
            min-height: 28px;
            padding: 2px 3px;
        }
        QFrame#HostTagSuggestionsPopup {
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 10px;
        }
        QToolButton#HostTagFilterButton {
            color: @MUTED@;
            background: @RAISED@;
            border: 0;
            border-radius: 8px;
            min-height: 24px;
            max-height: 24px;
            padding: 0 7px;
            font-size: 12px;
            font-weight: 600;
        }
        QToolButton#HostTagFilterButton:hover {
            color: @TEXT@;
            background: @SELECTION@;
        }
        QToolButton#HostTagFilterButton:pressed {
            color: @TEXT@;
            background: @PROMINENT_SELECTION@;
        }
        QToolButton#hostRecordTagButton {
            color: @MUTED@;
            background: @RAISED@;
            border: 0;
            border-radius: 8px;
            min-height: 24px;
            max-height: 24px;
            padding: 0 7px;
            font-size: 12px;
            font-weight: 600;
        }
        QToolButton#hostRecordTagButton:hover {
            color: @TEXT@;
            background: @SELECTION@;
        }
        QToolButton#hostRecordTagButton:pressed {
            color: @TEXT@;
            background: @PROMINENT_SELECTION@;
        }
        QToolButton#hostTagChip {
            color: @MUTED@;
            background: @RAISED@;
            border: 0;
            border-radius: 8px;
            min-height: 24px;
            max-height: 24px;
            padding: 0 8px;
            font-size: 12px;
            font-weight: 600;
        }
        QToolButton#hostTagChip:hover {
            color: @TEXT@;
            background: @SELECTION@;
        }
        QToolButton#hostTagChip:checked {
            color: white;
            background: @ACCENT@;
        }
        /* 远程文件管理器由顶栏、文件区、操作栏、传输信息带四段连续组成。选择器只匹配
           SFTP 命名控件，避免为了参考图的紧凑尺寸改变设置页或主机侧栏的通用控件。 */
        QWidget#RemoteFileManager {
            color: @TEXT@;
            background: @SURFACE@;
        }
        QWidget#SftpTopBar {
            background: @SIDEBAR@;
            border-bottom: 1px solid @BORDER@;
        }
        QLabel#SftpServerLabel {
            color: @MUTED@;
            font-size: 13px;
            font-weight: 700;
        }
        QComboBox#SftpTargetCombo {
            background: @SURFACE@;
            min-height: 32px;
        }
        QToolButton[sftpFullScreenAction="true"],
        QToolButton[sftpPaneAction="true"] {
            color: @MUTED@;
            background: transparent;
            border: 0;
            border-radius: 7px;
            padding: 5px;
        }
        QToolButton[sftpFullScreenAction="true"]:hover,
        QToolButton[sftpPaneAction="true"]:hover {
            color: @TEXT@;
            background: @RAISED@;
        }
        QWidget#SftpLocalPane,
        QWidget#SftpRemotePane {
            background: @SURFACE@;
        }
        QSplitter#SftpPaneSplitter::handle:horizontal {
            background: @BORDER@;
        }
        QWidget#SftpLocalToolbar,
        QWidget#SftpRemoteToolbar {
            background: @SURFACE@;
            border-bottom: 1px solid @BORDER@;
        }
    )QSS");

    // MSVC 对单个字符串字面量的长度有额外限制。完整应用样式表超过该上限，因此必须在
    // 完整 QSS 规则边界处拆段并在运行时追加；这样既不会截断字符串，也不会改变选择器顺序、
    // 空白语义或后续颜色占位符替换结果。各片段均控制在 8 KiB 左右，为后续扩展保留余量。
    styleSheet += QStringLiteral(R"QSS(
        QLabel#SftpPaneLabel {
            color: @MUTED@;
            font-size: 12px;
            font-weight: 700;
            padding: 0 2px 0 0;
        }
        QLineEdit#SftpLocalPathEdit,
        QLineEdit#SftpRemotePathEdit {
            color: @MUTED@;
            background: @SIDEBAR@;
            border: 1px solid @BORDER@;
            border-radius: 7px;
            min-height: 26px;
            padding: 1px 8px;
            font-family: "SF Mono", "Menlo", monospace;
            font-size: 12px;
        }
        QLineEdit#SftpLocalPathEdit:focus,
        QLineEdit#SftpRemotePathEdit:focus {
            color: @TEXT@;
            border-color: @ACCENT@;
        }
        QTreeView#LocalFileTree,
        QTreeView#RemoteFileTree {
            background: @SURFACE@;
            /* 边框始终占位为 2px 透明，拖拽悬停时只切换颜色而不改变控件尺寸，
               否则周围的分栏、滚动条会在拖拽进入/离开时跳动一次。 */
            border: 2px solid transparent;
        }
        QTreeView#LocalFileTree[dragActive="true"],
        QTreeView#RemoteFileTree[dragActive="true"] {
            border: 2px dashed @ACCENT@;
            background: @RAISED@;
        }
        QTreeView#LocalFileTree::item,
        QTreeView#RemoteFileTree::item {
            min-height: 30px;
            border-bottom: 1px solid @BORDER@;
            border-radius: 0;
            padding: 2px 6px;
        }
        QTreeView#LocalFileTree::item:hover,
        QTreeView#RemoteFileTree::item:hover {
            background: @RAISED@;
        }
        QTreeView#LocalFileTree QHeaderView::section,
        QTreeView#RemoteFileTree QHeaderView::section {
            color: @MUTED@;
            background: @RAISED@;
            border: 0;
            border-bottom: 1px solid @BORDER@;
            padding: 7px 10px;
            font-size: 12px;
            font-weight: 700;
        }
        QFrame#SftpOperationBar {
            background: @SIDEBAR@;
            border-top: 1px solid @BORDER@;
            border-bottom: 1px solid @BORDER@;
        }
        QFrame#SftpOperationSeparator {
            color: @BORDER@;
            background: @BORDER@;
            border: 0;
        }
        QToolButton[sftpResponsiveAction="true"] {
            color: @TEXT@;
            background: transparent;
            border: 1px solid transparent;
            border-radius: 8px;
            padding: 5px 10px;
            font-size: 13px;
            font-weight: 600;
        }
        QToolButton[sftpActionRole="transfer"] {
            background: @SURFACE@;
            border-color: @BORDER@;
        }
        QToolButton[sftpResponsiveAction="true"]:hover {
            background: @RAISED@;
            border-color: @BORDER@;
        }
        QToolButton[sftpActionRole="danger"] {
            color: @DANGER@;
        }
        QToolButton[sftpActionRole="danger"]:hover {
            color: @DANGER@;
            background: @DANGER_TINT@;
            border-color: transparent;
        }
        QCheckBox#SftpResumeCheck {
            color: @MUTED@;
            spacing: 7px;
            font-size: 12px;
        }
        QFrame#SftpTransferBand {
            background: @SIDEBAR@;
            border-top: 1px solid @BORDER@;
        }
        QProgressBar#SftpTransferProgress {
            background: @RAISED@;
            border: 0;
            border-radius: 3px;
            min-height: 6px;
            max-height: 6px;
        }
        QProgressBar#SftpTransferProgress::chunk {
            background: @ACCENT@;
            border-radius: 3px;
        }
        QLabel#SftpTransferStatus,
        QLabel#SftpTransferDetail {
            color: @MUTED@;
            font-size: 12px;
        }
        QListWidget#SftpTransferQueue {
            color: @TEXT@;
            background: transparent;
            border: 0;
            outline: 0;
        }
        QListWidget#SftpTransferQueue::item {
            min-height: 30px;
            border: 0;
            border-radius: 7px;
            padding: 0;
        }
        QListWidget#SftpTransferQueue::item:selected {
            background: @SELECTION@;
        }
        QLabel#SftpQueueItemName {
            color: @TEXT@;
            background: transparent;
            border: 0;
            font-size: 12px;
        }
        QLabel#SftpQueueStateBadge {
            border: 0;
            border-radius: 5px;
            padding: 2px 7px;
            font-size: 11px;
            font-weight: 700;
        }
        QLabel#SftpQueueStateBadge[queueState="active"] {
            color: @ACCENT@;
            background: @SELECTION@;
        }
        QLabel#SftpQueueStateBadge[queueState="queued"] {
            color: @MUTED@;
            background: @RAISED@;
        }
        QLabel#SftpQueueStateBadge[queueState="paused"] {
            color: @WARNING@;
            background: @WARNING_TINT@;
        }
        QTabWidget QTabBar::tab {
            color: @MUTED@;
            background: transparent;
            border: 0;
            border-radius: 8px;
            min-height: 26px;
            padding: 5px 11px;
            margin: 1px 2px;
        }
        QTabWidget QTabBar::tab:selected {
            color: @TEXT@;
            background: @RAISED@;
        }
        QTabWidget QTabBar::tab:hover:!selected {
            color: @TEXT@;
            background: @SURFACE@;
        }
        QLineEdit,
        QTextEdit,
        QPlainTextEdit,
        QComboBox,
        QAbstractSpinBox {
            color: @TEXT@;
            background: @SURFACE@;
            selection-color: @TEXT@;
            selection-background-color: @SELECTION@;
            border: 1px solid @BORDER@;
            border-radius: 8px;
            min-height: 28px;
            padding: 3px 8px;
        }
        QLineEdit:focus,
        QTextEdit:focus,
        QPlainTextEdit:focus,
        QComboBox:focus,
        QAbstractSpinBox:focus {
            border: 1px solid @ACCENT@;
        }
        QComboBox::drop-down {
            border: 0;
            width: 26px;
            subcontrol-origin: padding;
            subcontrol-position: center right;
        }
        QComboBox::down-arrow {
            image: url(@COMBO_ARROW@);
            width: 12px;
            height: 8px;
            margin-right: 10px;
        }
        QComboBox QAbstractItemView {
            color: @TEXT@;
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 10px;
            outline: 0;
            padding: 6px;
            selection-color: @TEXT@;
            selection-background-color: @SELECTION@;
        }
        QComboBox QAbstractItemView::item {
            min-height: 30px;
            border: 0;
            border-radius: 7px;
            padding: 3px 8px;
        }
        QComboBox QAbstractItemView::item:hover {
            background: @RAISED@;
        }
        QComboBox#PortForwardHostCombo {
            /* 端口转发主机是侧栏中的主要上下文选择器，使用比通用表单边框更清楚的弱化
               文字色，避免白色背景下边界过浅而像一个无边框文本区域。 */
            border: 1px solid @MUTED@;
            padding-right: 30px;
        }
    )QSS");

    // 继续按完整规则追加第二个中间片段，避免相邻 C++ 字面量在编译阶段被重新合并成一个
    // 超长常量；显式 QString 追加会让分段发生在运行时，因而能够可靠绕过 C2026。
    styleSheet += QStringLiteral(R"QSS(
        QComboBox#PortForwardHostCombo:focus {
            border: 1px solid @ACCENT@;
        }
        QFrame#PortForwardHostPopup {
            color: @TEXT@;
            background-color: @SURFACE@;
            border: 1px solid @MUTED@;
            border-radius: 10px;
        }
        QTreeWidget#PortForwardHostTree {
            color: @TEXT@;
            /* 必须显式给树和 viewport 实色背景；仅给顶层 QFrame 设置背景时，macOS 的
               QAbstractScrollArea viewport 仍可能使用透明画刷，导致主界面文字透入弹层。 */
            background-color: @SURFACE@;
            border: 0;
            outline: 0;
            padding: 6px;
            font-weight: 400;
            selection-color: @TEXT@;
            selection-background-color: transparent;
        }
        QTreeWidget#PortForwardHostTree QWidget {
            background-color: @SURFACE@;
        }
        QTreeWidget#PortForwardHostTree::item {
            min-height: 0;
            border: 0;
            border-radius: 0;
            padding: 0;
        }
        QTreeWidget#PortForwardHostTree::item:hover,
        QTreeWidget#PortForwardHostTree::item:selected,
        QTreeWidget#PortForwardHostTree::item:selected:active,
        QTreeWidget#PortForwardHostTree::item:selected:!active,
        QTreeWidget#PortForwardHostTree::item:selected:hover {
            /* 端口转发主机树复用主机管理的委托式交互背景。平台 QSS 保持透明，避免给缩进
               区和实际节点区铺出一整条直角选中色。 */
            background: transparent;
        }
        QLabel#PortForwardHostLabel {
            font-weight: 700;
        }
        QPushButton[forwardingRuntimeAction="true"] {
            /* 启停与“新建”同处一行，缩短左右内边距才能在 260px 最小侧栏内保持完整文字，
               固定宽度由构造代码统一约束，避免不同平台字体导致两个按钮宽度跳变。 */
            padding-left: 6px;
            padding-right: 6px;
        }
        QListWidget#PortForwardRuleList {
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 10px;
        }
        QListWidget#PortForwardRuleList::item {
            border-radius: 0;
            border-bottom: 1px solid @BORDER@;
            padding: 0;
        }
        QListWidget#PortForwardRuleList::item:selected,
        QListWidget#PortForwardRuleList::item:selected:active,
        QListWidget#PortForwardRuleList::item:selected:!active {
            background: @SELECTION@;
        }
        QLabel#PortForwardSummary {
            color: @MUTED@;
            background: @RAISED@;
            border: 1px solid @BORDER@;
            border-top: 0;
            border-bottom-left-radius: 10px;
            border-bottom-right-radius: 10px;
            padding: 7px 9px;
            font-size: 11px;
        }
        QLabel#PortForwardSecurityNotice {
            color: @WARNING@;
        }
        QPlainTextEdit#TerminalView {
            color: palette(text);
            background: palette(base);
            selection-color: palette(highlighted-text);
            selection-background-color: palette(highlight);
            border: 0;
            border-radius: 0;
            padding: 6px;
        }
        QPushButton {
            color: @TEXT@;
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 8px;
            min-height: 28px;
            padding: 4px 11px;
        }
        QPushButton:hover {
            background: @RAISED@;
            border-color: @MUTED@;
        }
        QPushButton:pressed,
        QPushButton:checked {
            background: @SELECTION@;
            border-color: @ACCENT@;
        }
        QPushButton:disabled {
            color: @MUTED@;
            background: @SIDEBAR@;
        }
        QPushButton[primaryAction="true"] {
            color: #0c2f24;
            background: @ACCENT@;
            border-color: @ACCENT@;
            font-weight: 700;
        }
        QPushButton[primaryAction="true"]:hover {
            background: #45e394;
            border-color: #45e394;
        }
        QPushButton[destructiveAction="true"] {
            color: #ffffff;
            background: @DANGER@;
            border-color: @DANGER@;
            font-weight: 700;
        }
        QPushButton[destructiveAction="true"]:hover {
            background: @DANGER@;
            border-color: @DANGER@;
        }
        QPushButton[destructiveAction="true"]:disabled {
            color: @MUTED@;
            background: @SIDEBAR@;
            border-color: @BORDER@;
        }
        QPushButton[hostPrimaryAction="true"] {
            /* 固定按钮总高为 30px；内容高 20px + 上下内边距 6px + 边框 2px 留有余量，
               不再因通用按钮的 28px 最小内容高与固定高度冲突而裁掉下圆角。 */
            min-height: 20px;
            max-height: 20px;
            border-radius: 8px;
            padding: 3px 10px;
            font-size: 13px;
        }
        QPushButton[compactAction="true"] {
            /* Snippet 列表行内的"执行/复制"按钮：行高有限，需要比通用按钮更矮更窄，
               同时保留一致的圆角和边框视觉。 */
            min-height: 22px;
            max-height: 22px;
            padding: 2px 9px;
            font-size: 12px;
            border-radius: 6px;
        }
        QLabel[sectionCaption="true"] {
            color: @MUTED@;
            font-size: 12px;
            font-weight: 700;
            padding: 3px 1px;
        }
        QTreeView,
        QListView,
        QTableView {
            color: @TEXT@;
            background: transparent;
            alternate-background-color: @SURFACE@;
            border: 0;
            outline: 0;
            selection-color: @TEXT@;
            selection-background-color: @SELECTION@;
        }
        QTreeView::item,
        QListView::item,
        QTableView::item {
            min-height: 28px;
            border: 0;
            border-radius: 7px;
            padding: 3px 5px;
        }
        QTreeView::item:hover,
        QListView::item:hover,
        QTableView::item:hover {
            background: @RAISED@;
        }
        QTreeView#HostTree::item:hover:!selected {
            /* 悬停卡片由 HostItemDelegate 只在实际节点区绘制；QSS 必须透明，否则树缩进区
               会被平台样式补成左侧矩形尾巴。 */
            background: transparent;
        }
        QTreeView#HostTree::item {
            min-height: 0;
            padding: 0;
            border: 0;
            border-radius: 9px;
        }
        QTreeView::item:selected,
        QListView::item:selected,
        QTableView::item:selected,
        QTreeView::item:selected:active,
        QListView::item:selected:active,
        QTableView::item:selected:active,
        QTreeView::item:selected:!active,
        QListView::item:selected:!active,
        QTableView::item:selected:!active {
            background: @SELECTION@;
            color: @TEXT@;
        }
        QTreeView::item:selected:hover,
        QListView::item:selected:hover,
        QTableView::item:selected:hover {
            background: @SELECTION@;
        }
        QTreeView#RemoteFileTree::item:selected,
        QTreeView#RemoteFileTree::item:selected:active,
        QTreeView#RemoteFileTree::item:selected:!active,
        QTreeView#RemoteFileTree::item:selected:hover {
            background: @SELECTION@;
            color: @TEXT@;
        }
        QTreeView#HostTree::item:selected,
        QTreeView#HostTree::item:selected:active,
        QTreeView#HostTree::item:selected:!active,
        QTreeView#HostTree::item:selected:hover {
            /* 主机树由委托绘制淡绿圆角卡片。这里必须透明，否则 Qt 会先给缩进区和整行铺
               通用选中色，重新形成已移除的左侧矩形尾巴并叠深右侧卡片底色。 */
            background: transparent;
            color: @TEXT@;
        }
    )QSS");

    // 最后一段仍使用同一个原始字符串定界符，保证 QSS 内的引号和反斜杠无需额外转义，
    // 同时让后面的统一主题占位符替换继续作用于拼接完成后的整份样式表。
    styleSheet += QStringLiteral(R"QSS(
        QHeaderView::section {
            color: @MUTED@;
            background: @SIDEBAR@;
            border: 0;
            border-bottom: 1px solid @BORDER@;
            padding: 6px;
            font-weight: 600;
        }
        QGroupBox {
            color: @TEXT@;
            border: 1px solid @BORDER@;
            border-radius: 10px;
            margin-top: 10px;
            padding-top: 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 5px;
        }
        QCheckBox,
        QRadioButton,
        QLabel {
            color: @TEXT@;
        }
        QMenuBar {
            color: @TEXT@;
            background: @SURFACE@;
            border: 1px solid @BORDER@;
        }
        QMenu {
            color: @TEXT@;
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 10px;
            padding: 6px;
        }
        QMenuBar::item {
            background: transparent;
            border-radius: 6px;
            padding: 5px 9px;
        }
        QMenu::item {
            background: transparent;
            border-radius: 7px;
            min-height: 22px;
            padding: 6px 12px;
        }
        QMenuBar::item:selected,
        QMenu::item:selected {
            background: @SELECTION@;
        }
        QMenu::icon {
            padding-left: 4px;
        }
        QMenu::separator {
            height: 1px;
            background: @BORDER@;
            margin: 4px 8px;
        }
        QScrollBar:vertical {
            background: transparent;
            width: 10px;
            margin: 2px;
        }
        QScrollBar::handle:vertical {
            background: @BORDER@;
            border-radius: 4px;
            min-height: 28px;
        }
        QScrollBar::handle:vertical:hover {
            background: @MUTED@;
        }
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {
            background: transparent;
            border: 0;
            height: 0;
        }
        QScrollBar:horizontal {
            background: transparent;
            height: 10px;
            margin: 2px;
        }
        QScrollBar::handle:horizontal {
            background: @BORDER@;
            border-radius: 4px;
            min-width: 28px;
        }
        QScrollBar::handle:horizontal:hover {
            background: @MUTED@;
        }
        QScrollBar::add-line:horizontal,
        QScrollBar::sub-line:horizontal,
        QScrollBar::add-page:horizontal,
        QScrollBar::sub-page:horizontal {
            background: transparent;
            border: 0;
            width: 0;
        }
        QSplitter::handle {
            background: @BORDER@;
        }
        QProgressBar {
            color: @TEXT@;
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 5px;
            min-height: 8px;
            max-height: 8px;
            text-align: center;
        }
        QProgressBar::chunk {
            background: @ACCENT@;
            border-radius: 4px;
        }
        QToolTip {
            color: @TEXT@;
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            padding: 5px;
        }
        /* 设置类窗口的分栏版式：左侧导航贴合侧栏底色，右侧内容区用工作区底色托起卡片，
           这样卡片本身的 @SURFACE@ 才能形成清晰的层次，而不是与背景糊成一块。 */
        QDialog#SettingsDialog,
        QDialog#ConnectionDialog {
            background: @WORKSPACE@;
        }
        QWidget#SettingsSidebar {
            background: @SIDEBAR@;
            border-right: 1px solid @BORDER@;
        }
        QLabel#SettingsSidebarTitle {
            color: @TEXT@;
            font-size: 17px;
            font-weight: 700;
            padding: 2px 6px 4px 6px;
        }
        QListWidget#SettingsNav {
            background: transparent;
            border: 0;
        }
        QListWidget#SettingsNav::item {
            color: @TEXT@;
            border-radius: 9px;
            padding: 6px 10px;
        }
        QListWidget#SettingsNav::item:selected {
            background: @RAISED@;
            font-weight: 600;
        }
        QScrollArea#SettingsScroll,
        QScrollArea#SettingsScroll > QWidget > QWidget,
        QWidget#SettingsPage,
        QStackedWidget#SettingsPages {
            background: @WORKSPACE@;
        }
        QFrame#SettingsCard {
            background: @SURFACE@;
            border: 1px solid @BORDER@;
            border-radius: 14px;
        }
        QLabel#SettingsCardTitle {
            color: @TEXT@;
            font-size: 14px;
            font-weight: 700;
        }
        QLabel#SettingsRowLabel {
            color: @TEXT@;
        }
        QLabel#SettingsRowHint {
            color: @MUTED@;
            font-size: 12px;
        }
        QWidget#SettingsFooter {
            background: @SIDEBAR@;
            border-top: 1px solid @BORDER@;
        }
        QToolButton[stepperButton="true"] {
            color: @TEXT@;
            background: @RAISED@;
            border: 1px solid @BORDER@;
            border-radius: 9px;
            font-size: 15px;
            font-weight: 700;
        }
        QToolButton[stepperButton="true"]:hover {
            background: @SELECTION@;
            border-color: @ACCENT@;
        }
        QToolButton[stepperButton="true"]:disabled {
            color: @MUTED@;
            background: @SIDEBAR@;
        }
        QToolButton[advancedDisclosure="true"] {
            color: @MUTED@;
            background: transparent;
            border: 0;
            min-height: 24px;
            padding: 2px 0;
            text-align: left;
        }
        QToolButton[advancedDisclosure="true"]:hover,
        QToolButton[advancedDisclosure="true"]:checked {
            color: @TEXT@;
            background: transparent;
        }
        QPushButton[segmentButton="true"] {
            color: @MUTED@;
            background: transparent;
            border: 1px solid @BORDER@;
            border-radius: 9px;
            padding: 4px 13px;
        }
        QPushButton[segmentButton="true"]:hover {
            color: @TEXT@;
            background: @RAISED@;
        }
        QPushButton[segmentButton="true"]:checked {
            color: @TEXT@;
            background: @SELECTION@;
            border-color: @ACCENT@;
            font-weight: 600;
        }
    )QSS");
    styleSheet.replace(QStringLiteral("@WORKSPACE@"), workspaceBackground);
    styleSheet.replace(QStringLiteral("@NAVIGATION@"), navigationBackground);
    styleSheet.replace(QStringLiteral("@SIDEBAR@"), sidebarBackground);
    styleSheet.replace(QStringLiteral("@SURFACE@"), surfaceBackground);
    styleSheet.replace(QStringLiteral("@RAISED@"), raisedBackground);
    styleSheet.replace(QStringLiteral("@HOST_TREE_HOVER@"), hostTreeHoverBackground);
    styleSheet.replace(QStringLiteral("@BORDER@"), borderColor);
    styleSheet.replace(QStringLiteral("@TEXT@"), textColor);
    styleSheet.replace(QStringLiteral("@MUTED@"), mutedTextColor);
    // 箭头资源必须在颜色占位符替换后再写入，避免资源路径中的字符被其他主题替换误伤。
    // 使用编译进 qrc 的绝对资源路径，不依赖应用启动时的当前工作目录。
    styleSheet.replace(QStringLiteral("@COMBO_ARROW@"),
                       dark ? QStringLiteral(":/resources/icons/chevron-down-dark.svg")
                            : QStringLiteral(":/resources/icons/chevron-down-light.svg"));
    styleSheet.replace(QStringLiteral("@ACCENT@"), accentColor);
    styleSheet.replace(QStringLiteral("@DANGER@"), dangerColor);
    styleSheet.replace(QStringLiteral("@DANGER_TINT@"), dangerTintColor);
    styleSheet.replace(QStringLiteral("@WARNING@"), warningColor);
    styleSheet.replace(QStringLiteral("@WARNING_TINT@"), warningTintColor);
    styleSheet.replace(QStringLiteral("@PROMINENT_SELECTION@"),
                       prominentSelectionBackground);
    styleSheet.replace(QStringLiteral("@SELECTION@"), selectionBackground);
    // 保险库既可能在启动阶段使用独立样式，也可能在主窗口运行期间再次弹出。这里复用同一
    // 片段，确保应用自选主题与系统主题不一致时仍以应用主题绘制安全徽标、警示框和错误态。
    styleSheet += ShelliumUi::vaultDialogStyleSheet(dark);
    // 主机链弹窗包含自绘列表委托、固定目标卡片和危险移除动作；追加同源专用片段，使它们
    // 在应用自选主题下与独立测试环境保持一致，而不把这些选择器污染到普通 QListWidget。
    styleSheet += ShelliumUi::hostChainDialogStyleSheet(dark);
    // 顶层 QMenu / QComboBox 弹出层等不是 MainWindow 的子控件，
    // this->setStyleSheet(...) 只对 MainWindow 子树生效，右键菜单等
    // 顶层部件会吃不到这份样式，因此改为通过 qApp 应用到整个应用。
    qApp->setStyleSheet(styleSheet);

    if (m_hostManager != nullptr) {
        if (auto *hostTree = m_hostManager->findChild<QAbstractItemView *>(
                QStringLiteral("HostTree"))) {
            // QSS 能绘制列表表面，却不会可靠地把应用自选主题回写到操作系统 QPalette::Base。
            // 主机委托自绘图标卡片时通过动态属性读取同一 SURFACE 色，才能正确支持“系统深色
            // + 应用浅色”及其反向组合；更新 viewport 让主题切换在当前帧立即生效。
            hostTree->setProperty("hostIconSurfaceColor", surfaceBackground);
            hostTree->setProperty("hostHoverBackgroundColor", hostTreeHoverBackground);
            hostTree->viewport()->update();
        }
    }
    if (m_portForwardManager != nullptr) {
        if (auto *forwardingHostTree = m_portForwardManager->findChild<QAbstractItemView *>(
                QStringLiteral("PortForwardHostTree"))) {
            // 端口转发的主机下拉树采用与主机管理同源的委托式卡片。两棵树必须读取完全
            // 相同的主题色属性，否则应用主题与系统主题不一致时又会出现明显的页面割裂。
            forwardingHostTree->setProperty("hostIconSurfaceColor", surfaceBackground);
            forwardingHostTree->setProperty("hostHoverBackgroundColor",
                                             hostTreeHoverBackground);
            forwardingHostTree->viewport()->update();
        }
    }

    if (auto *sessionTabs = dynamic_cast<SessionTabBar *>(m_titleTabBar)) {
        sessionTabs->setVisualColors(QColor(QStringLiteral("#aeb6cc")),
                                     QColor(QStringLiteral("#f8f9fd")),
                                     QColor(QStringLiteral("#39415d")),
                                     QColor(QStringLiteral("#424b68")));
    }

    const QColor chromeIconColor(QStringLiteral("#c6ccdc"));
    const QColor navigationIconColor(mutedTextColor);
    for (QToolButton *button : std::as_const(m_chromeButtons)) {
        if (button == nullptr) {
            continue;
        }
        const bool livesInChrome = button->parentWidget() == m_titleChrome;
        const QColor iconColor = livesInChrome ? chromeIconColor : navigationIconColor;
        const QString role = button->property("chromeRole").toString();
        QIcon icon;
        if (role == QStringLiteral("sidebar")) {
            icon = UiIcons::symbol(this, UiIcons::Symbol::Sidebar, iconColor);
        } else if (role == QStringLiteral("newTab")) {
            icon = UiIcons::symbol(this, UiIcons::Symbol::Plus, iconColor);
        } else if (role == QStringLiteral("scrollLeft")) {
            icon = UiIcons::symbol(this, UiIcons::Symbol::ChevronLeft, iconColor);
        } else if (role == QStringLiteral("scrollRight")) {
            icon = UiIcons::symbol(this, UiIcons::Symbol::ChevronRight, iconColor);
        } else if (role == QStringLiteral("command")) {
            icon = UiIcons::symbol(this, UiIcons::Symbol::Command, iconColor);
        } else if (role == QStringLiteral("settings")) {
            icon = UiIcons::symbol(this, UiIcons::Symbol::Settings, iconColor);
        }
        if (icon.isNull()) {
            continue;
        }
        // 可勾选动作在每次切换时都会发出 changed()，QToolButton 随后从 defaultAction 重新
        // 同步图标与文字。若图标只设在按钮上，动作自身的空图标会把它覆盖掉：按钮退化成
        // 纯文字，又因宽度只有 38px 被省略成"…"——正是侧边栏按钮点一下就变三个点的原因。
        // 因此对带默认动作的按钮把图标写回动作，让这次同步取到的仍是同一个图标。
        if (QAction *defaultAction = button->defaultAction();
            defaultAction != nullptr && defaultAction->isCheckable()) {
            defaultAction->setIcon(icon);
        } else {
            button->setIcon(icon);
        }
        button->setIconSize(QSize(20, 20));
    }

    const QList<UiIcons::Symbol> navigationSymbols = {
        UiIcons::Symbol::Hosts,
        UiIcons::Symbol::Snippets,
        UiIcons::Symbol::Forwarding,
        UiIcons::Symbol::Sparkle};
    for (int index = 0;
         index < m_navigationButtons.size() && index < navigationSymbols.size();
         ++index) {
        QToolButton *button = m_navigationButtons.at(index);
        button->setIcon(UiIcons::symbol(this,
                                        navigationSymbols.at(index),
                                        navigationIconColor));
        button->setIconSize(QSize(21, 21));
    }
}

void MainWindow::showSessionTabContextMenu(const QPoint &position)
{
    if (m_titleTabBar == nullptr) {
        return;
    }
    const int index = m_titleTabBar->tabAt(position);
    QMenu *menu = createSessionTabMenu(index, this);
    menu->exec(m_titleTabBar->mapToGlobal(position));
    delete menu;
}

QMenu *MainWindow::createSessionTabMenu(const int index, QWidget *parent)
{
    auto *menuPointer = new QMenu(parent);
    QMenu &menu = *menuPointer;
    UiIcons::modernizePopup(&menu);
    QAction *newAction = menu.addAction(
        UiIcons::symbol(this, UiIcons::Symbol::Plus, this->palette().color(QPalette::WindowText)),
        tr("New terminal tab"));
    connect(newAction, &QAction::triggered, this, [this]() {
        TerminalWorkspace *workspace = createWorkspaceTab(
            tr("Terminal %1").arg(m_tabs->count() + 1));
        workspace->activePane()->openLocalShell();
    });

    if (index >= 0) {
        if (m_titleTabBar != nullptr) {
            m_titleTabBar->setCurrentIndex(index);
        }
        menu.addSeparator();
        QAction *duplicateAction = menu.addAction(
            UiIcons::symbol(this, UiIcons::Symbol::Copy, this->palette().color(QPalette::WindowText)),
            tr("Copy tag"));
        connect(duplicateAction,
                &QAction::triggered,
                this,
                [this, index]() { duplicateWorkspaceTab(index); });
        QAction *renameAction = menu.addAction(
            UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)),
            tr("Rename"));
        QAction *closeAction = menu.addAction(
            UiIcons::symbol(this, UiIcons::Symbol::Close, this->palette().color(QPalette::WindowText)),
            tr("Close tab"));
        QAction *closeOthersAction = menu.addAction(tr("Close other tabs"));
        // 关闭最后一个标签会回到品牌空态页，因此不再禁用；"关闭其他"只有真的存在其他
        // 标签时才有意义。
        closeOthersAction->setEnabled(m_tabs->count() > 1);

        connect(renameAction, &QAction::triggered, this, [this, index]() {
            bool accepted = false;
            const QString name = QInputDialog::getText(this,
                                                        tr("Rename tab"),
                                                        tr("Tag name:"),
                                                        QLineEdit::Normal,
                                                        m_tabs->tabText(index),
                                                        &accepted).trimmed();
            if (accepted && !name.isEmpty()) {
                setWorkspaceTabText(index, name);
            }
        });
        connect(closeAction,
                &QAction::triggered,
                this,
                [this, index]() { closeWorkspaceTab(index); });
        connect(closeOthersAction, &QAction::triggered, this, [this, index]() {
            // 从末尾倒序删除可保持尚未处理索引稳定；跳过原索引后，低位删除只会把保留项
            // 向左移动，不会误删该会话。
            for (int candidate = m_tabs->count() - 1; candidate >= 0; --candidate) {
                if (candidate != index) {
                    closeWorkspaceTab(candidate);
                }
            }
        });
    }
    return menuPointer;
}

void MainWindow::duplicateWorkspaceTab(const int index)
{
    auto *source = qobject_cast<TerminalWorkspace *>(m_tabs->widget(index));
    TerminalPane *sourcePane = source == nullptr ? nullptr : source->activePane();
    if (sourcePane == nullptr) {
        return;
    }
    const QList<ConnectionTarget> connectionPlan = sourcePane->connectionPlan();

    TerminalWorkspace *copy = createWorkspaceTab(m_tabs->tabText(index));
    copy->setFileManagerVisible(source->isFileManagerVisible());
    m_tabs->setCurrentWidget(copy);
    TerminalPane *pane = copy->activePane();
    if (pane == nullptr) {
        return;
    }
    if (connectionPlan.isEmpty() || !connectionPlan.constLast().isValid()) {
        pane->openLocalShell();
        return;
    }

    // 计划里已经带着逐跳解析好的凭据，副本因此不必重新解锁保险库；只有当根目标确实是
    // 主机树里的持久化记录时才登记连接历史，临时连接的随机 UUID 写进去会成为悬空外键。
    const QString rootHostId = connectionPlan.constLast().id;
    QString lookupError;
    if (!rootHostId.isEmpty()
        && m_database->findHost(rootHostId, &lookupError).has_value()) {
        m_pendingHistoryHosts.insert(pane, rootHostId);
        m_pendingHistoryStartedAt.insert(pane, QDateTime::currentDateTimeUtc());
    }
    const std::optional<HostRecord> copiedHost = m_database->findHost(rootHostId, &lookupError);
    // 复制标签仍然属于同一条已保存主机。若用户在第一次连接尚未等到识别完成就复制标签，
    // 允许两个会话中的任意一个完成一次探测；数据库一旦写入，后续新连接都会自动跳过。
    pane->setRemoteDistributionDetectionEnabled(
        copiedHost.has_value() && copiedHost->distributionId.isEmpty());
    pane->connectToHosts(connectionPlan);

    const int copyIndex = m_tabs->indexOf(copy);
    if (copyIndex >= 0 && m_titleTabBar != nullptr) {
        m_titleTabBar->setTabConnectionState(copyIndex,
                                             SessionTabBar::ConnectionState::Connecting);
    }
}

void MainWindow::showTransientMessage(const QString &message, int)
{
    // 这些消息一律只落日志：界面上曾经的标题栏提示条已被移除，超时参数因此失去意义，
    // 保留在签名里只是为了不让几十处调用点跟着改动。
    ActivityLog::instance().append(message);
}

void MainWindow::connectCurrentPane()
{
    TerminalWorkspace *workspace = activeWorkspace();
    TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
    if (pane == nullptr) {
        return;
    }

    ConnectionDialog dialog(this);
    if (pane->activeTarget().isValid()) {
        dialog.setInitialTarget(pane->activeTarget());
    }
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    pane->connectToHost(dialog.target());
}

void MainWindow::connectNestedFromCurrentPane()
{
    TerminalWorkspace *workspace = activeWorkspace();
    TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
    if (pane == nullptr) {
        return;
    }
    if (!pane->canPushNestedLayer()) {
        QMessageBox::information(
            this,
            tr("Could not establish the nested connection"),
            tr("The current tab is not connected to a remote server yet. Connect to a server first, then continue to an internal host from it."));
        return;
    }

    ConnectionDialog dialog(this);
    // 内层地址由当前服务器的视角解析，因此不预填当前主机名——那几乎肯定不是用户想去的
    // 下一跳，预填反而容易被误当成默认值直接确认。
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    pane->connectNestedHost(dialog.target());
}

void MainWindow::connectSavedHost(const QString &hostId,
                                  const QList<ConnectionTarget> &requestedPlan,
                                  const bool openInNewTab)
{
    QString routeError;
    QList<ConnectionTarget> connectionPlan = requestedPlan;
    if (connectionPlan.isEmpty()) {
        const QList<HostRecord> route = m_database->resolveConnectionChain(hostId, &routeError);
        if (route.isEmpty()) {
            QMessageBox::critical(this,
                                  tr("Could not connect to the saved host"),
                                  routeError.isEmpty() ? tr("The host record or host chain does not exist") : routeError);
            return;
        }
        for (const HostRecord &record : route) {
            ConnectionTarget target = record.toConnectionTarget();
            if (route.size() > 1) {
                target.proxyJumps.clear();
            }
            connectionPlan.append(target);
        }
    }
    if (connectionPlan.isEmpty() || !connectionPlan.constLast().isValid()) {
        return;
    }
    ConnectionTarget &logicalTarget = connectionPlan.last();
    TerminalWorkspace *workspace = openInNewTab
        ? createWorkspaceTab(logicalTarget.displayName.isEmpty() ? logicalTarget.host
                                                                  : logicalTarget.displayName)
        : activeWorkspace();
    if (openInNewTab && workspace != nullptr) {
        m_tabs->setCurrentWidget(workspace);
    }
    TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
    if (pane == nullptr) {
        return;
    }

    // 一次解锁后逐跳读取各自主机凭据。每一跳都按自己的 id 查找已解析记录，绝不能把最终
    // 目标密码复制给跳板机；这正是自由文本 ProxyJump 无法满足的核心场景。
    bool vaultReady = false;
    for (ConnectionTarget &target : connectionPlan) {
        if (!target.password.isEmpty()) {
            continue;
        }
        QString lookupError;
        const std::optional<HostRecord> savedHost = m_database->resolveHost(target.id, &lookupError);
        if (!savedHost.has_value()) {
            QMessageBox::critical(this,
                                  tr("Could not read the host chain"),
                                  lookupError.isEmpty()
                                      ? tr("The chain host does not exist: %1").arg(target.displayName)
                                      : lookupError);
            return;
        }
        if (!savedHost->credentialId.isEmpty()) {
            const bool requiresUnlock = m_credentialVault->credentialRequiresUnlock(
                savedHost->credentialId, &lookupError);
            if (!lookupError.isEmpty()) {
                QMessageBox::critical(this, tr("Could not read the host credentials"), lookupError);
                return;
            }
            if (requiresUnlock && !vaultReady) {
                if (!ensureVaultUnlocked()) {
                    return;
                }
                vaultReady = true;
            }
            QString credentialError;
            if (!m_credentialVault->applyHostCredential(*savedHost, target, &credentialError)) {
                QMessageBox::critical(this,
                                      tr("Could not read the host credentials"),
                                      credentialError.isEmpty() ? tr("The credential does not exist") : credentialError);
                return;
            }
        }
    }
    // 持久化主机 id 必须进入终端根层快照。ConnectionDialog 会为临时会话生成 UUID，如果不
    // 覆盖，连接历史、命令历史与凭据引用就无法回到主机树中的同一条记录。
    logicalTarget.id = hostId;
    logicalTarget.parentId.clear();
    logicalTarget.confidence = ConnectionTarget::Confidence::Confirmed;
    QString distributionLookupError;
    const std::optional<HostRecord> savedLogicalHost =
        m_database->findHost(hostId, &distributionLookupError);
    // “首次成功登录”由持久化字段是否为空决定，而不是进程内状态：应用重启、连接失败或
    // 自动重连都不会把同一台主机重新标成首次。读取异常不会中断已经完成的连接配置，只是
    // 安全地跳过本轮可选探测，避免把数据库错误伪装成 SSH 错误。
    pane->setRemoteDistributionDetectionEnabled(
        savedLogicalHost.has_value() && savedLogicalHost->distributionId.isEmpty());
    m_pendingHistoryHosts.insert(pane, hostId);
    m_pendingHistoryStartedAt.insert(pane, QDateTime::currentDateTimeUtc());
    pane->connectToHosts(connectionPlan);

    const int tabIndex = m_tabs->indexOf(workspace);
    if (tabIndex >= 0) {
        setWorkspaceTabText(tabIndex,
                            logicalTarget.displayName.isEmpty() ? logicalTarget.connectionSpecifier()
                                                                : logicalTarget.displayName);
        if (m_titleTabBar != nullptr) {
            m_titleTabBar->setTabConnectionState(tabIndex,
                                                 SessionTabBar::ConnectionState::Connecting);
        }
    }
}

bool MainWindow::ensureVaultUnlocked()
{
    return VaultDialog::ensureUnlocked(m_credentialVault, this, true);
}

void MainWindow::recordConnectionSucceeded(TerminalPane *pane,
                                           const ConnectionTarget &target)
{
    Q_UNUSED(target)
    if (pane == nullptr || !m_pendingHistoryHosts.contains(pane)) {
        return;
    }

    ConnectionHistoryEntry entry;
    entry.hostId = m_pendingHistoryHosts.take(pane);
    entry.connectedAt = m_pendingHistoryStartedAt.take(pane);
    entry.succeeded = true;

    QString error;
    const std::optional<qint64> historyId =
        m_database->appendConnectionHistory(entry, &error);
    if (historyId.has_value()) {
        m_openHistoryRows.insert(pane, *historyId);
        m_hostManager->refresh();
    } else {
        showTransientMessage(tr("Connected, but writing the connection history failed: %1").arg(error), 0);
    }
}

void MainWindow::recordConnectionFailed(TerminalPane *pane,
                                        const ConnectionTarget &target,
                                        const QString &message)
{
    Q_UNUSED(target)
    if (pane == nullptr || !m_pendingHistoryHosts.contains(pane)) {
        return;
    }

    ConnectionHistoryEntry entry;
    entry.hostId = m_pendingHistoryHosts.take(pane);
    entry.connectedAt = m_pendingHistoryStartedAt.take(pane);
    entry.disconnectedAt = QDateTime::currentDateTimeUtc();
    entry.succeeded = false;
    entry.error = message;

    QString error;
    if (!m_database->appendConnectionHistory(entry, &error).has_value()) {
        showTransientMessage(tr("The connection failed, and writing the connection history also failed: %1").arg(error), 0);
    }
    m_hostManager->refresh();
}

void MainWindow::recordConnectionDisconnected(TerminalPane *pane,
                                              const ConnectionTarget &target,
                                              const int exitStatus)
{
    Q_UNUSED(target)
    Q_UNUSED(exitStatus)
    if (pane == nullptr || !m_openHistoryRows.contains(pane)) {
        return;
    }

    const qint64 historyId = m_openHistoryRows.take(pane);
    bool updated = false;
    QString error;
    if (!m_database->finishConnectionHistory(historyId,
                                             QDateTime::currentDateTimeUtc(),
                                             &updated,
                                             &error)) {
        showTransientMessage(tr("Failed to write disconnect time: %1").arg(error), 0);
    }
}

void MainWindow::configureAiAssistant(AiAssistantWidget *assistant, TerminalPane *pane)
{
    Q_ASSERT(assistant != nullptr);
    Q_ASSERT(pane != nullptr);

    QPointer<TerminalPane> guardedPane(pane);
    connect(assistant,
            &AiAssistantWidget::statusMessage,
            this,
            [this](const QString &message) { showTransientMessage(message, 8000); });
    connect(assistant,
            &AiAssistantWidget::commandExecutionRequested,
            this,
            [this, guardedPane](const QString &command, const QStringList &targetIds) {
        // 单标签 AI 助手不支持广播目标；命令必须投递给创建它时绑定的终端，而不是槽执行
        // 瞬间的 activeWorkspace()。这样即使点击事件与标签切换落在同一事件循环，也不会
        // 把旧聊天里的命令发到新标签。
        if (!targetIds.isEmpty()) {
            showTransientMessage(tr("The AI assistant does not support running commands on multiple terminals"), 5000);
            return;
        }
        if (guardedPane.isNull()) {
            showTransientMessage(tr("The matching terminal tab is already closed"), 5000);
            return;
        }
        guardedPane->executeCommand(command);
    });
    connect(assistant,
            &AiAssistantWidget::commandInsertionRequested,
            this,
            [this, guardedPane](const QString &command, const QStringList &targetIds) {
        // 插入与执行共享同一绑定规则，但仍保持两个独立信号，避免“修改”动作误带回车执行。
        if (!targetIds.isEmpty()) {
            showTransientMessage(tr("Inserting an unexecuted command into multiple terminals is not supported yet"), 5000);
            return;
        }
        if (guardedPane.isNull()) {
            showTransientMessage(tr("The matching terminal tab is already closed"), 5000);
            return;
        }
        guardedPane->insertCommandText(command);
        showTransientMessage(tr("The command was inserted into the matching terminal; press Enter to run it"), 5000);
    });
    assistant->setTerminalContextProvider([guardedPane]() -> QString {
        // QPointer 在标签关闭后自动归零；回调即使被延迟的捕获定时器触发，也只返回空文本，
        // 不会解引用已经析构的 TerminalPane。
        return guardedPane.isNull() ? QString() : guardedPane->terminalPlainText();
    });
    assistant->setActiveTarget(pane->activeTarget());
}

QWidget *MainWindow::createWorkspacePlaceholder()
{
    auto *placeholder = new QWidget(this);
    placeholder->setObjectName(QStringLiteral("WorkspacePlaceholder"));
    auto *layout = new QVBoxLayout(placeholder);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(14);
    layout->addStretch(1);

    auto *iconLabel = new QLabel(placeholder);
    iconLabel->setObjectName(QStringLiteral("WorkspacePlaceholderIcon"));
    iconLabel->setAlignment(Qt::AlignCenter);
    // 优先使用 QApplication 已设置的主图标，保证空态页与 Dock、任务切换器里看到的是
    // 同一枚图标；测试或裁剪构建里没有设置时再退回编译进 qrc 的同一张位图。
    QIcon brandIcon = QApplication::windowIcon();
    if (brandIcon.isNull()) {
        brandIcon = QIcon(QStringLiteral(":/resources/icons/shellium-app-icon-macos-512.png"));
    }
    iconLabel->setPixmap(brandIcon.pixmap(QSize(112, 112)));
    layout->addWidget(iconLabel);

    // 产品名写死为品牌名，不使用 QCoreApplication::applicationName()：后者默认取可执行
    // 文件名，在测试构建或改名分发时会把一个内部名字直接印在空态页正中央。
    auto *titleLabel = new QLabel(QStringLiteral("Shellium"), placeholder);
    titleLabel->setObjectName(QStringLiteral("WorkspacePlaceholderTitle"));
    titleLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(titleLabel);

    auto *hintLabel = new QLabel(
        tr("Press + in the title bar to open a new terminal tab, or connect to a server from the host list on the left."), placeholder);
    hintLabel->setObjectName(QStringLiteral("WorkspacePlaceholderHint"));
    hintLabel->setAlignment(Qt::AlignCenter);
    hintLabel->setWordWrap(true);
    layout->addWidget(hintLabel);

    layout->addStretch(1);
    return placeholder;
}

void MainWindow::updateWorkspacePlaceholder()
{
    if (m_workspaceStack == nullptr || m_tabs == nullptr) {
        return;
    }
    const bool hasSessions = m_tabs->count() > 0;
    m_workspaceStack->setCurrentWidget(hasSessions ? static_cast<QWidget *>(m_tabs)
                                                   : m_workspacePlaceholder);

    // 空标签栏并不是零宽：QTabBar 的 minimumSizeHint 与滚动箭头的隐藏占位加在一起，会在
    // 侧栏开关和"+"之间留下一大段无法解释的空白。没有会话时把这三个控件整体移出布局，
    // "+"才会紧贴侧栏开关。
    if (m_titleTabBar != nullptr) {
        m_titleTabBar->setVisible(hasSessions);
    }
    for (QToolButton *arrow : {m_tabScrollLeftButton, m_tabScrollRightButton}) {
        if (arrow == nullptr) {
            continue;
        }
        QSizePolicy policy = arrow->sizePolicy();
        policy.setRetainSizeWhenHidden(hasSessions);
        arrow->setSizePolicy(policy);
    }
}

TerminalWorkspace *MainWindow::createWorkspaceTab(const QString &title)
{
    auto *workspace = new TerminalWorkspace(m_tabs);
    const QIcon terminalIcon = UiIcons::symbol(this, UiIcons::Symbol::Terminal, this->palette().color(QPalette::WindowText));
    const int index = m_tabs->addTab(workspace, terminalIcon, title);
    // QTabWidget 在从零恢复到第一个页面时可能重新计算标签栏可见性；每次新增后再次隐藏，
    // 保证中央区域永远只显示标题栏中的那一套会话标签。
    m_tabs->tabBar()->hide();
    updateWorkspacePlaceholder();
    if (m_titleTabBar != nullptr) {
        // 运行期间创建的新工作区必须在标题栏加入同索引标签；初始工作区创建得更早，会在
        // 会话栏构造时统一镜像，因此这里需要允许指针为空。
        m_titleTabBar->insertTab(index, terminalIcon, title);
    }

    TerminalPane *pane = workspace->activePane();
    AiAssistantWidget *assistant = nullptr;
    if (m_aiAssistantStack != nullptr && m_aiSettingsService != nullptr) {
        assistant = new AiAssistantWidget(m_aiSettingsService, m_aiAssistantStack);
        configureAiAssistant(assistant, pane);
        m_aiAssistantStack->insertWidget(index, assistant);
    }
    m_tabs->setCurrentIndex(index);
    if (m_titleTabBar != nullptr) {
        m_titleTabBar->setCurrentIndex(index);
    }
    if (m_aiAssistantStack != nullptr) {
        m_aiAssistantStack->setCurrentIndex(index);
    }

    connect(workspace,
            &TerminalWorkspace::statusMessage,
            this,
            [this, workspace](const QString &message) {
                if (workspace == activeWorkspace()) {
                    showTransientMessage(message, 8000);
                }
            });
    connect(workspace,
            &TerminalWorkspace::closeRequested,
            this,
            [this](TerminalWorkspace *requester) {
                // 索引必须现查：用户可能已经拖动过标签，创建时的那个 index 早已失效。
                const int currentIndex = m_tabs->indexOf(requester);
                if (currentIndex >= 0) {
                    closeWorkspaceTab(currentIndex);
                }
            });
    pane->setCommandHistoryDatabase(m_database);
    pane->applyTerminalSettings(m_settingsService->currentSettings());
    connect(workspace,
            &TerminalWorkspace::activeTargetsChanged,
            this,
            [this, assistant](TerminalPane *changedPane) {
                // 每个 AI 页面直接跟随自己终端的目标变化，即使该标签当前隐藏也保持上下文
                // 名称准确；效率工具的全局目标集合仍由统一刷新路径维护。
                if (assistant != nullptr && changedPane != nullptr) {
                    assistant->setActiveTarget(changedPane->activeTarget());
                }
                refreshProductivityTargets();
            });
    connect(pane,
            &TerminalPane::commandHistoryChanged,
            this,
            [this](const QString &) {
                // TerminalPane 直接使用数据库以保持终端层可独立测试，因此历史写入不会经过
                // ProductivityModel；显式刷新侧栏可让刚执行的命令立即出现。
                m_productivityWidget->refresh();
            });
    connect(pane,
            &TerminalPane::broadcastSourceInput,
            this,
            [this](TerminalPane *source, const QByteArray &bytes) {
                TerminalWorkspace *workspace = activeWorkspace();
                if (!m_broadcastEnabled || workspace == nullptr
                    || workspace->activePane() != source) {
                    return;
                }
                sendBytesToTargets(bytes, m_broadcastTargetIds, source);
            });
    connect(pane,
            &TerminalPane::forwardingSessionAboutToClose,
            this,
            [this, pane](SshSessionStack *sessionStack) {
                const QList<ConnectionTarget> targets = pane->targets();
                const QString rootHostId = targets.isEmpty() ? QString() : targets.constFirst().id;
                m_portForwardManager->unregisterSession(rootHostId, sessionStack);
            });
    connect(pane,
            &TerminalPane::rootConnectionSucceeded,
            this,
            [this, pane, workspace](const ConnectionTarget &target) {
                recordConnectionSucceeded(pane, target);
                if (m_titleTabBar != nullptr) {
                    m_titleTabBar->setTabConnectionState(m_tabs->indexOf(workspace),
                                                         SessionTabBar::ConnectionState::Connected);
                }
                if (!target.id.isEmpty() && pane->forwardingSessionStack() != nullptr) {
                    // 只有完成认证后的根栈才可承载规则；临时连接 id 不在数据库中，因此
                    // 管理器即使收到也不会找到任何持久化规则。
                    m_portForwardManager->registerSession(target.id,
                                                          pane->forwardingSessionStack());
                }
            });
    connect(pane,
            &TerminalPane::remoteDistributionDetected,
            this,
            [this](const ConnectionTarget &target,
                   const QString &distributionId,
                   const QString &versionId) {
                if (target.id.isEmpty() || distributionId.isEmpty()) {
                    return;
                }
                QString lookupError;
                std::optional<HostRecord> savedHost = m_database->findHost(target.id, &lookupError);
                // 信号可能来自用户刚删除、编辑或替换的旧标签。重新按稳定 id 读取并确认仍是
                // 主机节点，避免把异步探测结果覆盖到分组或已修改成其他配置的记录。
                if (!savedHost.has_value() || savedHost->kind != HostNodeKind::Host
                    || !savedHost->distributionId.isEmpty()) {
                    return;
                }
                savedHost->distributionId = distributionId;
                savedHost->distributionVersionId = versionId;
                // saveHost() 对携带有效 updatedAt 的记录会保留原时间；此次属于真实业务更新，
                // 显式清空让数据库统一写入当前 UTC，便于将来排查图标识别的更新时间。
                savedHost->updatedAt = {};
                QString saveError;
                if (!m_database->saveHost(*savedHost, &saveError)) {
                    ActivityLog::instance().append(
                        tr("Failed to save host distribution information: %1").arg(saveError));
                    return;
                }
                // 图标由 HostRecord 重建得到，刷新能同时更新嵌套目录、搜索结果与所有主题下
                // 的委托绘制，不需要直接持有或猜测某个 QTreeWidgetItem 的生命周期。
                m_hostManager->refresh();
            });
    connect(pane,
            &TerminalPane::automaticReconnectStarted,
            this,
            [this, pane, workspace](const ConnectionTarget &target,
                         const int attempt,
                         const int maximumAttempts) {
                // 只有主机树中的持久化 id 才写历史；临时连接仍可自动恢复，但不能制造无法在
                // 主机管理中归属的孤立历史行。
                QString lookupError;
                const std::optional<HostRecord> saved =
                    m_database->findHost(target.id, &lookupError);
                if (saved.has_value() && saved->kind == HostNodeKind::Host) {
                    m_pendingHistoryHosts.insert(pane, target.id);
                    m_pendingHistoryStartedAt.insert(pane, QDateTime::currentDateTimeUtc());
                }
                if (m_titleTabBar != nullptr) {
                    m_titleTabBar->setTabConnectionState(
                        m_tabs->indexOf(workspace), SessionTabBar::ConnectionState::Connecting);
                }
                showTransientMessage(
                    tr("Reconnecting to %1 automatically (attempt %2/%3)")
                        .arg(target.connectionSpecifier())
                        .arg(attempt)
                        .arg(maximumAttempts),
                    5000);
            });
    connect(pane,
            &TerminalPane::rootConnectionFailed,
            this,
            [this, pane, workspace](const ConnectionTarget &target, const QString &message) {
                recordConnectionFailed(pane, target, message);
                if (m_titleTabBar != nullptr) {
                    m_titleTabBar->setTabConnectionState(m_tabs->indexOf(workspace),
                                                         SessionTabBar::ConnectionState::Failed);
                }
                m_portForwardManager->unregisterSession(target.id,
                                                        pane->forwardingSessionStack());
            });
    connect(pane,
            &TerminalPane::rootConnectionDisconnected,
            this,
            [this, pane, workspace](const ConnectionTarget &target, const int exitStatus) {
                recordConnectionDisconnected(pane, target, exitStatus);
                if (m_titleTabBar != nullptr) {
                    // -1 表示网络异常中断（无正常退出码）；用户主动 exit 产生的正常退出码
                    // 只是回退到本地终端，不是故障，不该继续顶着一个红点。
                    m_titleTabBar->setTabConnectionState(
                        m_tabs->indexOf(workspace),
                        exitStatus == -1 ? SessionTabBar::ConnectionState::Failed
                                         : SessionTabBar::ConnectionState::Unknown);
                }
                m_portForwardManager->unregisterSession(target.id,
                                                        pane->forwardingSessionStack());
            });
    connect(pane, &QObject::destroyed, this, [this, pane]() {
        // 标签关闭时先补齐已成功连接记录的断开时间，再清除尚未成功的连接请求，避免哈希表
        // 长期保存悬空 QObject 地址。
        if (m_openHistoryRows.contains(pane)) {
            bool updated = false;
            QString error;
            m_database->finishConnectionHistory(m_openHistoryRows.take(pane),
                                                QDateTime::currentDateTimeUtc(),
                                                &updated,
                                                &error);
        }
        m_pendingHistoryHosts.remove(pane);
        m_pendingHistoryStartedAt.remove(pane);
    });
    connect(workspace, &QObject::destroyed, this, [this]() {
        // QObject::destroyed 发出时标签页对象尚在析构栈上；排到下一轮事件循环再枚举，避免
        // 把即将释放的页面重新加入执行目标。
        QTimer::singleShot(0, this, [this]() { refreshProductivityTargets(); });
    });
    return workspace;
}

TerminalWorkspace *MainWindow::activeWorkspace() const
{
    return qobject_cast<TerminalWorkspace *>(m_tabs->currentWidget());
}

QList<TerminalPane *> MainWindow::allTerminalPanes() const
{
    QList<TerminalPane *> panes;
    panes.reserve(m_tabs->count());
    for (int index = 0; index < m_tabs->count(); ++index) {
        auto *workspace = qobject_cast<TerminalWorkspace *>(m_tabs->widget(index));
        if (workspace != nullptr && workspace->activePane() != nullptr) {
            panes.append(workspace->activePane());
        }
    }
    return panes;
}

void MainWindow::refreshProductivityTargets()
{
    if (m_productivityWidget == nullptr) {
        return;
    }
    QList<ConnectionTarget> targets;
    QSet<QString> seenHostIds;
    for (TerminalPane *pane : allTerminalPanes()) {
        ConnectionTarget target = pane->activeTarget();
        if (!pane->isConnected() || !target.isValid() || target.id.isEmpty()
            || seenHostIds.contains(target.id)) {
            continue;
        }
        // 效率侧栏按稳定主机 id 路由，同一主机打开多个标签时只显示一次；真正执行时会
        // 投递到全部匹配标签，避免界面出现无法区分的重复名称。侧栏不需要凭据，主动
        // 擦除副本可避免长期界面模型延长密码的内存驻留时间。
        seenHostIds.insert(target.id);
        target.password.fill(QChar::Null);
        target.password.clear();
        targets.append(std::move(target));
    }
    m_productivityWidget->setExecutionTargets(targets);

    if (m_aiAssistantStack != nullptr) {
        const int currentIndex = m_tabs == nullptr ? -1 : m_tabs->currentIndex();
        if (currentIndex >= 0 && currentIndex < m_aiAssistantStack->count()) {
            // AI 栈与终端标签使用相同索引；切换标签只改变当前页，不复制任何 m_history，因而
            // 每个实例的会话列表、进行中请求和终端读取位置都留在自己的标签中。
            m_aiAssistantStack->setCurrentIndex(currentIndex);
            auto *assistant = qobject_cast<AiAssistantWidget *>(
                m_aiAssistantStack->widget(currentIndex));
            TerminalWorkspace *workspace = activeWorkspace();
            TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
            if (assistant != nullptr) {
                assistant->setActiveTarget(
                    pane == nullptr ? ConnectionTarget{} : pane->activeTarget());
            }
        }
    }
}

void MainWindow::applyConfiguredShortcuts()
{
    if (m_settingsService == nullptr) {
        return;
    }
    for (auto iterator = m_configurableActions.cbegin();
         iterator != m_configurableActions.cend(); ++iterator) {
        if (iterator.value() != nullptr) {
            iterator.value()->setShortcut(m_settingsService->shortcut(iterator.key()));
        }
    }
}

void MainWindow::openCommandPalette()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Command palette"));
    dialog.resize(560, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *filterEdit = new QLineEdit(&dialog);
    filterEdit->setPlaceholderText(tr("Type a command name to filter…"));
    auto *list = new QListWidget(&dialog);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(filterEdit);
    layout->addWidget(list, 1);
    layout->addWidget(new QLabel(tr("Enter to run, Esc to close; only currently available actions are shown here."), &dialog));

    QList<QAction *> candidates;
    QSet<QAction *> seen;
    for (QAction *action : findChildren<QAction *>()) {
        if (action == nullptr || seen.contains(action) || action->isSeparator()
            || action->menu() != nullptr || !action->isVisible() || !action->isEnabled()
            || action->objectName() == QStringLiteral("CommandPaletteAction")) {
            continue;
        }
        QString text = action->text();
        text.remove(QLatin1Char('&'));
        text = text.trimmed();
        if (text.isEmpty()) {
            continue;
        }
        seen.insert(action);
        const int actionIndex = candidates.size();
        candidates.append(action);
        QString display = text;
        if (!action->shortcut().isEmpty()) {
            display += QStringLiteral("\t%1").arg(
                action->shortcut().toString(QKeySequence::NativeText));
        }
        auto *item = new QListWidgetItem(display, list);
        // 只存本次对话框局部数组下标，不把 QObject 指针塞入 QVariant；动作始终由主窗口
        // 拥有，但下标形式仍能避免元类型注册和跨架构指针整数转换。
        item->setData(Qt::UserRole, actionIndex);
        item->setToolTip(action->toolTip());
    }
    if (list->count() > 0) {
        list->setCurrentRow(0);
    }

    int selectedActionIndex = -1;
    const auto acceptCurrent = [&]() {
        QListWidgetItem *item = list->currentItem();
        if (item == nullptr || item->isHidden()) {
            return;
        }
        selectedActionIndex = item->data(Qt::UserRole).toInt();
        dialog.accept();
    };
    connect(filterEdit, &QLineEdit::textChanged, &dialog, [list](const QString &text) {
        const QString keyword = text.trimmed();
        int firstVisibleRow = -1;
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem *item = list->item(row);
            const bool matches = keyword.isEmpty()
                || item->text().contains(keyword, Qt::CaseInsensitive);
            item->setHidden(!matches);
            if (matches && firstVisibleRow < 0) {
                firstVisibleRow = row;
            }
        }
        list->setCurrentRow(firstVisibleRow);
    });
    connect(filterEdit, &QLineEdit::returnPressed, &dialog, acceptCurrent);
    connect(list, &QListWidget::itemActivated, &dialog,
            [&acceptCurrent](QListWidgetItem *) { acceptCurrent(); });
    filterEdit->setFocus();

    if (dialog.exec() == QDialog::Accepted && selectedActionIndex >= 0
        && selectedActionIndex < candidates.size()) {
        // 对话框已经关闭后再触发，避免目标动作弹出的模态窗口与命令面板争夺父窗口和焦点。
        QPointer<QAction> selectedAction = candidates.at(selectedActionIndex);
        QTimer::singleShot(0, this, [selectedAction]() {
            if (selectedAction != nullptr && selectedAction->isEnabled()) {
                selectedAction->trigger();
            }
        });
    }
}

void MainWindow::searchCurrentTerminal()
{
    TerminalWorkspace *workspace = activeWorkspace();
    TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
    if (pane == nullptr) {
        return;
    }

    bool accepted = false;
    const QString text = QInputDialog::getText(this,
                                               tr("Search terminal output"),
                                               tr("Find text:"),
                                               QLineEdit::Normal,
                                               m_lastTerminalSearch,
                                               &accepted);
    if (!accepted || text.isEmpty()) {
        return;
    }
    m_lastTerminalSearch = text;
    if (!pane->findTerminalText(text)) {
        showTransientMessage(tr("'%1' was not found in the terminal output").arg(text), 5000);
    }
}

void MainWindow::exportCurrentTerminal()
{
    TerminalWorkspace *workspace = activeWorkspace();
    TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
    if (pane == nullptr) {
        return;
    }
    const QString defaultName = QStringLiteral("shellium-terminal-%1.txt")
                                    .arg(QDateTime::currentDateTime()
                                             .toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("Export terminal output"),
        QDir::home().filePath(defaultName),
        tr("Text files (*.txt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this,
                              tr("Export failed"),
                              tr("Could not open %1: %2").arg(path, output.errorString()));
        return;
    }
    const QByteArray content = pane->terminalPlainText().toUtf8();
    if (output.write(content) != content.size() || !output.commit()) {
        QMessageBox::critical(this,
                              tr("Export failed"),
                              tr("Failed to write %1: %2").arg(path, output.errorString()));
        return;
    }
    showTransientMessage(tr("Terminal output exported to %1").arg(path), 5000);
}

void MainWindow::saveCurrentWorkspace()
{
    if (m_tabs == nullptr || m_tabs->count() == 0) {
        return;
    }
    QJsonArray tabs;
    for (int index = 0; index < m_tabs->count(); ++index) {
        auto *workspace = qobject_cast<TerminalWorkspace *>(m_tabs->widget(index));
        TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
        if (workspace == nullptr || pane == nullptr) {
            continue;
        }
        QString rootHostId;
        const QList<ConnectionTarget> targets = pane->targets();
        if (!targets.isEmpty()) {
            rootHostId = targets.constFirst().id;
            QString lookupError;
            const std::optional<HostRecord> savedHost =
                m_database->findHost(rootHostId, &lookupError);
            if (!savedHost.has_value() || savedHost->kind != HostNodeKind::Host) {
                // 临时连接的随机 UUID 无法在下次启动时还原地址、跳板链和凭据引用。静默把它
                // 保存成本地 Shell 会让用户误以为工作区完整，故在写库前整体拒绝。
                QMessageBox::warning(
                    this,
                    tr("The workspace contains temporary connections"),
                    lookupError.isEmpty()
                        ? tr("Tab '%1' is not a persistent host opened from the host tree. Save this connection to the host tree first, then save the workspace.")
                              .arg(m_tabs->tabText(index))
                        : tr("Failed to check the host record for tab '%1': %2")
                              .arg(m_tabs->tabText(index), lookupError));
                return;
            }
        }

        QJsonObject tab;
        tab.insert(QStringLiteral("title"), m_tabs->tabText(index));
        tab.insert(QStringLiteral("hostId"), rootHostId);
        tab.insert(QStringLiteral("filesVisible"), workspace->isFileManagerVisible());
        tabs.append(tab);
    }
    if (tabs.isEmpty()) {
        QMessageBox::information(this, tr("No tabs to save"), tr("The current window has no valid terminal tab."));
        return;
    }

    bool accepted = false;
    const QString name = QInputDialog::getText(this,
                                               tr("Save workspace"),
                                               tr("Workspace name:"),
                                               QLineEdit::Normal,
                                               QString(),
                                               &accepted).trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }

    QString workspaceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    QString error;
    const QList<WorkspaceRecord> existing = m_database->listWorkspaces(&ok, &error);
    if (!ok) {
        QMessageBox::critical(this, tr("Failed to save workspace"), error);
        return;
    }
    const auto sameName = std::find_if(
        existing.cbegin(), existing.cend(), [&name](const WorkspaceRecord &record) {
            return record.name.compare(name, Qt::CaseInsensitive) == 0;
        });
    if (sameName != existing.cend()) {
        if (QMessageBox::question(this,
                                  tr("Overwrite existing workspace"),
                                  tr("A workspace named '%1' already exists. Overwrite it with the current set of tabs?").arg(name),
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes) {
            return;
        }
        workspaceId = sameName->id;
    }

    QJsonObject layout;
    layout.insert(QStringLiteral("version"), 1);
    layout.insert(QStringLiteral("tabs"), tabs);
    WorkspaceRecord record;
    record.id = workspaceId;
    record.name = name;
    record.layoutJson = QString::fromUtf8(QJsonDocument(layout).toJson(QJsonDocument::Compact));
    record.updatedAt = QDateTime::currentDateTimeUtc();
    if (!m_database->saveWorkspace(record, &error)) {
        QMessageBox::critical(this, tr("Failed to save workspace"), error);
        return;
    }
    showTransientMessage(tr("Workspace '%1' saved (%2 tabs)").arg(name).arg(tabs.size()),
                         5000);
}

void MainWindow::openSavedWorkspace()
{
    bool ok = false;
    QString error;
    const QList<WorkspaceRecord> records = m_database->listWorkspaces(&ok, &error);
    if (!ok) {
        QMessageBox::critical(this, tr("Failed to read workspace"), error);
        return;
    }
    if (records.isEmpty()) {
        QMessageBox::information(this, tr("No workspaces"), tr("Save the current workspace first."));
        return;
    }

    QStringList labels;
    QHash<QString, QString> idByLabel;
    for (const WorkspaceRecord &record : records) {
        const QString label = tr("%1  ·  %2")
                                  .arg(record.name,
                                       record.updatedAt.toLocalTime().toString(
                                           QStringLiteral("yyyy-MM-dd HH:mm")));
        // 名称和更新时间都可能相同，附加短 id 保证 QInputDialog 返回文本可无歧义映射。
        QString uniqueLabel = label;
        if (idByLabel.contains(uniqueLabel)) {
            uniqueLabel += QStringLiteral("  [%1]").arg(record.id.left(8));
        }
        labels.append(uniqueLabel);
        idByLabel.insert(uniqueLabel, record.id);
    }
    bool accepted = false;
    const QString selectedLabel = QInputDialog::getItem(this,
                                                        tr("Open workspace"),
                                                        tr("Select workspace:"),
                                                        labels,
                                                        0,
                                                        false,
                                                        &accepted);
    if (!accepted || selectedLabel.isEmpty()) {
        return;
    }
    const std::optional<WorkspaceRecord> selected =
        m_database->findWorkspace(idByLabel.value(selectedLabel), &error);
    if (!selected.has_value()) {
        QMessageBox::critical(this,
                              tr("Failed to read workspace"),
                              error.isEmpty() ? tr("The selected workspace has been deleted") : error);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        selected->layoutJson.toUtf8(), &parseError);
    const QJsonObject layout = document.object();
    const QJsonArray serializedTabs = layout.value(QStringLiteral("tabs")).toArray();
    if (!document.isObject() || layout.value(QStringLiteral("version")).toInt(-1) != 1
        || serializedTabs.isEmpty() || serializedTabs.size() > 64) {
        QMessageBox::critical(
            this,
            tr("The workspace layout is corrupt"),
            tr("The version, tab list, or count of workspace '%1' is invalid; the current session was not modified.")
                .arg(selected->name));
        return;
    }

    struct RestoredTab {
        QString title;
        QString hostId;
        bool filesVisible = true;
        QList<ConnectionTarget> connectionPlan;
    };
    QList<RestoredTab> restoredTabs;
    restoredTabs.reserve(serializedTabs.size());
    for (const QJsonValue &value : serializedTabs) {
        if (!value.isObject()) {
            error = tr("The tag entry is not an object");
            break;
        }
        const QJsonObject tab = value.toObject();
        if (!tab.value(QStringLiteral("title")).isString()
            || !tab.value(QStringLiteral("hostId")).isString()
            || !tab.value(QStringLiteral("filesVisible")).isBool()) {
            error = tr("Invalid tag field type");
            break;
        }
        RestoredTab restored;
        restored.title = tab.value(QStringLiteral("title")).toString().trimmed();
        restored.hostId = tab.value(QStringLiteral("hostId")).toString().trimmed();
        restored.filesVisible = tab.value(QStringLiteral("filesVisible")).toBool();
        if (restored.title.isEmpty()) {
            restored.title = tr("Terminal");
        }
        if (!restored.hostId.isEmpty()) {
            QString hostError;
            const std::optional<HostRecord> host =
                m_database->resolveHost(restored.hostId, &hostError);
            if (!host.has_value() || host->kind != HostNodeKind::Host) {
                error = hostError.isEmpty()
                    ? tr("The host reference does not exist: %1").arg(restored.hostId)
                    : hostError;
                break;
            }
            const QList<HostRecord> route = m_database->resolveConnectionChain(
                restored.hostId, &hostError);
            if (route.isEmpty()) {
                error = hostError.isEmpty()
                    ? tr("The host chain could not be resolved: %1").arg(restored.hostId)
                    : hostError;
                break;
            }
            for (const HostRecord &routeHost : route) {
                ConnectionTarget target = routeHost.toConnectionTarget();
                if (route.size() > 1) {
                    target.proxyJumps.clear();
                }
                restored.connectionPlan.append(target);
            }
        }
        restoredTabs.append(restored);
    }
    if (!error.isEmpty() || restoredTabs.size() != serializedTabs.size()) {
        QMessageBox::critical(
            this,
            tr("The workspace layout is corrupt"),
            tr("Workspace '%1' could not be fully restored: %2. The current session was not modified.")
                .arg(selected->name, error.isEmpty() ? parseError.errorString() : error));
        return;
    }

    if (QMessageBox::question(
            this,
            tr("Replace the current workspace"),
            tr("Opening '%1' closes the current %2 tabs and their connections, then restores %3 tabs. Continue?")
                .arg(selected->name)
                .arg(m_tabs->count())
                .arg(restoredTabs.size()),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    while (m_tabs->count() > 0) {
        closeWorkspaceTab(0, true);
    }
    for (const RestoredTab &restored : std::as_const(restoredTabs)) {
        TerminalWorkspace *workspace = createWorkspaceTab(restored.title);
        workspace->setFileManagerVisible(restored.filesVisible);
        m_tabs->setCurrentWidget(workspace);
        if (restored.hostId.isEmpty()) {
            workspace->activePane()->openLocalShell();
        } else {
            connectSavedHost(restored.hostId, restored.connectionPlan);
            const int index = m_tabs->indexOf(workspace);
            if (index >= 0) {
                // connectSavedHost 默认使用主机显示名；工作区显式保存的标签名优先。
                setWorkspaceTabText(index, restored.title);
            }
        }
    }
    m_tabs->setCurrentIndex(0);
    m_workspace = activeWorkspace();
    showTransientMessage(tr("Workspace '%1' restored").arg(selected->name), 5000);
}

void MainWindow::removeSavedWorkspace()
{
    bool ok = false;
    QString error;
    const QList<WorkspaceRecord> records = m_database->listWorkspaces(&ok, &error);
    if (!ok) {
        QMessageBox::critical(this, tr("Failed to read workspace"), error);
        return;
    }
    if (records.isEmpty()) {
        QMessageBox::information(this, tr("No workspaces"), tr("There are no workspaces to delete."));
        return;
    }
    QStringList labels;
    QHash<QString, QString> idByLabel;
    for (const WorkspaceRecord &record : records) {
        QString label = QStringLiteral("%1  [%2]").arg(record.name, record.id.left(8));
        labels.append(label);
        idByLabel.insert(label, record.id);
    }
    bool accepted = false;
    const QString selectedLabel = QInputDialog::getItem(this,
                                                        tr("Delete workspace"),
                                                        tr("Select a workspace to delete:"),
                                                        labels,
                                                        0,
                                                        false,
                                                        &accepted);
    if (!accepted || selectedLabel.isEmpty()) {
        return;
    }
    if (QMessageBox::warning(this,
                             tr("Confirm workspace deletion"),
                             tr("Only the saved layout is deleted; the current terminals are not closed. This action cannot be undone."),
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    int removed = 0;
    if (!m_database->removeWorkspace(idByLabel.value(selectedLabel), &removed, &error)) {
        QMessageBox::critical(this, tr("Failed to delete workspace"), error);
        return;
    }
    showTransientMessage(removed > 0 ? tr("Workspace deleted") : tr("The workspace no longer exists"),
                         5000);
}

void MainWindow::executeOnTargets(const QString &command, const QStringList &targetIds)
{
    if (targetIds.isEmpty()) {
        TerminalWorkspace *workspace = activeWorkspace();
        TerminalPane *pane = workspace == nullptr ? nullptr : workspace->activePane();
        if (pane != nullptr) {
            pane->executeCommand(command);
        }
        return;
    }

    int delivered = 0;
    for (TerminalPane *pane : allTerminalPanes()) {
        const ConnectionTarget target = pane->activeTarget();
        if (pane->isConnected() && targetIds.contains(target.id)) {
            pane->executeCommand(command);
            ++delivered;
        }
    }
    if (delivered == 0) {
        showTransientMessage(tr("The selected target is closed or not connected yet"), 5000);
    }
}

void MainWindow::sendBytesToTargets(const QByteArray &bytes,
                                    const QStringList &targetIds,
                                    TerminalPane *excludedSource)
{
    if (bytes.isEmpty() || targetIds.isEmpty()) {
        return;
    }
    for (TerminalPane *pane : allTerminalPanes()) {
        if (pane == excludedSource || !pane->isConnected()) {
            continue;
        }
        if (targetIds.contains(pane->activeTarget().id)) {
            // 接收端直接写 worker，不重发 broadcastSourceInput；否则两个开启广播的标签会
            // 形成 A→B→A 的无限输入回路。
            pane->sendInputBytes(bytes);
        }
    }
}

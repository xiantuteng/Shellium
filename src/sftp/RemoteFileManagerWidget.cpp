#include "sftp/RemoteFileManagerWidget.h"
#include "i18n/Retranslate.h"
#include "ui/UiIcons.h"

#include <QAction>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QPixmap>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

namespace {

/**
 * @brief 远程条目在服务器返回列表中的原始序号，用于"还原默认排序"。
 *
 * Qt::UserRole..UserRole+4 已被路径、是否目录、大小、权限和是否符号链接占用，这里从
 * UserRole+5 起继续，避免与既有取值冲突。
 */
constexpr int kOriginalIndexRole = Qt::UserRole + 5;

/**
 * @brief 条目修改时间的结构化原值，用于按时间排序。
 *
 * 排序必须比较 QDateTime 原值而不是格式化后的文本：按字典序比较字符串会把跨年份、
 * 跨月份的日期排错。
 */
constexpr int kModifiedAtRole = Qt::UserRole + 6;

/**
 * @brief 远程文件树"名称"列允许收缩到的最小宽度。
 *
 * 名称是唯一无法预估长度、也最需要看全的一列；面板变窄时其余列已经按内容取了最小宽度，
 * 不能再从名称列这里继续挤占空间——宁可出现横向滚动条，也不能让文件名缩到看不清。
 */
constexpr int kRemoteNameColumnMinWidth = 190;

/**
 * @brief 三路比较，返回负/零/正，供排序比较器统一表达"小于/等于/大于"。
 *
 * 直接写 a < b 的比较器在需要"相等则回落到次级条件"时会退化成两次比较，用三路结果
 * 表达可以让次级条件只写一次。
 */
template <typename T>
int compareValues(const T &left, const T &right)
{
    if (left < right) {
        return -1;
    }
    return right < left ? 1 : 0;
}

/** @brief 把秒数格式化成人类可读的剩余时间。 */
QString formatDuration(const double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return QObject::tr("Unknown");
    }
    const qint64 total = qint64(seconds + 0.5);
    if (total < 60) {
        return QObject::tr("%1 s").arg(total);
    }
    if (total < 3600) {
        return QObject::tr("%1 min %2 s").arg(total / 60).arg(total % 60);
    }
    return QObject::tr("%1 h %2 min").arg(total / 3600).arg((total % 3600) / 60);
}

/** @brief 把字节数格式化为适合文件列表展示的 IEC 单位。 */
QString formatSize(const quint64 bytes)
{
    static const QStringList units = {
        QStringLiteral("B"), QStringLiteral("KiB"), QStringLiteral("MiB"),
        QStringLiteral("GiB"), QStringLiteral("TiB")
    };
    double value = static_cast<double>(bytes);
    qsizetype unitIndex = 0;
    while (value >= 1024.0 && unitIndex + 1 < units.size()) {
        value /= 1024.0;
        ++unitIndex;
    }
    return unitIndex == 0
        ? QStringLiteral("%1 %2").arg(bytes).arg(units.at(unitIndex))
        : QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(units.at(unitIndex));
}

/** @brief 把 POSIX 权限位格式化成固定四位八进制，包含 setuid/setgid/sticky 位。 */
QString formatPermissions(const quint32 permissions)
{
    return QString::number(permissions & 07777U, 8).rightJustified(4, QLatin1Char('0'));
}

/**
 * @brief 判断预览内容是否适合按只读 UTF-8 文本展示。
 *
 * MIME 仅由文件名和少量内容推断，不能作为安全边界；这里额外拒绝 NUL 和密集控制字符，
 * 避免把二进制数据塞进富文本/终端控件。预览最终使用 QPlainTextEdit，不解释 HTML 或脚本。
 */
bool looksLikeTextPreview(const QByteArray &data)
{
    if (data.contains('\0')) {
        return false;
    }
    qsizetype unsafeControls = 0;
    for (const unsigned char byte : data) {
        if ((byte < 0x20 && byte != '\n' && byte != '\r' && byte != '\t')
            || byte == 0x7f) {
            ++unsafeControls;
        }
    }
    return data.isEmpty() || unsafeControls * 100 <= data.size();
}

using SftpRecursive::joinRemotePath;

/** @brief 远程条目拖向本地栏时使用的进程内 MIME 类型。 */
constexpr char kRemotePathMimeType[] = "application/x-shellium-remote-path";

/** @brief 与远程路径配套的目录标志，避免仅凭扩展名猜测条目类型。 */
constexpr char kRemoteDirectoryMimeType[] = "application/x-shellium-remote-directory";

/**
 * @brief 返回远程文件列表使用的目录或普通文件图标。
 * @param context 图标所属控件，用于高 DPI 绘制与生命周期跟踪，不得为空。
 * @param directory true 返回蓝色文件夹，false 返回弱化灰色文件图标。
 * @return 与参考设计一致、跨平台不依赖系统图标主题的远程文件线性图标。
 *
 * 远程条目没有本地 QFileIconProvider 可用，因此使用稳定的 Shellium 图标。该函数刻意不用于
 * 本地栏：本地侧保留操作系统原生文件图标，可以让用户在双栏对拷时快速区分文件来源。
 */
QIcon remoteFileEntryIcon(QWidget *context, const bool directory)
{
    Q_ASSERT(context != nullptr);
    return UiIcons::symbol(context,
                           directory ? UiIcons::Symbol::Folder : UiIcons::Symbol::File,
                           directory ? QColor(QStringLiteral("#5b95d6"))
                                     : QColor(QStringLiteral("#9aa6b2")),
                           QSize(16, 16));
}

/**
 * @brief 把文件管理高频动作配置为带完整辅助说明的紧凑图标按钮。
 * @param button 待配置按钮；不得为空。
 * @param icon 语义图标，已包含跨平台回退资源。
 * @param accessibleName 完整中文动作名称，同时用于 Tooltip 和无障碍树。
 *
 * 文件管理器经常与终端并排，文字工具条会比文件内容占用更多宽度。图标按钮仍通过
 * Tooltip、AccessibleName 与右键菜单提供完整文字，兼顾窄布局、键鼠发现性和辅助技术。
 */
void configureCompactFileButton(QToolButton *button,
                                const QIcon &icon,
                                const QString &accessibleName)
{
    Q_ASSERT(button != nullptr);
    button->setText(accessibleName);
    button->setIcon(icon);
    button->setIconSize(QSize(17, 17));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(false);
    button->setFixedSize(34, 34);
    button->setProperty("compactIconButton", true);
    button->setToolTip(accessibleName);
    button->setAccessibleName(accessibleName);
}

/**
 * @brief 配置双栏路径工具条里的小型图标按钮。
 * @param button 待配置按钮，不得为空。
 * @param icon 返回上级或刷新图标。
 * @param accessibleName Tooltip 与辅助技术读取的完整动作名称。
 *
 * 路径工具条需要把“栏标题、返回、刷新、地址”放在同一行。这里使用 28px 点击区，在不
 * 破坏单行节奏的前提下仍保留足够的鼠标命中面积。
 */
void configurePaneNavigationButton(QToolButton *button,
                                   const QIcon &icon,
                                   const QString &accessibleName)
{
    Q_ASSERT(button != nullptr);
    button->setText(accessibleName);
    button->setIcon(icon);
    button->setIconSize(QSize(15, 15));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(false);
    button->setFixedSize(28, 28);
    button->setProperty("sftpPaneAction", true);
    button->setToolTip(accessibleName);
    button->setAccessibleName(accessibleName);
}

/**
 * @brief 配置操作栏里可随宽度切换文字的文件动作按钮。
 * @param button 待配置按钮，不得为空。
 * @param icon 动作语义图标。
 * @param text 用户可见的简短动作名称。
 * @param role 视觉角色：transfer、file 或 danger，由全局 QSS 精确着色。
 *
 * 业务动作和响应式展示分离：按钮始终保留 text、Tooltip 与 AccessibleName，窄态只改变
 * ToolButtonStyle，不清空文字，切回全屏时无需重新构造菜单或信号连接。
 */
void configureResponsiveFileAction(QToolButton *button,
                                   const QIcon &icon,
                                   const QString &text,
                                   const QString &role)
{
    Q_ASSERT(button != nullptr);
    button->setText(text);
    button->setIcon(icon);
    button->setIconSize(QSize(16, 16));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(false);
    button->setMinimumHeight(32);
    button->setProperty("sftpResponsiveAction", true);
    button->setProperty("sftpActionRole", role);
    button->setToolTip(text);
    button->setAccessibleName(text);
}

/**
 * @brief 创建操作行里的一条竖向分隔线。
 * @param parent 父控件，负责其生命周期。
 *
 * 操作行把"传输""文件操作""传输控制"三组动作放在同一行，从左到右符合从常用到不
 * 常用的操作频率；不加分隔线时六七个紧挨着的图标按钮很难一眼看出分组边界。
 */
QFrame *createOperationRowSeparator(QWidget *parent)
{
    auto *separator = new QFrame(parent);
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Plain);
    separator->setFixedWidth(1);
    separator->setObjectName(QStringLiteral("SftpOperationSeparator"));
    return separator;
}

/**
 * @brief 切换文件树的拖放高亮动态属性，并让样式表立即按新状态重新生效。
 * @param tree 目标文件树；本地栏、远程栏共用同一份逻辑。
 * @param active true 表示合法拖拽正悬停在该栏上方，应显示放置目标框。
 *
 * setProperty 之后必须 unpolish/polish 才能让 QSS 的属性选择器（如
 * `[dragActive="true"]`）重新求值——仅调用 update() 只会重绘已经算好的样式。
 */
void setDragTargetActive(QWidget *tree, const bool active)
{
    if (tree->property("dragActive").toBool() == active) {
        return;
    }
    tree->setProperty("dragActive", active);
    tree->style()->unpolish(tree);
    tree->style()->polish(tree);
    tree->update();
}

/**
 * @brief 支持把本地文件系统 URL 拖入的远程文件树。
 *
 * 控件只负责把 Qt 拖放事件转换成路径列表；服务器选择、路径校验和实际 SFTP 操作仍由
 * RemoteFileManagerWidget 统一处理，避免视图类持有凭据或网络对象。落点始终是当前浏览
 * 目录（见 dropEvent），因此不再需要解析鼠标悬停的具体条目。
 */
class RemoteDropTree final : public QTreeWidget {
public:
    using LocalDropHandler = std::function<void(const QStringList &localPaths)>;

    explicit RemoteDropTree(QWidget *parent = nullptr)
        : QTreeWidget(parent)
    {
        setAcceptDrops(true);
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::CopyAction);
    }

    /** @brief 注入本地路径落到远端树后的业务处理函数。 */
    void setLocalDropHandler(LocalDropHandler handler)
    {
        m_localDropHandler = std::move(handler);
    }

protected:
    /** @brief 只接受本地文件 URL；远程树内部不允许通过拖拽重排。 */
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            setDragTargetActive(this, true);
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    /** @brief 在合法本地文件拖过时持续显示可放置反馈。 */
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    /** @brief 拖拽离开控件而未落下时撤销放置反馈，避免高亮框一直停留在栏上。 */
    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        setDragTargetActive(this, false);
        QTreeWidget::dragLeaveEvent(event);
    }

    /**
     * @brief 收集本地绝对路径，一律上传到当前远程目录。
     *
     * 刻意不看鼠标落在哪个条目上：目录列表随时可能因为刷新、排序而与用户记忆中的
     * 行位置错位，"永远拖到当前目录"比"猜测悬停的文件夹"更可预期，也和大多数文件
     * 管理器窗口内拖拽的直觉一致。
     */
    void dropEvent(QDropEvent *event) override
    {
        setDragTargetActive(this, false);
        if (!event->mimeData()->hasUrls() || !m_localDropHandler) {
            event->ignore();
            return;
        }
        QStringList localPaths;
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                localPaths.append(QDir::cleanPath(url.toLocalFile()));
            }
        }
        if (localPaths.isEmpty()) {
            event->ignore();
            return;
        }
        m_localDropHandler(localPaths);
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }

    /** @brief 把当前远程条目的完整路径和目录类型编码为自定义拖拽数据。 */
    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions)
        QTreeWidgetItem *item = currentItem();
        if (item == nullptr) {
            return;
        }
        auto *mimeData = new QMimeData;
        mimeData->setData(kRemotePathMimeType,
                          item->data(0, Qt::UserRole).toString().toUtf8());
        mimeData->setData(kRemoteDirectoryMimeType,
                          item->data(0, Qt::UserRole + 1).toBool()
                              ? QByteArrayLiteral("1")
                              : QByteArrayLiteral("0"));
        auto *drag = new QDrag(this);
        drag->setMimeData(mimeData);
        drag->exec(Qt::CopyAction);
    }

private:
    /** @brief 收到合法本地 URL 拖入后调用的业务回调。 */
    LocalDropHandler m_localDropHandler;
};

/**
 * @brief 接收 Shellium 远程条目并把目标解析为本地目录的文件树。
 *
 * 拖入动作仅代表“下载副本”，绝不会调用本地移动或删除；这对运维文件管理尤为重要，因为
 * 一次误判为 MoveAction 就可能破坏用户本机原文件。落点一律是外层持有的 m_localPathEdit
 * 当前目录，不解析鼠标悬停的具体条目——见 dropEvent 注释。
 */
class LocalDropTree final : public QTreeWidget {
public:
    using RemoteDropHandler = std::function<void(const QString &remotePath, bool directory)>;

    explicit LocalDropTree(QWidget *parent = nullptr)
        : QTreeWidget(parent)
    {
        setAcceptDrops(true);
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::CopyAction);
    }

    /** @brief 注入远程条目落到本地树后的下载处理函数。 */
    void setRemoteDropHandler(RemoteDropHandler handler)
    {
        m_remoteDropHandler = std::move(handler);
    }

protected:
    /** @brief 只接收本控件定义的远程条目格式，本地内部拖拽不会触发文件移动。 */
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasFormat(kRemotePathMimeType)) {
            setDragTargetActive(this, true);
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    /** @brief 在远程条目拖过时持续显示复制反馈。 */
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData()->hasFormat(kRemotePathMimeType)) {
            event->acceptProposedAction();
            return;
        }
        event->ignore();
    }

    /** @brief 拖拽离开控件而未落下时撤销放置反馈，避免高亮框一直停留在栏上。 */
    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        setDragTargetActive(this, false);
        QTreeWidget::dragLeaveEvent(event);
    }

    /**
     * @brief 把远程路径交给业务层，一律下载到当前本地目录。
     *
     * 与 RemoteDropTree::dropEvent 对称：不再按鼠标落点是否为目录条目决定目标，始终
     * 下载到 m_localPathEdit 显示的当前目录，避免"以为拖进了子目录，其实那一行早已
     * 因为排序或刷新挪了位置"这种误操作。
     */
    void dropEvent(QDropEvent *event) override
    {
        setDragTargetActive(this, false);
        if (!event->mimeData()->hasFormat(kRemotePathMimeType)
            || !m_remoteDropHandler) {
            event->ignore();
            return;
        }
        const QString remotePath = QString::fromUtf8(
            event->mimeData()->data(kRemotePathMimeType));
        const bool directory =
            event->mimeData()->data(kRemoteDirectoryMimeType) == QByteArrayLiteral("1");
        m_remoteDropHandler(remotePath, directory);
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }

    /** @brief 把当前选中本地条目编码为标准 file:// URL，供远程栏拖拽上传识别。 */
    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions)
        QTreeWidgetItem *item = currentItem();
        if (item == nullptr) {
            return;
        }
        auto *mimeData = new QMimeData;
        mimeData->setUrls({QUrl::fromLocalFile(item->data(0, Qt::UserRole).toString())});
        auto *drag = new QDrag(this);
        drag->setMimeData(mimeData);
        drag->exec(Qt::CopyAction);
    }

private:
    /** @brief 收到合法远程条目拖入后调用的业务回调。 */
    RemoteDropHandler m_remoteDropHandler;
};

/**
 * @brief 获得焦点时自动全选已有文字的路径输入框。
 *
 * 本地/远程路径框里几乎总是装着一条完整绝对路径；用户点进去多半是想整段替换成新路径，
 * 而不是在中间插入光标手动删。全选放在 QTimer::singleShot(0, ...) 里执行，是因为
 * focusInEvent 触发时鼠标点击导致的光标定位还没发生——同帧内 selectAll 会被那次定位
 * 覆盖掉，必须让它排到事件队列最后才能生效。
 */
class SelectAllOnFocusLineEdit final : public QLineEdit {
public:
    using QLineEdit::QLineEdit;

protected:
    void focusInEvent(QFocusEvent *event) override
    {
        QLineEdit::focusInEvent(event);
        QTimer::singleShot(0, this, &QLineEdit::selectAll);
    }
};

/**
 * @brief 把 QFileInfo::permissions() 换算成 POSIX 八进制权限位。
 *
 * Qt 的跨平台权限标志按 owner/group/other 三组暴露读写执行位，数值巧合地与 POSIX
 * mode 位对齐，可以直接按位或拼出来复用 RemoteFileInfo::permissionText() 的符号
 * 渲染；Qt 没有跨平台途径读取 setuid/setgid/sticky，因此这三位固定为 0。
 */
quint32 localPermissionBits(const QFileInfo &info)
{
    const QFileDevice::Permissions permissions = info.permissions();
    quint32 bits = 0;
    if (permissions & QFileDevice::ReadOwner) bits |= 0400U;
    if (permissions & QFileDevice::WriteOwner) bits |= 0200U;
    if (permissions & QFileDevice::ExeOwner) bits |= 0100U;
    if (permissions & QFileDevice::ReadGroup) bits |= 0040U;
    if (permissions & QFileDevice::WriteGroup) bits |= 0020U;
    if (permissions & QFileDevice::ExeGroup) bits |= 0010U;
    if (permissions & QFileDevice::ReadOther) bits |= 0004U;
    if (permissions & QFileDevice::WriteOther) bits |= 0002U;
    if (permissions & QFileDevice::ExeOther) bits |= 0001U;
    return bits;
}

/**
 * @brief 按 正序→倒序→默认 三态语义重排一棵文件树的顶层条目。
 *
 * 本地栏与远程栏的列布局（名称/大小/修改时间/权限/所有者）和条目角色数据完全一致，
 * 比较逻辑因此可以共用同一份实现，不必为两棵树各写一遍。
 * @param tree 待重排的文件树；名称在列 0，大小在列 1，修改时间在列 2，权限在列 3，
 *             所有者在列 4，目录标记与原始序号见 kOriginalIndexRole 一带的角色约定。
 * @param column 排序列，-1 表示还原为条目入树时的原始顺序。
 * @param order 排序方向，仅在 column >= 0 时生效。
 */
void sortFileTreeItems(QTreeWidget *tree, const int column, const Qt::SortOrder order)
{
    if (tree->topLevelItemCount() == 0) {
        return;
    }

    QList<QTreeWidgetItem *> items;
    items.reserve(tree->topLevelItemCount());
    while (tree->topLevelItemCount() > 0) {
        items.append(tree->takeTopLevelItem(0));
    }

    const bool descending = order == Qt::DescendingOrder;
    std::stable_sort(items.begin(), items.end(),
                     [column, descending](const QTreeWidgetItem *left,
                                          const QTreeWidgetItem *right) {
        if (column < 0) {
            // 还原默认：完全按入树顺序，不再做任何分组。
            return left->data(0, kOriginalIndexRole).toInt()
                < right->data(0, kOriginalIndexRole).toInt();
        }

        // 目录恒排在文件之前。文件管理的首要用途是逐层进入目录，若按大小排序把目录
        // 冲散到列表中间，导航会变得非常费力；这也是各类文件管理器的一致做法。
        const bool leftIsDirectory = left->data(0, Qt::UserRole + 1).toBool();
        const bool rightIsDirectory = right->data(0, Qt::UserRole + 1).toBool();
        if (leftIsDirectory != rightIsDirectory) {
            return leftIsDirectory;
        }

        int comparison = 0;
        switch (column) {
        case 1:
            comparison = compareValues(left->data(0, Qt::UserRole + 2).toULongLong(),
                                       right->data(0, Qt::UserRole + 2).toULongLong());
            break;
        case 2:
            comparison = compareValues(left->data(0, kModifiedAtRole).toDateTime(),
                                       right->data(0, kModifiedAtRole).toDateTime());
            break;
        case 3:
            comparison = compareValues(left->data(0, Qt::UserRole + 3).toUInt(),
                                       right->data(0, Qt::UserRole + 3).toUInt());
            break;
        case 4:
            comparison = QString::compare(left->text(4), right->text(4), Qt::CaseInsensitive);
            break;
        case 0:
        default:
            comparison = QString::compare(left->text(0), right->text(0), Qt::CaseInsensitive);
            break;
        }
        if (comparison == 0) {
            // 同值时回落到名称，保证结果稳定可预期，不随刷新前后跳动。
            comparison = QString::compare(left->text(0), right->text(0), Qt::CaseInsensitive);
        }
        return descending ? comparison > 0 : comparison < 0;
    });

    tree->addTopLevelItems(items);
}

} // namespace

RemoteFileManagerWidget::RemoteFileManagerWidget(QWidget *parent)
    : QWidget(parent)
    , m_targetCombo(new QComboBox(this))
    , m_followInnermostCheck(new QCheckBox(tr("Follow the innermost layer automatically"), this))
    , m_followTerminalCwdCheck(new QCheckBox(tr("Follow terminal directory"), this))
    , m_fullScreenButton(new QToolButton(this))
    , m_closeButton(new QToolButton(this))
    , m_localPathEdit(new SelectAllOnFocusLineEdit(QDir::homePath(), this))
    , m_localTree(new LocalDropTree(this))
    , m_pathEdit(new SelectAllOnFocusLineEdit(QStringLiteral("/"), this))
    , m_fileTree(new RemoteDropTree(this))
    , m_statusLabel(new QLabel(tr("Select a terminal server"), this))
    , m_progressBar(new QProgressBar(this))
    , m_cancelButton(new QToolButton(this))
    , m_pauseButton(new QToolButton(this))
    , m_resumeTransfersCheck(new QCheckBox(tr("Prefer resumable transfer"), this))
    , m_transferQueueList(new QListWidget(this))
    , m_service(new SftpService(this))
{
    setObjectName(QStringLiteral("RemoteFileManager"));
    auto *mainLayout = new QVBoxLayout(this);
    // 顶栏、文件区、操作栏与传输信息带直接首尾相接，由各自边框形成分区。若继续保留外层
    // 10px 空隙，每一段都会像孤立控件漂在背景上，无法形成参考图中的完整文件管理面板。
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_targetRow = new QWidget(this);
    m_targetRow->setObjectName(QStringLiteral("SftpTopBar"));
    auto *targetRowLayout = new QHBoxLayout(m_targetRow);
    targetRowLayout->setContentsMargins(14, 12, 14, 12);
    targetRowLayout->setSpacing(10);
    auto *serverLabel = new QLabel(tr("Server"), m_targetRow);
    serverLabel->setObjectName(QStringLiteral("SftpServerLabel"));
    UiIcons::modernizePopup(m_targetCombo);
    m_targetCombo->setObjectName(QStringLiteral("SftpTargetCombo"));
    m_targetCombo->setMinimumWidth(260);
    m_targetCombo->setMaximumWidth(520);
    m_targetCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    targetRowLayout->addWidget(serverLabel);
    targetRowLayout->addWidget(m_targetCombo);

    // 自动跟随最内层是唯一合理的默认行为，不再作为可见选项占用顶部空间；它退化为内部
    // 状态：用户一旦在下拉框里手动选择外层，这里会自动置否，避免后续刷新把人抢回内层。
    m_followInnermostCheck->setChecked(true);
    m_followInnermostCheck->setVisible(false);

    m_fullScreenButton->setCheckable(true);
    m_fullScreenButton->setChecked(false);
    m_fullScreenButton->setAutoRaise(true);
    m_fullScreenButton->setIconSize(QSize(18, 18));
    m_fullScreenButton->setFixedSize(32, 32);
    m_fullScreenButton->setProperty("sftpFullScreenAction", true);
    m_fullScreenButton->setObjectName(QStringLiteral("SftpFullScreenButton"));
    m_fullScreenButton->setToolTip(tr("Show the file manager full screen and expand local files for side-by-side copying"));
    m_fullScreenButton->setAccessibleName(tr("Full screen"));
    m_closeButton->setAutoRaise(true);
    m_closeButton->setIconSize(QSize(18, 18));
    m_closeButton->setFixedSize(32, 32);
    m_closeButton->setProperty("sftpFullScreenAction", true);
    m_closeButton->setObjectName(QStringLiteral("SftpCloseButton"));
    m_closeButton->setToolTip(tr("Close the remote file manager and give all the width back to the terminal"));
    m_closeButton->setAccessibleName(tr("Close"));
    m_closeButton->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Close, this->palette().color(QPalette::WindowText)));
    connect(m_closeButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::closeRequested);
    targetRowLayout->addStretch(1);
    targetRowLayout->addWidget(m_fullScreenButton);
    targetRowLayout->addWidget(m_closeButton);
    mainLayout->addWidget(m_targetRow);

    auto *contentSplitter = new QSplitter(Qt::Horizontal, this);
    contentSplitter->setObjectName(QStringLiteral("SftpPaneSplitter"));
    contentSplitter->setHandleWidth(1);
    m_localPanel = new QWidget(contentSplitter);
    m_localPanel->setObjectName(QStringLiteral("SftpLocalPane"));
    auto *localPanel = m_localPanel;
    auto *localLayout = new QVBoxLayout(localPanel);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->setSpacing(0);
    auto *localToolbar = new QWidget(localPanel);
    localToolbar->setObjectName(QStringLiteral("SftpLocalToolbar"));
    auto *localPathRow = new QHBoxLayout(localToolbar);
    localPathRow->setContentsMargins(10, 8, 10, 8);
    localPathRow->setSpacing(6);
    auto *localCaption = new QLabel(tr("Local files"), localToolbar);
    localCaption->setObjectName(QStringLiteral("SftpPaneLabel"));
    localCaption->setProperty("sectionCaption", true);
    auto *localUpButton = new QToolButton(localToolbar);
    localUpButton->setObjectName(QStringLiteral("SftpLocalUpButton"));
    auto *localRefreshButton = new QToolButton(localToolbar);
    localRefreshButton->setObjectName(QStringLiteral("SftpLocalRefreshButton"));
    configurePaneNavigationButton(
        localUpButton,
        UiIcons::symbol(this, UiIcons::Symbol::ArrowUp, this->palette().color(QPalette::WindowText)),
        tr("Go to the local parent directory"));
    configurePaneNavigationButton(
        localRefreshButton,
        UiIcons::symbol(this, UiIcons::Symbol::Refresh, this->palette().color(QPalette::WindowText)),
        tr("Refresh local directory"));
    m_localPathEdit->setObjectName(QStringLiteral("SftpLocalPathEdit"));
    localPathRow->addWidget(localCaption);
    localPathRow->addWidget(localUpButton);
    localPathRow->addWidget(localRefreshButton);
    localPathRow->addWidget(m_localPathEdit, 1);
    localLayout->addWidget(localToolbar);

    // 本地栏按需重新列举目录内容，保持只读可确保拖放只触发 Shellium 的上传/下载，不会
    // 意外在本机执行移动、重命名或删除。
    m_localTree->setObjectName(QStringLiteral("LocalFileTree"));
    m_localTree->setColumnCount(5);
    // 双栏对拷时，本地侧和远程侧要回答同一个问题——"这两边是不是同一个文件"。列布局与
    // 远程栏保持一致（名称/大小/修改时间/权限/所有者），排序比较逻辑因此可以共用
    // sortFileTreeItems，两栏也能逐列直接对照。
    m_localTree->setHeaderLabels(
        {tr("Name"), tr("Size"), tr("Modified"), tr("Permissions"), tr("Owner")});
    m_localTree->setRootIsDecorated(false);
    m_localTree->setAlternatingRowColors(false);
    m_localTree->setUniformRowHeights(true);
    m_localTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_localTree->setContextMenuPolicy(Qt::CustomContextMenu);
    const QFontMetrics localMetrics(m_localTree->font());
    constexpr int kLocalCellPadding = 18;
    m_localTree->setColumnWidth(0, 190);
    m_localTree->setColumnWidth(
        1, localMetrics.horizontalAdvance(QStringLiteral("1023.9 MiB")) + kLocalCellPadding);
    m_localTree->setColumnWidth(
        2,
        localMetrics.horizontalAdvance(QStringLiteral("2026-08-09 23:56:28"))
            + kLocalCellPadding);
    m_localTree->setColumnWidth(
        3, localMetrics.horizontalAdvance(QStringLiteral("drwxrwxrwt")) + kLocalCellPadding);
    m_localTree->setColumnWidth(
        4, localMetrics.horizontalAdvance(QStringLiteral("root")) + kLocalCellPadding);
    m_localTree->header()->setMinimumSectionSize(40);
    m_localTree->header()->setStretchLastSection(false);
    for (int column = 0; column < m_localTree->columnCount(); ++column) {
        m_localTree->header()->setSectionResizeMode(column, QHeaderView::Interactive);
    }
    // 与远程栏一样自行处理三态排序，而不是 setSortingEnabled 内置的两态排序。
    m_localTree->setSortingEnabled(false);
    m_localTree->header()->setSectionsClickable(true);
    m_localTree->header()->setSortIndicatorShown(false);
    connect(m_localTree->header(), &QHeaderView::sectionClicked,
            this, &RemoteFileManagerWidget::cycleLocalSort);
    m_localTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_localTree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_localTree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    localLayout->addWidget(m_localTree, 1);

    auto *remotePanel = new QWidget(contentSplitter);
    remotePanel->setObjectName(QStringLiteral("SftpRemotePane"));
    auto *remoteLayout = new QVBoxLayout(remotePanel);
    remoteLayout->setContentsMargins(0, 0, 0, 0);
    remoteLayout->setSpacing(0);
    auto *remoteToolbar = new QWidget(remotePanel);
    remoteToolbar->setObjectName(QStringLiteral("SftpRemoteToolbar"));
    auto *pathRow = new QHBoxLayout(remoteToolbar);
    pathRow->setContentsMargins(10, 8, 10, 8);
    pathRow->setSpacing(6);
    auto *remoteCaption = new QLabel(tr("Remote files"), remoteToolbar);
    remoteCaption->setObjectName(QStringLiteral("SftpPaneLabel"));
    remoteCaption->setProperty("sectionCaption", true);
    auto *upButton = new QToolButton(remoteToolbar);
    upButton->setObjectName(QStringLiteral("SftpRemoteUpButton"));
    auto *refreshButton = new QToolButton(remoteToolbar);
    refreshButton->setObjectName(QStringLiteral("SftpRemoteRefreshButton"));
    configurePaneNavigationButton(
        upButton,
        UiIcons::symbol(this, UiIcons::Symbol::ArrowUp, this->palette().color(QPalette::WindowText)),
        tr("Go to the remote parent directory"));
    configurePaneNavigationButton(
        refreshButton,
        UiIcons::symbol(this, UiIcons::Symbol::Refresh, this->palette().color(QPalette::WindowText)),
        tr("Refresh the current remote directory"));
    m_pathEdit->setObjectName(QStringLiteral("SftpRemotePathEdit"));
    m_followTerminalCwdCheck->setObjectName(QStringLiteral("SftpFollowTerminalCwdCheck"));
    m_followTerminalCwdCheck->setChecked(false);
    m_followTerminalCwdCheck->setToolTip(
        tr("When enabled, the path box jumps automatically to the remote directory the left terminal is currently in"));
    pathRow->addWidget(remoteCaption);
    pathRow->addWidget(upButton);
    pathRow->addWidget(refreshButton);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(m_followTerminalCwdCheck);
    remoteLayout->addWidget(remoteToolbar);

    // 目录/链接/普通文件已由名称列的图标区分，独立“类型”列没有保留价值，因此不再单独占列。
    m_fileTree->setObjectName(QStringLiteral("RemoteFileTree"));
    m_fileTree->setColumnCount(5);
    m_fileTree->setHeaderLabels(
        {tr("Name"), tr("Size"), tr("Modified"), tr("Permissions"), tr("Owner")});
    m_fileTree->setRootIsDecorated(false);
    m_fileTree->setAlternatingRowColors(false);
    m_fileTree->setUniformRowHeights(true);
    m_fileTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_fileTree->header()->setStretchLastSection(false);
    // 所有列都交给 Interactive：名称列的宽度改由 updateRemoteNameColumnWidth() 按可用空间
    // 手动计算并下限钳在 kRemoteNameColumnMinWidth，而不是用 Stretch——Stretch 会在面板变
    // 窄时把名称列一路压到 setMinimumSectionSize 的下限，文件名会被挤成看不清的几个字符。
    for (int column = 0; column < m_fileTree->columnCount(); ++column) {
        m_fileTree->header()->setSectionResizeMode(column, QHeaderView::Interactive);
    }
    m_fileTree->header()->setMinimumSectionSize(40);
    // 除名称外的每一列都按"最长的真实内容"取宽，而不是拍脑袋给一个整数：面板本来就只占
    // 窗口的三分之一，多给出去的每一像素都是从文件名那里抢来的。
    const QFontMetrics columnMetrics(m_fileTree->font());
    const auto columnWidthFor = [&columnMetrics](const QString &widestContent,
                                                 const QString &header) {
        // 单元格左右内边距各 5px（见全局样式表），再给排序指示与边框留出余量。
        constexpr int kCellPadding = 18;
        return std::max(columnMetrics.horizontalAdvance(widestContent),
                        columnMetrics.horizontalAdvance(header))
            + kCellPadding;
    };
    const QList<int> defaultColumnWidths = {
        kRemoteNameColumnMinWidth,
        columnWidthFor(QStringLiteral("1023.9 MiB"), tr("Size")),
        columnWidthFor(QStringLiteral("2026-08-09 23:56:28"), tr("Modified")),
        columnWidthFor(QStringLiteral("drwxrwxrwt"), tr("Permissions")),
        columnWidthFor(QStringLiteral("root"), tr("Owner")),
    };
    for (int column = 0; column < defaultColumnWidths.size(); ++column) {
        m_fileTree->setColumnWidth(column, defaultColumnWidths.at(column));
    }
    // 自行处理排序而不是使用 setSortingEnabled：Qt 内置排序只有升/降两态，无法表达
    // 用户要求的"再点一次回到服务器原始顺序"。
    m_fileTree->setSortingEnabled(false);
    m_fileTree->header()->setSectionsClickable(true);
    m_fileTree->header()->setSortIndicatorShown(false);
    connect(m_fileTree->header(), &QHeaderView::sectionClicked,
            this, &RemoteFileManagerWidget::cycleRemoteSort);
    // 宽度不足时显示横向滚动条，而不是隐藏列；按像素滚动可停在任意列中间。
    m_fileTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_fileTree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_fileTree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    remoteLayout->addWidget(m_fileTree, 1);

    contentSplitter->addWidget(localPanel);
    contentSplitter->addWidget(remotePanel);
    contentSplitter->setChildrenCollapsible(false);
    contentSplitter->setStretchFactor(0, 1);
    contentSplitter->setStretchFactor(1, 1);
    contentSplitter->setSizes({420, 520});
    mainLayout->addWidget(contentSplitter, 1);

    // 本地面板只在全屏时出现：并排模式下文件管理只占约三分之一宽度，塞进两棵目录树
    // 会让两边都窄到无法辨认路径。全屏与"双栏对拷"因此是同一个动作。
    m_localPanel->setVisible(false);
    connect(m_fullScreenButton, &QToolButton::toggled, this, [this](const bool fullScreen) {
        m_localPanel->setVisible(fullScreen);
        m_fullScreenButton->setToolTip(
            fullScreen ? tr("Exit full screen and collapse local files") : tr("Show the file manager full screen and expand local files for side-by-side copying"));
        updateFullScreenIcon();
        emit fullScreenChanged(fullScreen);
    });

    auto *operationBar = new QFrame(this);
    operationBar->setObjectName(QStringLiteral("SftpOperationBar"));
    auto *operationRow = new QHBoxLayout(operationBar);
    operationRow->setContentsMargins(14, 10, 14, 10);
    operationRow->setSpacing(5);
    auto *uploadButton = new QToolButton(operationBar);
    uploadButton->setObjectName(QStringLiteral("SftpUploadButton"));
    auto *downloadButton = new QToolButton(operationBar);
    downloadButton->setObjectName(QStringLiteral("SftpDownloadButton"));
    auto *previewButton = new QToolButton(operationBar);
    previewButton->setObjectName(QStringLiteral("SftpPreviewButton"));
    auto *chmodButton = new QToolButton(operationBar);
    chmodButton->setObjectName(QStringLiteral("SftpChmodButton"));
    auto *renameButton = new QToolButton(operationBar);
    renameButton->setObjectName(QStringLiteral("SftpRenameButton"));
    auto *deleteButton = new QToolButton(operationBar);
    deleteButton->setObjectName(QStringLiteral("SftpDeleteButton"));
    configureResponsiveFileAction(
        uploadButton,
        UiIcons::symbol(this, UiIcons::Symbol::Upload, this->palette().color(QPalette::WindowText)),
        tr("Upload"),
        QStringLiteral("transfer"));
    // 文件与目录合成一个按钮。系统文件选择器无法在一次会话里同时接受文件和目录，因此按钮
    // 展开一个二选一菜单，而不是让用户在两个外观相近的图标之间猜。
    auto *uploadMenu = new QMenu(uploadButton);
    UiIcons::modernizePopup(uploadMenu);
    QAction *uploadFilesAction = uploadMenu->addAction(
        UiIcons::symbol(this, UiIcons::Symbol::Send, this->palette().color(QPalette::WindowText)),
        tr("Upload Files…"));
    uploadFilesAction->setObjectName(QStringLiteral("SftpUploadFilesAction"));
    QAction *uploadDirectoryAction = uploadMenu->addAction(
        UiIcons::symbol(this, UiIcons::Symbol::Folder, this->palette().color(QPalette::WindowText)),
        tr("Upload Directory…"));
    uploadDirectoryAction->setObjectName(QStringLiteral("SftpUploadDirectoryAction"));
    connect(uploadFilesAction, &QAction::triggered,
            this, &RemoteFileManagerWidget::uploadSelectedFile);
    connect(uploadDirectoryAction, &QAction::triggered,
            this, &RemoteFileManagerWidget::uploadSelectedDirectory);
    uploadButton->setMenu(uploadMenu);
    uploadButton->setPopupMode(QToolButton::InstantPopup);
    configureResponsiveFileAction(
        downloadButton,
        UiIcons::symbol(this, UiIcons::Symbol::Download, this->palette().color(QPalette::WindowText)),
        tr("Download"),
        QStringLiteral("transfer"));
    configureResponsiveFileAction(
        previewButton,
        UiIcons::symbol(this, UiIcons::Symbol::Eye, this->palette().color(QPalette::WindowText)),
        tr("Preview"),
        QStringLiteral("file"));
    configureResponsiveFileAction(
        chmodButton,
        UiIcons::symbol(this, UiIcons::Symbol::Info, this->palette().color(QPalette::WindowText)),
        tr("Permissions"),
        QStringLiteral("file"));
    configureResponsiveFileAction(
        renameButton,
        UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)),
        tr("Rename"),
        QStringLiteral("file"));
    configureResponsiveFileAction(
        deleteButton,
        UiIcons::symbol(this,
                        UiIcons::Symbol::Trash,
                        QColor(QStringLiteral("#d84a41"))),
        tr("Delete"),
        QStringLiteral("danger"));
    configureCompactFileButton(
        m_pauseButton,
        UiIcons::symbol(this, UiIcons::Symbol::Pause, this->palette().color(QPalette::WindowText)),
        tr("Pause or resume the current transfer"));
    configureCompactFileButton(
        m_cancelButton,
        UiIcons::symbol(this, UiIcons::Symbol::Cancel, this->palette().color(QPalette::WindowText)),
        tr("Cancel the current transfer or the selected waiting task"));
    m_pauseButton->setObjectName(QStringLiteral("SftpPauseButton"));
    m_cancelButton->setObjectName(QStringLiteral("SftpCancelButton"));
    m_cancelButton->setEnabled(false);
    m_pauseButton->setEnabled(false);
    m_cancelButton->setVisible(false);
    m_pauseButton->setVisible(false);
    m_resumeTransfersCheck->setChecked(true);
    m_resumeTransfersCheck->setObjectName(QStringLiteral("SftpResumeCheck"));
    m_resumeTransfersCheck->setText(tr("Resumable transfer"));
    m_resumeTransfersCheck->setToolTip(
        tr("On: files whose content is truly identical are skipped, and partial fragments left by an interruption continue after verification;\nOff: everything is re-uploaded in full regardless of whether content matches."));
    // 操作行按"传输""文件操作""传输控制"三组用竖线隔开，从左到右符合从常用到不常用
    // 的操作频率，用户不用逐个图标猜边界。
    operationRow->addWidget(uploadButton);
    operationRow->addWidget(downloadButton);
    operationRow->addWidget(createOperationRowSeparator(operationBar));
    operationRow->addWidget(previewButton);
    operationRow->addWidget(chmodButton);
    operationRow->addWidget(renameButton);
    operationRow->addWidget(deleteButton);
    operationRow->addStretch(1);
    operationRow->addWidget(m_resumeTransfersCheck);
    m_transferControlSeparator = createOperationRowSeparator(operationBar);
    m_transferControlSeparator->setVisible(false);
    operationRow->addWidget(m_transferControlSeparator);
    operationRow->addWidget(m_pauseButton);
    operationRow->addWidget(m_cancelButton);
    mainLayout->addWidget(operationBar);

    // 进度、当前状态与队列合并在同一信息带：进度条只表达当前项百分比，队列行右侧的状态
    // 徽章表达“进行中/等待/已暂停”，避免旧版同时出现两组相似进度信息。
    m_transferPanel = new QFrame(this);
    m_transferPanel->setObjectName(QStringLiteral("SftpTransferBand"));
    auto *transferLayout = new QVBoxLayout(m_transferPanel);
    transferLayout->setContentsMargins(14, 10, 14, 14);
    transferLayout->setSpacing(8);
    m_progressBar->setObjectName(QStringLiteral("SftpTransferProgress"));
    m_progressBar->setVisible(false);
    m_progressBar->setRange(0, 100);
    m_progressBar->setTextVisible(false);
    transferLayout->addWidget(m_progressBar);

    auto *transferStatusRow = new QHBoxLayout;
    transferStatusRow->setContentsMargins(0, 0, 0, 0);
    transferStatusRow->setSpacing(12);
    m_statusLabel->setObjectName(QStringLiteral("SftpTransferStatus"));
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // 状态消息可能包含完整路径。Ignored 允许布局在窄栏里压缩标签，而不是由一条长路径
    // 反向撑宽整个文件管理器；完整文字仍可通过鼠标选择横向拖动读取。
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_transferDetailLabel = new QLabel(m_transferPanel);
    m_transferDetailLabel->setObjectName(QStringLiteral("SftpTransferDetail"));
    m_transferDetailLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_transferDetailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    transferStatusRow->addWidget(m_statusLabel, 1);
    transferStatusRow->addWidget(m_transferDetailLabel, 0);
    transferLayout->addLayout(transferStatusRow);

    m_transferQueueList->setObjectName(QStringLiteral("SftpTransferQueue"));
    m_transferQueueList->setMaximumHeight(116);
    m_transferQueueList->setAlternatingRowColors(false);
    m_transferQueueList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_transferQueueList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_transferQueueList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_transferQueueList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_transferQueueList->setToolTip(
        tr("Transfers run strictly in order; select a waiting item and click 'Deselect' to remove it directly."));
    transferLayout->addWidget(m_transferQueueList);
    m_transferPanel->setVisible(false);
    mainLayout->addWidget(m_transferPanel);

    connect(refreshButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::refreshDirectory);
    connect(upButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::navigateUp);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, &RemoteFileManagerWidget::refreshDirectory);
    connect(localUpButton, &QToolButton::clicked, this, [this]() {
        setLocalDirectory(QFileInfo(m_localPathEdit->text().trimmed()).absolutePath());
    });
    connect(localRefreshButton, &QToolButton::clicked, this, [this]() {
        setLocalDirectory(m_localPathEdit->text().trimmed());
    });
    connect(m_localPathEdit, &QLineEdit::returnPressed, this, [this]() {
        setLocalDirectory(m_localPathEdit->text().trimmed());
    });
    connect(m_localTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
        if (item != nullptr && item->data(0, Qt::UserRole + 1).toBool()) {
            setLocalDirectory(item->data(0, Qt::UserRole).toString());
        }
    });
    connect(m_localTree,
            &QWidget::customContextMenuRequested,
            this,
            [this](const QPoint &position) {
                QTreeWidgetItem *item = m_localTree->itemAt(position);
                if (item != nullptr) {
                    m_localTree->setCurrentItem(item);
                }
                QMenu menu(this);
                UiIcons::modernizePopup(&menu);
                if (item != nullptr) {
                    const QString localPath = item->data(0, Qt::UserRole).toString();
                    if (item->data(0, Qt::UserRole + 1).toBool()) {
                        QAction *openAction = menu.addAction(
                            UiIcons::symbol(this, UiIcons::Symbol::Folder, this->palette().color(QPalette::WindowText)),
                            tr("Open directory"));
                        connect(openAction, &QAction::triggered,
                                this, [this, localPath]() { setLocalDirectory(localPath); });
                    }
                    QAction *uploadAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Send, this->palette().color(QPalette::WindowText)),
                        tr("Upload to the current remote directory"));
                    connect(uploadAction, &QAction::triggered, this, [this, localPath]() {
                        uploadLocalPaths(selectedTarget(), {localPath}, m_pathEdit->text().trimmed());
                    });
                    menu.addSeparator();
                }
                QAction *refreshAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Refresh, this->palette().color(QPalette::WindowText)),
                    tr("Refresh local directory"));
                connect(refreshAction, &QAction::triggered, this, [this]() {
                    setLocalDirectory(m_localPathEdit->text().trimmed());
                });
                menu.exec(m_localTree->viewport()->mapToGlobal(position));
            });
    connect(m_targetCombo, &QComboBox::currentIndexChanged, this, [this](const int) {
        if (m_targetCombo->currentIndex() >= 0) {
            if (m_followInnermostCheck->isChecked()
                && m_targetCombo->currentIndex() != m_targets.size() - 1) {
                // 选择外层代表明确的人工意图；自动关闭跟随后，目录刷新、凭据更新等无关的
                // targetsChanged 不会再把用户抢回最内层。
                m_followInnermostCheck->setChecked(false);
            }
            m_fileTree->clear();
            restorePathForSelectedTarget();
            refreshDirectory();
        }
    });
    connect(m_followInnermostCheck, &QCheckBox::toggled, this, [this](const bool enabled) {
        if (!enabled || m_targets.isEmpty()) {
            return;
        }
        const int innermostIndex = static_cast<int>(m_targets.size() - 1);
        if (m_targetCombo->currentIndex() != innermostIndex) {
            m_targetCombo->setCurrentIndex(innermostIndex);
        }
    });
    connect(m_followTerminalCwdCheck, &QCheckBox::toggled, this, [this](const bool enabled) {
        if (!enabled) {
            return;
        }

        const ConnectionTarget target = selectedTarget();
        if (!target.isValid()) {
            // 终端尚未连接或当前是本地 Shell 时没有可跟随的远端目录；保持用户勾选状态，
            // 待后续收到有效服务器的 cwd 上报后，既有实时跟随路径会自然完成同步。
            return;
        }

        const QString stableKey = target.stableKey();
        // 复用统一入口可继续执行服务器匹配、空路径过滤与“相同路径不重复刷新”等保护，
        // 避免勾选即时同步和后续实时上报形成两套逐渐不一致的判断逻辑。
        followTerminalWorkingDirectory(
            stableKey, m_terminalWorkingDirectoryByTarget.value(stableKey));
    });
    connect(m_fileTree,
            &QTreeWidget::itemDoubleClicked,
            this,
            &RemoteFileManagerWidget::activateItem);
    connect(m_fileTree,
            &QWidget::customContextMenuRequested,
            this,
            [this](const QPoint &position) {
                QTreeWidgetItem *item = m_fileTree->itemAt(position);
                if (item != nullptr && !item->isSelected()) {
                    // 右键未选中行时切换到该行；右键已有多选中的任意一行则保留批量选择，
                    // 让“下载”继续按用户明确选择的全部条目入队。
                    m_fileTree->clearSelection();
                    m_fileTree->setCurrentItem(item);
                    item->setSelected(true);
                }
                QMenu menu(this);
                UiIcons::modernizePopup(&menu);
                QAction *uploadFileAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Send, this->palette().color(QPalette::WindowText)),
                    tr("Upload files to the current directory"));
                QAction *uploadDirectoryAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Upload, this->palette().color(QPalette::WindowText)),
                    tr("Upload a directory to the current directory"));
                connect(uploadFileAction,
                        &QAction::triggered,
                        this,
                        &RemoteFileManagerWidget::uploadSelectedFile);
                connect(uploadDirectoryAction,
                        &QAction::triggered,
                        this,
                        &RemoteFileManagerWidget::uploadSelectedDirectory);
                if (item != nullptr) {
                    menu.addSeparator();
                    QAction *downloadAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Download, this->palette().color(QPalette::WindowText)),
                        tr("Download"));
                    QAction *previewAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Eye, this->palette().color(QPalette::WindowText)),
                        tr("Preview"));
                    QAction *chmodAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Info, this->palette().color(QPalette::WindowText)),
                        tr("Change permissions"));
                    QAction *renameAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Edit, this->palette().color(QPalette::WindowText)),
                        tr("Rename"));
                    QAction *deleteAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Trash, this->palette().color(QPalette::WindowText)),
                        tr("Delete"));
                    connect(downloadAction, &QAction::triggered,
                            this, &RemoteFileManagerWidget::downloadSelectedFile);
                    connect(previewAction, &QAction::triggered,
                            this, &RemoteFileManagerWidget::previewSelectedFile);
                    connect(chmodAction, &QAction::triggered,
                            this, &RemoteFileManagerWidget::chmodSelectedPath);
                    connect(renameAction, &QAction::triggered,
                            this, &RemoteFileManagerWidget::renameSelectedPath);
                    connect(deleteAction, &QAction::triggered,
                            this, &RemoteFileManagerWidget::deleteSelectedPath);
                }
                menu.addSeparator();
                QAction *refreshAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Refresh, this->palette().color(QPalette::WindowText)),
                    tr("Refresh remote directory"));
                connect(refreshAction, &QAction::triggered,
                        this, &RemoteFileManagerWidget::refreshDirectory);
                menu.exec(m_fileTree->viewport()->mapToGlobal(position));
            });
    connect(m_cancelButton,
            &QToolButton::clicked,
            this,
            &RemoteFileManagerWidget::cancelActiveTransfer);
    connect(m_pauseButton,
            &QToolButton::clicked,
            this,
            &RemoteFileManagerWidget::toggleActiveTransferPause);
    connect(downloadButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::downloadSelectedFile);
    connect(previewButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::previewSelectedFile);
    connect(chmodButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::chmodSelectedPath);
    connect(renameButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::renameSelectedPath);
    connect(deleteButton, &QToolButton::clicked, this, &RemoteFileManagerWidget::deleteSelectedPath);
    connect(m_transferQueueList,
            &QWidget::customContextMenuRequested,
            this,
            [this](const QPoint &position) {
                QListWidgetItem *item = m_transferQueueList->itemAt(position);
                if (item != nullptr) {
                    m_transferQueueList->setCurrentItem(item);
                }
                if (item == nullptr && !m_activeQueuedTransfer.has_value()) {
                    return;
                }
                QMenu menu(this);
                UiIcons::modernizePopup(&menu);
                if (m_activeQueuedTransfer.has_value()) {
                    QAction *pauseAction = menu.addAction(
                        UiIcons::symbol(this, UiIcons::Symbol::Pause, this->palette().color(QPalette::WindowText)),
                        m_activeTransferPaused ? tr("Continue the current transfer") : tr("Pause the current transfer"));
                    connect(pauseAction,
                            &QAction::triggered,
                            this,
                            &RemoteFileManagerWidget::toggleActiveTransferPause);
                }
                QAction *cancelAction = menu.addAction(
                    UiIcons::symbol(this, UiIcons::Symbol::Cancel, this->palette().color(QPalette::WindowText)),
                    tr("Deselect task"));
                connect(cancelAction,
                        &QAction::triggered,
                        this,
                        &RemoteFileManagerWidget::cancelActiveTransfer);
                menu.exec(m_transferQueueList->viewport()->mapToGlobal(position));
            });

    auto *remoteDropTree = static_cast<RemoteDropTree *>(m_fileTree);
    remoteDropTree->setLocalDropHandler([this](const QStringList &localPaths) {
        // 一律上传到当前远程目录，不按鼠标落点猜测目标子目录——见 RemoteDropTree::dropEvent。
        uploadLocalPaths(selectedTarget(), localPaths, m_pathEdit->text().trimmed());
    });
    auto *localDropTree = static_cast<LocalDropTree *>(m_localTree);
    localDropTree->setRemoteDropHandler(
        [this](const QString &remotePath, const bool directory) {
            // 一律下载到当前本地目录，与远程栏的落点语义对称——见 LocalDropTree::dropEvent。
            RemoteFileInfo entry;
            entry.path = remotePath;
            entry.name = remotePath.section(QLatin1Char('/'), -1);
            entry.isDirectory = directory;
            downloadRemoteEntryToDirectory(entry, m_localPathEdit->text().trimmed());
        });

    connect(m_service,
            &SftpService::directoryListed,
            this,
            [this](const quint64 requestId,
                   const ConnectionTarget &target,
                   const QString &path,
                   const QList<RemoteFileInfo> &entries) {
                // 用户可能已切换服务器或连续输入了新路径，过期响应不得覆盖最新页面。
                if (requestId != m_latestListRequest || target.stableKey() != selectedTarget().stableKey()) {
                    return;
                }
                m_fileTree->clear();
                int originalIndex = 0;
                for (const RemoteFileInfo &entry : entries) {
                    auto *item = new QTreeWidgetItem(m_fileTree);
                    item->setText(0, entry.name);
                    item->setIcon(
                        0,
                        remoteFileEntryIcon(m_fileTree, entry.isDirectory));
                    item->setText(1, entry.isDirectory ? QStringLiteral("—") : formatSize(entry.size));
                    item->setText(2, entry.modifiedAt.isValid()
                                          ? entry.modifiedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                          : QString());
                    item->setText(3, entry.permissionText());
                    item->setText(4, entry.owner);
                    item->setData(0, Qt::UserRole, entry.path);
                    item->setData(0, Qt::UserRole + 1, entry.isDirectory);
                    item->setData(0, Qt::UserRole + 2, QVariant::fromValue(entry.size));
                    item->setData(0, Qt::UserRole + 3, QVariant::fromValue(entry.permissions));
                    item->setData(0, Qt::UserRole + 4, entry.isSymbolicLink);
                    // 大小排序必须比较真实字节值，否则 "9 B" 会大于 "10 MiB"；权限同样
                    // 比较原始位掩码；修改时间排序按 QDateTime 原值比较，而不是格式化后的
                    // 字符串——字符串按字典序排会把跨年份、跨月份的日期排错。
                    item->setData(0, kOriginalIndexRole, originalIndex++);
                    item->setData(0, kModifiedAtRole, entry.modifiedAt);
                }
                applyRemoteSort();
                m_pathEdit->setText(path);
                // 记住这台服务器的当前位置：下次切回来时应当回到这里，而不是沿用别处的路径。
                m_pathByTarget.insert(target.stableKey(), path);
                m_directoryDisconnectRetries.remove(target.stableKey());
                setBusy(false, tr("%1 items").arg(entries.size()));
            });
    connect(m_service,
            &SftpService::operationSucceeded,
            this,
            &RemoteFileManagerWidget::handleOperationSuccess);
    connect(m_service,
            &SftpService::operationFailed,
            this,
            &RemoteFileManagerWidget::handleOperationFailure);
    connect(m_service,
            &SftpService::hostKeyVerificationRequired,
            this,
            [this](const ConnectionTarget &target, const QString &fingerprint) {
                if (m_hostKeyDialogOpen) {
                    return;
                }
                m_hostKeyDialogOpen = true;
                const QMessageBox::StandardButton answer = QMessageBox::question(
                    this,
                    tr("Confirm the server host key"),
                    tr("Connecting to %1 for the first time.\n\nSHA-256 fingerprint:\n%2\n\nVerify the fingerprint through a trusted channel. Trust it and write it to known_hosts?")
                        .arg(target.connectionSpecifier(true), fingerprint),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                m_hostKeyDialogOpen = false;
                if (answer == QMessageBox::Yes) {
                    m_service->trustHostKey(target);
                    refreshDirectory();
                }
            });
    connect(m_service,
            &SftpService::hostKeyChangeVerificationRequired,
            this,
            [this](const ConnectionTarget &target, const QString &fingerprint) {
                if (m_hostKeyDialogOpen) {
                    return;
                }
                m_hostKeyDialogOpen = true;
                // SFTP 使用独立 SSH 会话，仍必须与终端执行同等级别的变更核验，不能因为用户
                // 已登录终端就假定后台连接拿到的一定是同一台服务器。
                const QMessageBox::StandardButton answer = QMessageBox::warning(
                    this,
                    tr("The server host key has changed"),
                    tr("The host key returned by %1 does not match the local record.\n\nNew SHA-256 fingerprint:\n%2\n\nThis may be a server reinstall or key rotation, or a man-in-the-middle attack. Verify the new fingerprint through a trusted channel first.\n\nConfirm the key has changed and replace the old record in known_hosts?")
                        .arg(target.connectionSpecifier(true), fingerprint),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                m_hostKeyDialogOpen = false;
                if (answer == QMessageBox::Yes) {
                    m_service->trustHostKey(target);
                    refreshDirectory();
                }
            });
    connect(m_service,
            &SftpService::transferProgress,
            this,
            [this](const quint64 requestId, const quint64 completed, const quint64 total) {
                if (requestId != m_activeTransferRequest) {
                    return;
                }
                m_progressBar->setVisible(true);
                const int percent = SftpRecursive::progressPercent(completed, total);
                if (percent < 0) {
                    m_progressBar->setRange(0, 0);
                } else {
                    m_progressBar->setRange(0, 100);
                    m_progressBar->setValue(percent);
                }
                // 百分比无法回答“还要多久”“是不是卡住了”。字节、速度与剩余时间放在
                // 进度条正下方的独立摘要标签，避免把长文本硬塞进只有 6px 高的轨道。
                updateTransferProgressText(completed, total);
            });
    connect(m_service,
            &SftpService::recursiveTransferProgress,
            this,
            [this](const quint64 requestId,
                   const quint64 filesCompleted,
                   const quint64 filesTotal,
                   const QString &currentItem) {
                if (requestId != m_activeTransferRequest) {
                    return;
                }
                // 字节进度条无法区分"一个大文件"和"几千个小文件"，条目计数补上这层信息。
                const QString message = tr("Transferring %1/%2: %3")
                                            .arg(filesCompleted + 1)
                                            .arg(filesTotal)
                                            .arg(currentItem);
                m_statusLabel->setText(message);
                emit statusMessage(message);
            });
    connect(m_service,
            &SftpService::recursiveTransferFinished,
            this,
            &RemoteFileManagerWidget::handleRecursiveTransferFinished);
    connect(m_service,
            &SftpService::pathStatReady,
            this,
            [this](const quint64 requestId,
                   const ConnectionTarget &,
                   const QString &,
                   const bool exists,
                   const bool isDirectory) {
                const auto pending = m_pendingDirectoryChecks.find(requestId);
                if (pending == m_pendingDirectoryChecks.end()) {
                    return;
                }
                const PendingDirectoryUpload upload = pending.value();
                m_pendingDirectoryChecks.erase(pending);

                if (!exists) {
                    // 远端还没有同名目录，没有冲突可言，直接按普通写入入队。
                    enqueueDirectoryUpload(upload, SftpTransfer::DirectoryConflictPolicy::Merge);
                    return;
                }

                QMessageBox box(this);
                box.setIcon(QMessageBox::Question);
                box.setWindowTitle(tr("An item with the same name already exists on the remote"));
                if (!isDirectory) {
                    // 同名的普通文件挡住了目录位置，"合并"无从谈起，只能替换或放弃。
                    box.setText(tr("A file with the same name already exists on the remote:\n%1").arg(upload.remoteRoot));
                    box.setInformativeText(
                        tr("To upload a directory, this file must be deleted first. Replace it?"));
                    QPushButton *replaceButton =
                        box.addButton(tr("Replace"), QMessageBox::DestructiveRole);
                    box.addButton(tr("Cancel"), QMessageBox::RejectRole);
                    box.setDefaultButton(replaceButton);
                    box.exec();
                    if (box.clickedButton() == replaceButton) {
                        enqueueDirectoryUpload(upload,
                                               SftpTransfer::DirectoryConflictPolicy::Replace);
                    }
                    return;
                }

                box.setText(tr("A directory with the same name already exists on the remote:\n%1").arg(upload.remoteRoot));
                // 两种语义的差别事后无法补救，必须在这里说清楚，而不是让用户凭按钮名去猜。
                box.setInformativeText(
                    tr("Merge update: compares content file by file and re-uploads only the files that actually changed; extra files on the remote are kept.\n\nReplace: deletes the entire remote directory and uploads everything again so the result exactly matches local; extra files on the remote are permanently deleted."));
                QPushButton *mergeButton =
                    box.addButton(tr("Merge update"), QMessageBox::AcceptRole);
                QPushButton *replaceButton =
                    box.addButton(tr("Replace"), QMessageBox::DestructiveRole);
                box.addButton(tr("Cancel"), QMessageBox::RejectRole);
                box.setDefaultButton(
                    m_lastDirectoryConflictChoice == SftpTransfer::DirectoryConflictPolicy::Replace
                        ? replaceButton
                        : mergeButton);
                box.exec();

                if (box.clickedButton() == mergeButton) {
                    m_lastDirectoryConflictChoice = SftpTransfer::DirectoryConflictPolicy::Merge;
                    enqueueDirectoryUpload(upload, SftpTransfer::DirectoryConflictPolicy::Merge);
                } else if (box.clickedButton() == replaceButton) {
                    m_lastDirectoryConflictChoice = SftpTransfer::DirectoryConflictPolicy::Replace;
                    enqueueDirectoryUpload(upload, SftpTransfer::DirectoryConflictPolicy::Replace);
                }
                // 取消：既不入队也不改变默认项，用户的犹豫不该被记成一次选择。
            });
    connect(m_service,
            &SftpService::filePreviewReady,
            this,
            [this](const quint64,
                   const ConnectionTarget &target,
                   const QString &remotePath,
                   const QByteArray &data,
                   const QString &mimeType,
                   const bool truncated) {
                if (target.stableKey() != selectedTarget().stableKey()) {
                    // 用户已经切换服务器时不弹出旧请求内容，避免把 A 服务器文件误认为 B。
                    return;
                }
                setBusy(false, tr("Loaded the safe preview of %1").arg(remotePath));

                auto *dialog = new QDialog(this);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->setWindowTitle(tr("Safe preview · %1").arg(remotePath));
                dialog->resize(760, 560);
                auto *layout = new QVBoxLayout(dialog);
                if (truncated) {
                    auto *notice = new QLabel(
                        tr("Showing only the first %1; the file content is truncated.")
                            .arg(formatSize(static_cast<quint64>(data.size()))),
                        dialog);
                    notice->setWordWrap(true);
                    layout->addWidget(notice);
                }

                static const QStringList safeRasterTypes{
                    QStringLiteral("image/png"), QStringLiteral("image/jpeg"),
                    QStringLiteral("image/gif"), QStringLiteral("image/webp"),
                    QStringLiteral("image/bmp")};
                QImage image;
                if (safeRasterTypes.contains(mimeType)) {
                    image = QImage::fromData(data);
                }
                if (!image.isNull()) {
                    auto *imageLabel = new QLabel(dialog);
                    imageLabel->setAlignment(Qt::AlignCenter);
                    imageLabel->setPixmap(QPixmap::fromImage(image).scaled(
                        QSize(720, 500), Qt::KeepAspectRatio, Qt::SmoothTransformation));
                    layout->addWidget(imageLabel, 1);
                } else if (mimeType.startsWith(QStringLiteral("text/"))
                           || looksLikeTextPreview(data)) {
                    auto *textView = new QPlainTextEdit(dialog);
                    textView->setReadOnly(true);
                    textView->setLineWrapMode(QPlainTextEdit::NoWrap);
                    // QPlainTextEdit 只按纯文本展示，不解析 HTML、终端转义序列或脚本。
                    textView->setPlainText(QString::fromUtf8(data));
                    layout->addWidget(textView, 1);
                } else {
                    auto *unsupported = new QLabel(
                        tr("This file is not a supported text file or safe raster image; binary content is not shown.\nMIME: %1")
                            .arg(mimeType),
                        dialog);
                    unsupported->setWordWrap(true);
                    layout->addWidget(unsupported);
                }
                dialog->show();
            });

    // 常驻文件管理面板：语言切换时重设列头、工具栏按钮提示与固定文案。传输状态文案
    // （m_statusLabel / m_pauseButton）是动态内容，随下一次传输事件转为新语言。
    i18n::onRetranslate(this,
                        [this, uploadButton, downloadButton, previewButton, chmodButton,
                         renameButton, deleteButton, uploadFilesAction, uploadDirectoryAction] {
        const QStringList headers{tr("Name"), tr("Size"), tr("Modified"),
                                  tr("Permissions"), tr("Owner")};
        m_localTree->setHeaderLabels(headers);
        m_fileTree->setHeaderLabels(headers);
        // 上传菜单里的这两个 QAction 只在构造期设过一次文案，且会被命令面板通过
        // findChildren<QAction*> 收录；不在此处重设，切换语言后面板里会残留上一次语言。
        uploadFilesAction->setText(tr("Upload Files…"));
        uploadDirectoryAction->setText(tr("Upload Directory…"));
        uploadButton->setToolTip(tr("Upload"));
        downloadButton->setToolTip(tr("Download"));
        previewButton->setToolTip(tr("Preview"));
        chmodButton->setToolTip(tr("Permissions"));
        renameButton->setToolTip(tr("Rename"));
        deleteButton->setToolTip(tr("Delete"));
        m_resumeTransfersCheck->setText(tr("Resumable transfer"));
        m_closeButton->setToolTip(
            tr("Close the remote file manager and give all the width back to the terminal"));
        m_fullScreenButton->setToolTip(
            m_localPanel != nullptr && m_localPanel->isVisible()
                ? tr("Exit full screen and collapse local files")
                : tr("Show the file manager full screen and expand local files for side-by-side copying"));
    });

    rebuildTransferQueueView();
    updateFullScreenIcon();
    updateResponsiveActionButtons();
    updateRemoteNameColumnWidth();
    // 本地树不再由 QFileSystemModel 惰性懒加载，必须显式列一次主目录，否则本地面板在
    // 首次展开全屏之前会一直是空的。
    setLocalDirectory(m_localPathEdit->text());
}

void RemoteFileManagerWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateResponsiveActionButtons();
    updateRemoteNameColumnWidth();
}

void RemoteFileManagerWidget::updateRemoteNameColumnWidth()
{
    QHeaderView *header = m_fileTree->header();
    if (header->count() < 1) {
        return;
    }
    int othersWidth = 0;
    for (int column = 1; column < header->count(); ++column) {
        othersWidth += header->sectionSize(column);
    }
    // 视口宽度足够时名称列吃掉剩余空间；不够时钳在下限，交给横向滚动条呈现被挤出的部分，
    // 而不是继续压缩名称列本身。
    const int available = m_fileTree->viewport()->width() - othersWidth;
    header->resizeSection(0, std::max(kRemoteNameColumnMinWidth, available));
}

void RemoteFileManagerWidget::updateResponsiveActionButtons()
{
    // 760px 能容纳六个文字动作、续传复选框和右侧控制按钮；低于该阈值时保留图标与完整
    // Tooltip。使用控件实际宽度而不是全屏布尔值，可正确处理用户拖动外层分隔器的场景。
    const bool showText = width() >= 760;
    const QList<QToolButton *> actionButtons = findChildren<QToolButton *>();
    for (QToolButton *button : actionButtons) {
        if (!button->property("sftpResponsiveAction").toBool()) {
            continue;
        }
        button->setToolButtonStyle(showText ? Qt::ToolButtonTextBesideIcon
                                            : Qt::ToolButtonIconOnly);
        // 窄态统一 34px 点击区；宽态让文字和菜单箭头按 sizeHint 自然展开。最大宽度必须
        // 恢复，否则用户退出全屏后再进入时会一直被前一次窄态尺寸锁住。
        if (showText) {
            button->setMinimumWidth(0);
            button->setMaximumWidth(QWIDGETSIZE_MAX);
        } else {
            button->setFixedWidth(34);
        }
    }
}

void RemoteFileManagerWidget::exitFullScreen()
{
    // setChecked 会触发 toggled，进而收起本地面板并发出 fullScreenChanged(false)，
    // 让工作区把终端恢复出来；因此这里不需要再重复做同样的事。
    m_fullScreenButton->setChecked(false);
}

void RemoteFileManagerWidget::updateFullScreenIcon()
{
    m_fullScreenButton->setIcon(
        m_fullScreenButton->isChecked()
            ? UiIcons::symbol(this, UiIcons::Symbol::FullscreenExit, this->palette().color(QPalette::WindowText))
            : UiIcons::symbol(this, UiIcons::Symbol::FullscreenEnter, this->palette().color(QPalette::WindowText)));
}

void RemoteFileManagerWidget::cycleRemoteSort(const int column)
{
    // 三态推进属于纯状态计算，放在 SftpUiState 中由单元测试覆盖；这里只负责把结果
    // 反映到表头指示器和实际排序上。
    const SftpUiState::SortState next =
        SftpUiState::nextSortState({m_sortColumn, m_sortOrder}, column);
    m_sortColumn = next.column;
    m_sortOrder = next.order;

    if (m_sortColumn < 0) {
        m_fileTree->header()->setSortIndicatorShown(false);
    } else {
        m_fileTree->header()->setSortIndicatorShown(true);
        m_fileTree->header()->setSortIndicator(m_sortColumn, m_sortOrder);
    }
    applyRemoteSort();
}

void RemoteFileManagerWidget::applyRemoteSort()
{
    sortFileTreeItems(m_fileTree, m_sortColumn, m_sortOrder);
}

void RemoteFileManagerWidget::cycleLocalSort(const int column)
{
    const SftpUiState::SortState next =
        SftpUiState::nextSortState({m_localSortColumn, m_localSortOrder}, column);
    m_localSortColumn = next.column;
    m_localSortOrder = next.order;

    if (m_localSortColumn < 0) {
        m_localTree->header()->setSortIndicatorShown(false);
    } else {
        m_localTree->header()->setSortIndicatorShown(true);
        m_localTree->header()->setSortIndicator(m_localSortColumn, m_localSortOrder);
    }
    applyLocalSort();
}

void RemoteFileManagerWidget::applyLocalSort()
{
    sortFileTreeItems(m_localTree, m_localSortColumn, m_localSortOrder);
}

void RemoteFileManagerWidget::resetTransferSpeed()
{
    m_transferClock.start();
    m_lastSpeedSampleMs = 0;
    m_lastSpeedSampleBytes = 0;
    m_smoothedBytesPerSecond = 0.0;
}

void RemoteFileManagerWidget::updateTransferProgressText(const quint64 completed,
                                                          const quint64 total)
{
    if (!m_transferClock.isValid()) {
        m_transferClock.start();
    }
    const qint64 nowMs = m_transferClock.elapsed();
    const qint64 deltaMs = nowMs - m_lastSpeedSampleMs;
    // 至少间隔 300ms 再采样：SFTP 每个数据块都会回调，间隔过短会让瞬时速度剧烈跳动，
    // 数字快到看不清。
    if (deltaMs >= 300 && completed >= m_lastSpeedSampleBytes) {
        const double instant =
            double(completed - m_lastSpeedSampleBytes) * 1000.0 / double(deltaMs);
        // 指数滑动平均进一步抑制网络抖动；首个样本直接采用，避免从 0 缓慢爬升。
        m_smoothedBytesPerSecond = m_smoothedBytesPerSecond > 0.0
            ? m_smoothedBytesPerSecond * 0.7 + instant * 0.3
            : instant;
        m_lastSpeedSampleMs = nowMs;
        m_lastSpeedSampleBytes = completed;
    }

    QString text = tr("Transferred %1").arg(formatSize(completed));
    if (m_smoothedBytesPerSecond > 0.0) {
        text += tr(" · %1/s").arg(formatSize(quint64(m_smoothedBytesPerSecond)));
        if (total > completed) {
            const double remainingSeconds =
                double(total - completed) / m_smoothedBytesPerSecond;
            // 剩余时间只在能算得出时给出，避免总量未知时显示一个凭空的数字。
            text += tr(" · %1 left").arg(formatDuration(remainingSeconds));
        }
    }
    m_transferDetailLabel->setText(text);
}

RemoteFileManagerWidget::~RemoteFileManagerWidget()
{
    // QString 可能因隐式共享存在多个缓冲区，无法提供操作系统级“绝不留痕”保证；仍主动覆盖
    // 本控件直接持有的会话副本，缩短密码在正常退出路径中的内存驻留时间。
    for (QString &secret : m_sessionSecrets) {
        secret.fill(QChar::Null);
    }
    m_sessionSecrets.clear();
    for (ConnectionTarget &target : m_targets) {
        target.password.fill(QChar::Null);
        target.password.clear();
    }
}

void RemoteFileManagerWidget::setAvailableTargets(const QList<ConnectionTarget> &targets,
                                                  const bool followInnermost)
{
    const QString previousKey = selectedTarget().stableKey();
    const int previousIndex = m_targetCombo->currentIndex();
    m_targets = targets;
    for (ConnectionTarget &target : m_targets) {
        if (target.password.isEmpty()) {
            const auto cachedSecret = m_sessionSecrets.constFind(target.stableKey());
            if (cachedSecret != m_sessionSecrets.cend()) {
                // TerminalPane 只维护连接拓扑，不保存用户后来为 SFTP 输入的密码；在界面列表
                // 每次重建时恢复内存缓存，保证切目录、切层级和切标签页都不会重复询问。
                target.password = cachedSecret.value();
            }
        } else {
            // 初始连接对话框提供的会话密码也进入同一缓存，但仍然不会被持久化。
            m_sessionSecrets.insert(target.stableKey(), target.password);
        }
    }

    m_targetCombo->blockSignals(true);
    m_targetCombo->clear();
    for (qsizetype index = 0; index < m_targets.size(); ++index) {
        const ConnectionTarget &target = m_targets.at(index);
        // 使用缩进和层级编号表达 A → B → C，不依赖服务器别名是否重复。
        const QString label = target.displayName.isEmpty() ? target.connectionSpecifier()
                                                           : target.displayName;
        // 推测层必须一眼可辨：它可能与远端真实状态不一致，而在错误的服务器上删除文件
        // 是不可撤销的。
        m_targetCombo->addItem(
            target.confidence == ConnectionTarget::Confidence::Inferred
                ? tr("Layer %1 · ⚠ inferred · %2").arg(index + 1).arg(label)
                : tr("Layer %1 · %2").arg(index + 1).arg(label));
    }

    QStringList stableKeys;
    stableKeys.reserve(m_targets.size());
    for (const ConnectionTarget &target : std::as_const(m_targets)) {
        stableKeys.append(target.stableKey());
    }
    // followInnermost 是调用方对本次连接链变化的建议，复选框才是用户在当前标签页内持久的
    // 偏好。两者同时为 true 才自动切到内层；人工关闭后，普通刷新不得覆盖其选择。
    const int selectedIndex = SftpUiState::selectTargetIndex(
        stableKeys,
        previousKey,
        previousIndex,
        followInnermost && m_followInnermostCheck->isChecked());
    m_targetCombo->setCurrentIndex(selectedIndex);
    m_targetCombo->blockSignals(false);

    if (selectedIndex >= 0) {
        if (m_targets.at(selectedIndex).stableKey() != previousKey) {
            // 切换层级后立即清空旧服务器列表，避免隧道尚未建立时把第一层缓存误认为第二层内容。
            m_fileTree->clear();
            restorePathForSelectedTarget();
        }
        refreshDirectory();
    } else {
        m_fileTree->clear();
        m_statusLabel->setText(tr("The current terminal is not connected to a server yet"));
    }
}

void RemoteFileManagerWidget::setLocalDirectory(const QString &directory)
{
    const QFileInfo directoryInfo(QDir::cleanPath(directory));
    if (!directoryInfo.exists() || !directoryInfo.isDir()) {
        QMessageBox::warning(this,
                             tr("Invalid local path"),
                             tr("The local directory does not exist or is not accessible:\n%1").arg(directory));
        return;
    }
    const QDir directoryToList(directoryInfo.absoluteFilePath());
    if (!directoryToList.isReadable()) {
        QMessageBox::warning(this,
                             tr("Could not browse the local directory"),
                             tr("No permission to read this directory:\n%1").arg(directoryToList.absolutePath()));
        return;
    }

    const QString absolutePath = directoryToList.absolutePath();
    m_localPathEdit->setText(absolutePath);

    m_localTree->clear();
    static QFileIconProvider iconProvider;
    // 与远程栏的排序稳定性要求一致：入树顺序即"还原默认"时的顺序，因此这里先固定按
    // 目录优先、名称升序枚举一遍，而不是依赖文件系统的原始返回顺序。
    const QFileInfoList entries = directoryToList.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    int originalIndex = 0;
    for (const QFileInfo &info : entries) {
        auto *item = new QTreeWidgetItem(m_localTree);
        item->setText(0, info.fileName());
        item->setIcon(0, iconProvider.icon(info));
        item->setText(1, info.isDir() ? QStringLiteral("—") : formatSize(static_cast<quint64>(info.size())));
        item->setText(2, info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        const quint32 permissionBits = localPermissionBits(info);
        RemoteFileInfo permissionText;
        permissionText.permissions = permissionBits;
        permissionText.isDirectory = info.isDir();
        permissionText.isSymbolicLink = info.isSymbolicLink();
        item->setText(3, permissionText.permissionText());
        item->setText(4, info.owner());
        item->setData(0, Qt::UserRole, info.absoluteFilePath());
        item->setData(0, Qt::UserRole + 1, info.isDir());
        item->setData(0, Qt::UserRole + 2, QVariant::fromValue<quint64>(static_cast<quint64>(info.size())));
        item->setData(0, Qt::UserRole + 3, QVariant::fromValue(permissionBits));
        item->setData(0, kModifiedAtRole, info.lastModified());
        item->setData(0, kOriginalIndexRole, originalIndex++);
    }
    applyLocalSort();
}

SftpTransfer::ExistingFilePolicy RemoteFileManagerWidget::selectedTransferPolicy() const
{
    return m_resumeTransfersCheck->isChecked()
        ? SftpTransfer::ExistingFilePolicy::ResumeWhenPossible
        : SftpTransfer::ExistingFilePolicy::Overwrite;
}

void RemoteFileManagerWidget::enqueueTransfer(QueuedTransfer transfer)
{
    transfer.queueId = m_nextTransferQueueId++;
    m_pendingTransfers.append(std::move(transfer));
    rebuildTransferQueueView();
    startNextQueuedTransfer();
}

void RemoteFileManagerWidget::startNextQueuedTransfer()
{
    if (m_activeQueuedTransfer.has_value() || m_pendingTransfers.isEmpty()) {
        return;
    }

    m_activeQueuedTransfer = m_pendingTransfers.takeFirst();
    const QueuedTransfer &transfer = *m_activeQueuedTransfer;
    m_activeTransferPaused = false;
    m_pauseButton->setText(tr("Pause"));
    m_pauseButton->setToolTip(tr("Pause the current transfer"));
    m_pauseButton->setAccessibleName(tr("Pause the current transfer"));
    m_pauseButton->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Pause, this->palette().color(QPalette::WindowText)));
    setBusy(true, tr("Running: %1").arg(transfer.description));
    // 每个传输独立计速：沿用上一次的样本会让新任务开头显示出前一个任务的速度。
    resetTransferSpeed();

    switch (transfer.kind) {
    case QueuedTransferKind::UploadFile:
        m_activeTransferRequest = m_service->uploadFile(transfer.target,
                                                        transfer.sourcePath,
                                                        transfer.destinationPath,
                                                        transfer.policy);
        break;
    case QueuedTransferKind::UploadDirectory:
        m_activeTransferRequest = m_service->uploadDirectory(transfer.target,
                                                             transfer.sourcePath,
                                                             transfer.destinationPath,
                                                             transfer.policy,
                                                             transfer.conflictPolicy);
        break;
    case QueuedTransferKind::DownloadFile:
        m_activeTransferRequest = m_service->downloadFile(transfer.target,
                                                          transfer.sourcePath,
                                                          transfer.destinationPath,
                                                          transfer.policy);
        break;
    case QueuedTransferKind::DownloadDirectory:
        m_activeTransferRequest = m_service->downloadDirectory(transfer.target,
                                                               transfer.sourcePath,
                                                               transfer.destinationPath,
                                                               transfer.policy);
        break;
    }
    rebuildTransferQueueView();
}

void RemoteFileManagerWidget::finishQueuedTransfer(const quint64 requestId)
{
    if (requestId == 0 || requestId != m_activeTransferRequest) {
        return;
    }
    m_activeTransferRequest = 0;
    m_activeQueuedTransfer.reset();
    m_activeTransferPaused = false;
    m_pauseButton->setText(tr("Pause"));
    m_pauseButton->setToolTip(tr("Pause the current transfer"));
    m_pauseButton->setAccessibleName(tr("Pause the current transfer"));
    m_pauseButton->setIcon(
        UiIcons::symbol(this, UiIcons::Symbol::Pause, this->palette().color(QPalette::WindowText)));
    rebuildTransferQueueView();

    // 信号处理函数可能仍在展示失败或汇总对话框；把下一项放到事件循环末尾启动，避免前一项
    // 的状态更新和后一项首个进度信号交错覆盖。
    QTimer::singleShot(0, this, &RemoteFileManagerWidget::startNextQueuedTransfer);
}

void RemoteFileManagerWidget::rebuildTransferQueueView()
{
    const quint64 selectedQueueId = m_transferQueueList->currentItem() == nullptr
        ? 0
        : m_transferQueueList->currentItem()->data(Qt::UserRole).toULongLong();
    m_transferQueueList->clear();

    const auto appendItem = [this, selectedQueueId](const QueuedTransfer &transfer,
                                                     const QString &state,
                                                     const QString &stateRole) {
        auto *item = new QListWidgetItem(m_transferQueueList);
        item->setData(Qt::UserRole, QVariant::fromValue(transfer.queueId));

        // 状态从文件名左侧前缀移到右侧徽章：文件名保持连续，三种状态又能通过固定位置和
        // 颜色迅速扫读。容器对鼠标透明，让 QListWidget 继续负责选择和右键取消逻辑。
        auto *row = new QWidget(m_transferQueueList);
        row->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 2, 0);
        rowLayout->setSpacing(8);
        auto *nameLabel = new QLabel(transfer.description, row);
        nameLabel->setObjectName(QStringLiteral("SftpQueueItemName"));
        nameLabel->setTextFormat(Qt::PlainText);
        nameLabel->setToolTip(transfer.description);
        // 队列描述常含完整路径。让名称区域可被压缩，才能保证右侧状态徽章始终留在可视区；
        // Tooltip 保留未经截断的完整信息，避免紧凑布局损失可诊断性。
        nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *stateLabel = new QLabel(state, row);
        stateLabel->setObjectName(QStringLiteral("SftpQueueStateBadge"));
        stateLabel->setProperty("queueState", stateRole);
        stateLabel->setAlignment(Qt::AlignCenter);
        rowLayout->addWidget(nameLabel, 1);
        rowLayout->addWidget(stateLabel, 0);
        item->setSizeHint(QSize(0, 30));
        m_transferQueueList->setItemWidget(item, row);
        if (transfer.queueId == selectedQueueId) {
            m_transferQueueList->setCurrentItem(item);
        }
    };
    if (m_activeQueuedTransfer.has_value()) {
        appendItem(*m_activeQueuedTransfer,
                   m_activeTransferPaused ? tr("Paused") : tr("In progress"),
                   m_activeTransferPaused ? QStringLiteral("paused")
                                          : QStringLiteral("active"));
    }
    for (const QueuedTransfer &transfer : std::as_const(m_pendingTransfers)) {
        appendItem(transfer,
                   tr("Waiting"),
                   QStringLiteral("queued"));
    }

    const bool hasActive = m_activeQueuedTransfer.has_value();
    const bool hasAnyTransfer = hasActive || !m_pendingTransfers.isEmpty();
    m_pauseButton->setEnabled(hasActive);
    m_cancelButton->setEnabled(hasAnyTransfer);
    // 空闲状态隐藏传输控制，比保留两个禁用按钮更能把空间留给文件名；任务出现时再以稳定
    // 顺序显示，不会影响文件操作按钮的位置。
    m_pauseButton->setVisible(hasActive);
    m_cancelButton->setVisible(hasAnyTransfer);
    m_transferControlSeparator->setVisible(hasActive || hasAnyTransfer);
    m_transferQueueList->setVisible(hasAnyTransfer);
    m_transferPanel->setVisible(hasAnyTransfer || m_progressBar->isVisible());
}

void RemoteFileManagerWidget::uploadLocalPaths(const ConnectionTarget &target,
                                               const QStringList &localPaths,
                                               const QString &remoteDirectory)
{
    if (!target.isValid()) {
        QMessageBox::information(this, tr("Not connected yet"), tr("Connect to and select a remote server first."));
        return;
    }
    if (remoteDirectory.isEmpty() || !remoteDirectory.startsWith(QLatin1Char('/'))) {
        QMessageBox::warning(this,
                             tr("Remote path unknown"),
                             tr("Enter a remote directory first, then drop local files."));
        return;
    }

    QList<QFileInfo> acceptedEntries;
    for (const QString &localPath : localPaths) {
        const QFileInfo info(localPath);
        if (!info.exists() || info.isSymbolicLink()) {
            // 拖拽入口也遵守递归传输“不跟随符号链接”的边界。单个链接若被当作普通文件
            // 上传，可能把用户未选择的目标内容泄漏到服务器。
            continue;
        }
        if (info.isDir() || info.isFile()) {
            acceptedEntries.append(info);
        }
    }
    if (acceptedEntries.isEmpty()) {
        QMessageBox::information(this,
                                 tr("Nothing to upload"),
                                 tr("The dropped item does not exist, cannot be read, or is a symlink that is not followed."));
        return;
    }

    const SftpTransfer::ExistingFilePolicy policy = selectedTransferPolicy();
    qsizetype queuedNow = 0;
    for (const QFileInfo &info : std::as_const(acceptedEntries)) {
        if (info.isDir()) {
            // 目录要先问清楚"合并还是替换"，因此不能在这里直接入队；探测结果回来后才决定。
            requestDirectoryUpload(target, info.absoluteFilePath(), remoteDirectory);
            continue;
        }
        QueuedTransfer transfer;
        transfer.kind = QueuedTransferKind::UploadFile;
        transfer.target = target;
        transfer.sourcePath = info.absoluteFilePath();
        transfer.destinationPath = joinRemotePath(remoteDirectory, info.fileName());
        transfer.policy = policy;
        transfer.description = tr("Upload %1 → %2")
                                   .arg(info.fileName(), remoteDirectory);
        enqueueTransfer(std::move(transfer));
        ++queuedNow;
    }
    if (queuedNow > 0) {
        emit statusMessage(tr("Added %1 local files to the transfer queue").arg(queuedNow));
    }
}

void RemoteFileManagerWidget::requestDirectoryUpload(const ConnectionTarget &target,
                                                     const QString &localDirectory,
                                                     const QString &remoteParentDirectory)
{
    PendingDirectoryUpload pending;
    pending.target = target;
    pending.localDirectory = localDirectory;
    pending.remoteParentDirectory = remoteParentDirectory;
    pending.remoteRoot = joinRemotePath(remoteParentDirectory, QDir(localDirectory).dirName());
    pending.policy = selectedTransferPolicy();

    const quint64 requestId = m_service->statPath(target, pending.remoteRoot);
    m_pendingDirectoryChecks.insert(requestId, pending);
}

void RemoteFileManagerWidget::enqueueDirectoryUpload(
    const PendingDirectoryUpload &pending,
    const SftpTransfer::DirectoryConflictPolicy conflictPolicy)
{
    QueuedTransfer transfer;
    transfer.kind = QueuedTransferKind::UploadDirectory;
    transfer.target = pending.target;
    transfer.sourcePath = pending.localDirectory;
    // 目录 API 接收远程父目录，文件 API 接收完整目标路径，两者不能混用。
    transfer.destinationPath = pending.remoteParentDirectory;
    transfer.policy = pending.policy;
    transfer.conflictPolicy = conflictPolicy;
    transfer.description = conflictPolicy == SftpTransfer::DirectoryConflictPolicy::Replace
        ? tr("Replace upload %1 → %2").arg(QDir(pending.localDirectory).dirName(),
                                     pending.remoteParentDirectory)
        : tr("Upload %1 → %2").arg(QDir(pending.localDirectory).dirName(),
                                 pending.remoteParentDirectory);
    enqueueTransfer(std::move(transfer));
    emit statusMessage(tr("Added directory %1 to the transfer queue").arg(pending.remoteRoot));
}

void RemoteFileManagerWidget::downloadRemoteEntryToDirectory(
    const RemoteFileInfo &entry,
    const QString &localDirectory)
{
    const QFileInfo destinationInfo(localDirectory);
    if (!destinationInfo.exists() || !destinationInfo.isDir()) {
        QMessageBox::warning(this,
                             tr("Invalid local path"),
                             tr("The download target is not an accessible directory:\n%1").arg(localDirectory));
        return;
    }
    if (entry.path.isEmpty() || entry.name.isEmpty()) {
        return;
    }

    QueuedTransfer transfer;
    transfer.kind = entry.isDirectory ? QueuedTransferKind::DownloadDirectory
                                      : QueuedTransferKind::DownloadFile;
    transfer.target = selectedTarget();
    transfer.sourcePath = entry.path;
    // 目录 API 接收本地父目录；文件 API 必须得到包含文件名的完整目标路径。
    transfer.destinationPath = entry.isDirectory
        ? destinationInfo.absoluteFilePath()
        : QDir(destinationInfo.absoluteFilePath()).filePath(entry.name);
    transfer.policy = selectedTransferPolicy();
    transfer.description = tr("Download %1 → %2")
                               .arg(entry.name, destinationInfo.absoluteFilePath());
    enqueueTransfer(std::move(transfer));
}

void RemoteFileManagerWidget::followTerminalWorkingDirectory(const QString &stableKey,
                                                              const QString &workingDirectory)
{
    if (!stableKey.isEmpty() && !workingDirectory.isEmpty()) {
        // 缓存必须先于开关判断更新：关闭状态下收到的 cwd 正是用户稍后勾选时需要立即应用
        // 的“当前目录”。按服务器隔离可以防止嵌套会话不同层之间互相覆盖路径。
        m_terminalWorkingDirectoryByTarget.insert(stableKey, workingDirectory);
    }

    const ConnectionTarget target = selectedTarget();
    if (!SftpUiState::shouldFollowTerminalWorkingDirectory(
            m_followTerminalCwdCheck->isChecked(), stableKey, target.stableKey(),
            workingDirectory)) {
        return;
    }
    if (m_pathEdit->text().trimmed() == workingDirectory) {
        // 提示符每次刷新都会上报一次；路径没变时不必重新拉取目录列表。
        return;
    }
    m_pathEdit->setText(workingDirectory);
    refreshDirectory();
}

void RemoteFileManagerWidget::restorePathForSelectedTarget()
{
    const ConnectionTarget target = selectedTarget();
    if (!target.isValid()) {
        return;
    }
    // 没有记录过的服务器留空，由服务端解析出主目录；不能沿用上一台的路径。
    m_pathEdit->setText(m_pathByTarget.value(target.stableKey()));
}

void RemoteFileManagerWidget::refreshDirectory()
{
    const ConnectionTarget target = selectedTarget();
    if (!target.isValid()) {
        return;
    }
    const QString path = m_pathEdit->text().trimmed();
    if (!path.isEmpty() && !path.startsWith(QLatin1Char('/'))) {
        QMessageBox::warning(this, tr("Invalid path"), tr("Enter a remote absolute path starting with /."));
        return;
    }

    // 空路径交给服务端解析成该账号的主目录；解析结果会由 directoryListed 带回来。
    setBusy(true, path.isEmpty() ? tr("Reading home directory…") : tr("Reading %1…").arg(path));
    m_latestListRequest = m_service->listDirectory(target, path);
}

void RemoteFileManagerWidget::activateItem(QTreeWidgetItem *item, const int column)
{
    Q_UNUSED(column)
    if (item == nullptr) {
        return;
    }
    const bool directory = item->data(0, Qt::UserRole + 1).toBool();
    if (directory) {
        m_pathEdit->setText(item->data(0, Qt::UserRole).toString());
        refreshDirectory();
    } else {
        downloadSelectedFile();
    }
}

void RemoteFileManagerWidget::uploadSelectedFile()
{
    const QStringList localPaths = QFileDialog::getOpenFileNames(
        this, tr("Select one or more files to upload"), m_localPathEdit->text().trimmed());
    if (localPaths.isEmpty()) {
        return;
    }
    uploadLocalPaths(selectedTarget(), localPaths, m_pathEdit->text().trimmed());
}

void RemoteFileManagerWidget::requestUploadToCurrentDirectory()
{
    // 从终端右键进入时文件管理面板可能处于隐藏或尚未加载路径的状态。必须在弹出本地文件
    // 选择器之前验证目标，避免用户选完文件后才得知没有可上传的服务器或目录。
    if (!selectedTarget().isValid()) {
        QMessageBox::information(this, tr("Not connected yet"), tr("Connect to and select a remote server first."));
        return;
    }
    const QString remoteDirectory = m_pathEdit->text().trimmed();
    if (remoteDirectory.isEmpty() || !remoteDirectory.startsWith(QLatin1Char('/'))) {
        QMessageBox::warning(this,
                             tr("Remote path unknown"),
                             tr("Enter a directory in the remote file manager first, then start the upload from the terminal."));
        return;
    }

    // 校验通过后仍走和页面操作按钮完全相同的选择与队列方法，不创建第二套隐式传输状态。
    uploadSelectedFile();
}

void RemoteFileManagerWidget::uploadFilesToDirectory(const ConnectionTarget &target,
                                                      const QString &remoteDirectory,
                                                      const QStringList &localPaths)
{
    // 直接复用 uploadLocalPaths 的路径校验、符号链接过滤和队列逻辑；target 完全来自调用方
    // （拖拽发生时的终端活动层），不读取面板自身的 selectedTarget()/m_pathEdit，因此面板
    // 隐藏、未展开或选中了另一台服务器都不影响这次上传落到正确的层。
    uploadLocalPaths(target, localPaths, remoteDirectory);
}

void RemoteFileManagerWidget::uploadSelectedDirectory()
{
    const QString localDirectory = QFileDialog::getExistingDirectory(
        this, tr("Select a directory to upload"), m_localPathEdit->text().trimmed());
    if (localDirectory.isEmpty()) {
        return;
    }
    const QString remoteParent = m_pathEdit->text().trimmed();
    if (remoteParent.isEmpty()) {
        QMessageBox::warning(this, tr("Path unknown"),
                             tr("Enter a remote directory first, then upload the local directory."));
        return;
    }

    // 目录上传是递归写入：一次点击可能覆盖远端成百上千个文件，且规模在选择器里完全看不出
    // 来。因此在入队前明确复述"哪个目录写到哪里"，让用户有机会发现选错了目录。
    const QString remoteRoot = remoteParent.endsWith(QLatin1Char('/'))
        ? remoteParent + QDir(localDirectory).dirName()
        : remoteParent + QLatin1Char('/') + QDir(localDirectory).dirName();
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this,
        tr("Confirm directory upload"),
        tr("The local directory will be uploaded recursively:\n%1\n\nto the remote location:\n%2\n\nIf a directory with the same name already exists on the remote, the next step lets you choose merge update or replace. Continue?")
            .arg(QDir::toNativeSeparators(localDirectory), remoteRoot),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (choice != QMessageBox::Yes) {
        return;
    }
    uploadLocalPaths(selectedTarget(), {localDirectory}, remoteParent);
}

void RemoteFileManagerWidget::downloadSelectedFile()
{
    const QList<RemoteFileInfo> entries = selectedFiles();
    if (entries.isEmpty()) {
        QMessageBox::information(this, tr("Select an item"), tr("Select a remote file or directory to download."));
        return;
    }
    const QString localParent = QFileDialog::getExistingDirectory(
        this, tr("Choose the local directory for bulk download"), m_localPathEdit->text().trimmed());
    if (localParent.isEmpty()) {
        return;
    }
    for (const RemoteFileInfo &entry : entries) {
        downloadRemoteEntryToDirectory(entry, localParent);
    }
}

void RemoteFileManagerWidget::cancelActiveTransfer()
{
    quint64 selectedQueueId = 0;
    if (m_transferQueueList->currentItem() != nullptr) {
        selectedQueueId = m_transferQueueList->currentItem()->data(Qt::UserRole).toULongLong();
    }

    if (selectedQueueId != 0
        && (!m_activeQueuedTransfer.has_value()
            || selectedQueueId != m_activeQueuedTransfer->queueId)) {
        for (auto iterator = m_pendingTransfers.begin(); iterator != m_pendingTransfers.end(); ++iterator) {
            if (iterator->queueId == selectedQueueId) {
                const QString description = iterator->description;
                m_pendingTransfers.erase(iterator);
                rebuildTransferQueueView();
                const QString message = tr("Waiting item cancelled: %1").arg(description);
                m_statusLabel->setText(message);
                emit statusMessage(message);
                return;
            }
        }
    }

    if (m_activeTransferRequest == 0 || !m_activeQueuedTransfer.has_value()) {
        return;
    }
    m_service->cancelRequest(m_activeTransferRequest);
    // 后台任务要跑到下一个分块边界才会停，先把按钮禁掉避免用户反复点击并以为没生效。
    m_cancelButton->setEnabled(false);
    const QString message = tr("Transfer cancellation requested; finishing up…");
    m_statusLabel->setText(message);
    emit statusMessage(message);
}

void RemoteFileManagerWidget::toggleActiveTransferPause()
{
    if (m_activeTransferRequest == 0 || !m_activeQueuedTransfer.has_value()) {
        return;
    }
    m_activeTransferPaused = !m_activeTransferPaused;
    if (m_activeTransferPaused) {
        m_service->pauseRequest(m_activeTransferRequest);
        m_pauseButton->setText(tr("Resume"));
        m_pauseButton->setToolTip(tr("Resume the current transfer"));
        m_pauseButton->setAccessibleName(tr("Resume the current transfer"));
        m_pauseButton->setIcon(
            UiIcons::symbol(this, UiIcons::Symbol::Play, this->palette().color(QPalette::WindowText)));
        emit statusMessage(tr("The transfer will pause at the next 64 KiB chunk boundary"));
    } else {
        m_service->resumeRequest(m_activeTransferRequest);
        m_pauseButton->setText(tr("Pause"));
        m_pauseButton->setToolTip(tr("Pause the current transfer"));
        m_pauseButton->setAccessibleName(tr("Pause the current transfer"));
        m_pauseButton->setIcon(
            UiIcons::symbol(this, UiIcons::Symbol::Pause, this->palette().color(QPalette::WindowText)));
        emit statusMessage(tr("Transfer resumed"));
    }
    rebuildTransferQueueView();
}

void RemoteFileManagerWidget::previewSelectedFile()
{
    const std::optional<RemoteFileInfo> entry = selectedFile();
    if (!entry.has_value() || entry->isDirectory || entry->isSymbolicLink) {
        QMessageBox::information(this,
                                 tr("Cannot preview"),
                                 tr("Select a regular remote file; directories and symlinks are not opened in the safe preview."));
        return;
    }
    setBusy(true, tr("Safely reading the preview of %1…").arg(entry->name));
    m_service->previewFile(selectedTarget(),
                           entry->path,
                           SftpTransfer::MaximumPreviewBytes);
}

void RemoteFileManagerWidget::chmodSelectedPath()
{
    const std::optional<RemoteFileInfo> entry = selectedFile();
    if (!entry.has_value()) {
        QMessageBox::information(this, tr("Select an item"), tr("Select a file or directory to change permissions for."));
        return;
    }
    bool accepted = false;
    const QString initial = formatPermissions(entry->permissions);
    const QString input = QInputDialog::getText(
        this,
        tr("Change remote permissions"),
        tr("Enter three or four octal permission digits (e.g. 0644, 0755):"),
        QLineEdit::Normal,
        initial,
        &accepted).trimmed();
    if (!accepted) {
        return;
    }
    static const QRegularExpression octalPattern(QStringLiteral("^[0-7]{3,4}$"));
    if (!octalPattern.match(input).hasMatch()) {
        QMessageBox::warning(this,
                             tr("Invalid permission format"),
                             tr("Permissions may contain only 0 to 7 and must be three or four digits long."));
        return;
    }
    bool converted = false;
    const quint32 permissions = input.toUInt(&converted, 8);
    if (!converted || permissions > 07777U) {
        QMessageBox::warning(this, tr("Invalid permission format"), tr("The permission value is outside the range allowed by POSIX."));
        return;
    }
    setBusy(true, tr("Changing permissions of %1…").arg(entry->name));
    m_service->chmodPath(selectedTarget(), entry->path, permissions);
}

void RemoteFileManagerWidget::renameSelectedPath()
{
    const std::optional<RemoteFileInfo> entry = selectedFile();
    if (!entry.has_value()) {
        QMessageBox::information(this, tr("Select an item"), tr("Select a file or directory to rename."));
        return;
    }
    bool accepted = false;
    const QString newName = QInputDialog::getText(this,
                                                   tr("Rename"),
                                                   tr("New name:"),
                                                   QLineEdit::Normal,
                                                   entry->name,
                                                   &accepted).trimmed();
    if (!accepted || newName.isEmpty() || newName == entry->name) {
        return;
    }
    if (newName.contains(QLatin1Char('/'))) {
        QMessageBox::warning(this, tr("Invalid name"), tr("The name cannot contain the path separator /."));
        return;
    }
    const QString newPath = joinRemotePath(m_pathEdit->text(), newName);
    setBusy(true, tr("Renaming…"));
    m_service->renamePath(selectedTarget(), entry->path, newPath);
}

void RemoteFileManagerWidget::deleteSelectedPath()
{
    const std::optional<RemoteFileInfo> entry = selectedFile();
    if (!entry.has_value()) {
        QMessageBox::information(this, tr("Select an item"), tr("Select a file or empty directory to delete."));
        return;
    }
    const ConnectionTarget target = selectedTarget();
    // 确认框必须写明服务器：删除是不可撤销的，而用户很可能同时开着多层、多标签页，
    // 只显示路径不足以让人确认自己在哪台机器上操作。
    QString message = tr("You are about to permanently delete %2 on server %1:\n\n%3\n\nThis action cannot be undone.")
                          .arg(target.connectionSpecifier(true),
                               entry->isDirectory ? tr("Directory") : tr("File"),
                               entry->path);
    if (target.confidence == ConnectionTarget::Confidence::Inferred) {
        message += tr("\n\n⚠ This server was inferred by parsing terminal commands and may not be the one you expect. Run 'hostname' in the terminal to verify before continuing.");
    }
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this,
        tr("Confirm delete"),
        message,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    setBusy(true, tr("Deleting %1…").arg(entry->name));
    m_service->removePath(target, entry->path, entry->isDirectory);
}

void RemoteFileManagerWidget::navigateUp()
{
    QString path = m_pathEdit->text().trimmed();
    if (path.isEmpty() || path == QStringLiteral("/")) {
        m_pathEdit->setText(QStringLiteral("/"));
        return;
    }
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    const qsizetype separator = path.lastIndexOf(QLatin1Char('/'));
    m_pathEdit->setText(separator <= 0 ? QStringLiteral("/") : path.left(separator));
    refreshDirectory();
}

ConnectionTarget RemoteFileManagerWidget::selectedTarget() const
{
    const int index = m_targetCombo->currentIndex();
    return index >= 0 && index < m_targets.size() ? m_targets.at(index) : ConnectionTarget{};
}

std::optional<RemoteFileInfo> RemoteFileManagerWidget::selectedFile() const
{
    return fileFromItem(m_fileTree->currentItem());
}

QList<RemoteFileInfo> RemoteFileManagerWidget::selectedFiles() const
{
    QList<RemoteFileInfo> entries;
    for (QTreeWidgetItem *item : m_fileTree->selectedItems()) {
        const std::optional<RemoteFileInfo> entry = fileFromItem(item);
        if (entry.has_value()) {
            entries.append(*entry);
        }
    }
    return entries;
}

std::optional<RemoteFileInfo> RemoteFileManagerWidget::fileFromItem(
    QTreeWidgetItem *item) const
{
    if (item == nullptr) {
        return std::nullopt;
    }
    RemoteFileInfo entry;
    entry.name = item->text(0);
    entry.path = item->data(0, Qt::UserRole).toString();
    entry.isDirectory = item->data(0, Qt::UserRole + 1).toBool();
    entry.size = item->data(0, Qt::UserRole + 2).toULongLong();
    entry.permissions = item->data(0, Qt::UserRole + 3).toUInt();
    entry.isSymbolicLink = item->data(0, Qt::UserRole + 4).toBool();
    return entry;
}

void RemoteFileManagerWidget::setBusy(const bool busy, const QString &message)
{
    if (!message.isEmpty()) {
        m_statusLabel->setText(message);
        emit statusMessage(message);
    }
    if (busy) {
        m_transferDetailLabel->clear();
        m_progressBar->setRange(0, 0);
        m_progressBar->setVisible(true);
        m_transferPanel->setVisible(true);
    } else {
        m_progressBar->setVisible(false);
        m_progressBar->setRange(0, 100);
        // 清掉上一次传输遗留的字节/速度/剩余时间，否则下次信息带出现会先闪出旧摘要。
        m_transferDetailLabel->clear();
        m_transferPanel->setVisible(
            m_activeQueuedTransfer.has_value() || !m_pendingTransfers.isEmpty());
    }
}

void RemoteFileManagerWidget::handleRecursiveTransferFinished(
    const quint64 requestId,
    const SftpRecursive::TransferSummary &summary)
{
    if (requestId != m_activeTransferRequest) {
        return;
    }
    const QString summaryText = summary.toMessage();
    setBusy(false, summaryText.section(QLatin1Char('\n'), 0, 0));

    // 跳过和失败必须主动弹给用户：把"有 37 个文件没传成功"埋进一行状态栏文字里，等同于
    // 让用户以为整个目录都传完了。
    if (summary.hasWarnings()) {
        QMessageBox::warning(this, tr("The directory transfer has ended (some items are incomplete)"), summaryText);
    }
    const QString headline = summaryText.section(QLatin1Char('\n'), 0, 0);
    emit statusMessage(headline);
    emit transferResultAvailable(!summary.hasWarnings(), headline);
    refreshDirectory();
    finishQueuedTransfer(requestId);
}

void RemoteFileManagerWidget::handleOperationSuccess(const quint64 requestId,
                                                     const QString &description)
{
    // 必须在 finishQueuedTransfer 清空 m_activeTransferRequest 之前判断：这决定了刚刚成功的
    // 是一次排队的上传/下载，还是重命名、删除、预览这类没有"传输成功"语义的一次性操作。
    const bool wasActiveTransfer = requestId == m_activeTransferRequest;
    setBusy(false, description);
    refreshDirectory();
    finishQueuedTransfer(requestId);

    if (wasActiveTransfer) {
        emit transferResultAvailable(true, description);
    }
}

void RemoteFileManagerWidget::handleOperationFailure(const quint64 requestId,
                                                     const ConnectionTarget &target,
                                                     const SftpService::ErrorKind kind,
                                                     const QString &message)
{
    const bool wasActiveTransfer = requestId == m_activeTransferRequest;
    setBusy(false, message);

    // 目录冲突探测失败时对应的待决目录上传不会再有结果回来，必须一并清掉，否则它会一直
    // 挂在等待表里，而用户看到的是一个"点了上传却什么都没发生"的目录。
    m_pendingDirectoryChecks.remove(requestId);

    // 单文件失败以及递归传输在连接/根目录阶段的失败都走本信号。先释放活动队列项，再让
    // 事件循环启动下一项；后续认证或错误对话框仍可完整展示，不会卡死整个队列。
    if (wasActiveTransfer) {
        finishQueuedTransfer(requestId);
        emit transferResultAvailable(false, message);
    }

    if (wasActiveTransfer && message.contains(tr("Cancelled"))) {
        // 用户主动取消属于预期控制流，不应再弹红色错误框；保留半成品供下一次续传。
        emit statusMessage(message);
        return;
    }

    if (requestId == m_latestListRequest
        && kind == SftpService::ErrorKind::Connection
        && message.contains(QStringLiteral("disconnected"), Qt::CaseInsensitive)
        && target.stableKey() == selectedTarget().stableKey()) {
        int &retryCount = m_directoryDisconnectRetries[target.stableKey()];
        if (retryCount < 2) {
            ++retryCount;
            const int retryDelayMs = retryCount * 400;
            const QString retryMessage = tr("The jump host connection dropped temporarily; retrying (attempt %1)…")
                                             .arg(retryCount);
            m_statusLabel->setText(retryMessage);
            emit statusMessage(retryMessage);
            // 仅自动重试幂等的目录读取；上传、重命名和删除绝不能在结果未知时自动重复。
            QTimer::singleShot(retryDelayMs, this, [this, target]() {
                if (target.stableKey() == selectedTarget().stableKey()) {
                    refreshDirectory();
                }
            });
            return;
        }
    }

    if (requestId == m_latestListRequest
        && kind == SftpService::ErrorKind::RemoteOperation
        && target.stableKey() == selectedTarget().stableKey()
        && !m_pathEdit->text().trimmed().isEmpty()) {
        // 记录的目录可能已经被删除或改名，也可能是用户手输了错误路径。退回主目录，
        // 否则用户会被卡在一个打不开的位置，只能自己猜一个存在的路径。
        m_pathByTarget.remove(target.stableKey());
        m_pathEdit->clear();
        emit statusMessage(tr("%1; returned to home directory").arg(message));
        refreshDirectory();
        return;
    }

    if (kind == SftpService::ErrorKind::Authentication
        && target.stableKey() == selectedTarget().stableKey()) {
        if (target.interactiveAuthenticationPending) {
            // 内层 OpenSSH 可能正在显示第二次 password 提示。此时弹出独立输入框会迫使用户
            // 输入两次；保持文件管理器静默，TerminalPane 捕获下一次终端输入后会更新凭据
            // 并通过 targetsChanged 自动重新发起当前目录读取。
            const QString retryMessage = tr("Inner SSH authentication is not finished; re-enter the password directly in the terminal");
            m_statusLabel->setText(retryMessage);
            emit statusMessage(retryMessage);
            return;
        }

        if (m_credentialDialogOpen) {
            // 多个请求可能同时失败（例如切换目录时前一个请求尚未返回）。只保留一个对话框，
            // 否则用户会看到密码框层层叠加，且后面几个填了也没用。
            return;
        }
        m_credentialDialogOpen = true;
        bool accepted = false;
        const QString password = QInputDialog::getText(
            this,
            tr("Enter inner server credentials"),
            tr("The server rejected the current session credentials. Enter a new password or private key passphrase for %1:")
                .arg(target.connectionSpecifier(true)),
            QLineEdit::Password,
            {},
            &accepted);
        m_credentialDialogOpen = false;
        if (accepted && !password.isEmpty()) {
            const int selectedIndex = m_targetCombo->currentIndex();
            if (selectedIndex >= 0 && selectedIndex < m_targets.size()) {
                // 密码只保存在当前进程的目标副本中，不写入数据库、日志或错误信息。
                m_targets[selectedIndex].password = password;
                m_sessionSecrets.insert(m_targets[selectedIndex].stableKey(), password);
                refreshDirectory();
            }
        }
        return;
    }

    if (kind == SftpService::ErrorKind::LegacyAlgorithmRequired
        && target.stableKey() == selectedTarget().stableKey()) {
        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this,
            tr("The server only supports legacy MAC algorithms"),
            tr("The SFTP service of server %1 offers only hmac-sha1.\n\nThis algorithm is obsolete; upgrading the server's SSH configuration is recommended. Enable it for this connection only and retry?\n\n%2")
                .arg(target.connectionSpecifier(true), message),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer == QMessageBox::Yes) {
            const int selectedIndex = m_targetCombo->currentIndex();
            if (selectedIndex >= 0 && selectedIndex < m_targets.size()) {
                // 兼容标志只写入当前文件管理器内存中的目标副本，不影响其他主机或后续新连接。
                m_targets[selectedIndex].allowLegacyHmacSha1 = true;
                refreshDirectory();
            }
        }
        return;
    }

    // 未知主机错误紧随指纹确认信号到达，由确认对话框负责提示，避免重复弹窗。
    if (kind != SftpService::ErrorKind::HostKeyUnknown
        && kind != SftpService::ErrorKind::HostKeyChanged) {
        QMessageBox::critical(this, tr("SFTP operation failed"), message);
    }
}

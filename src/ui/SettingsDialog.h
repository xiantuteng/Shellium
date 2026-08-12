#pragma once

#include "settings/TerminalSettings.h"

#include <QDialog>
#include <QHash>
#include <QList>

class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLineEdit;
class QKeySequenceEdit;
class QLabel;
class QListWidget;
class QSpinBox;
class QStackedWidget;
class SegmentedControl;
class SettingsCard;
class TerminalSettingsService;
class CredentialVault;
class ThemeSwatchButton;
class ToggleSwitch;

/**
 * @brief 编辑主题、终端外观、滚动行为、铃声和全局快捷键的设置窗口。
 *
 * 版式沿用主流终端客户端的设置形态：左侧是固定的分类导航，右侧是可滚动的分组卡片，每张
 * 卡片内部是“左侧说明 + 右侧控件”的行。布尔项用滑动开关、数值项用加减步进器、少量互斥项
 * 用分段控件、终端配色用可直接看出底色的色板卡，避免整页都是尺寸不一的原生表单控件。
 *
 * 交互契约与旧版一致：用户调整任一控件即调用 TerminalSettingsService::applyTransient()，所有
 * 已订阅工作区立即预览效果；取消对话框会恢复打开前快照，保存则通过服务的单事务 save()
 * 持久化。这样既有实时反馈，也不会让“取消”意外改写 SQLite。
 */
class SettingsDialog final : public QDialog {
    Q_OBJECT

public:
    /**
     * @brief 创建设置对话框并载入服务当前快照。
     * @param service 应用级设置服务，生命周期必须覆盖本对话框且不可为空。
     * @param parent Qt 父窗口。
     */
    explicit SettingsDialog(TerminalSettingsService *service, QWidget *parent = nullptr);

    /**
     * @brief 创建可管理保险箱主密码的设置对话框。
     * @param service 应用级设置服务，生命周期必须覆盖本对话框且不可为空。
     * @param credentialVault 应用共享凭据保险库；启用主密码时用于创建或解锁保险库。
     * @param parent Qt 父窗口。
     */
    SettingsDialog(TerminalSettingsService *service,
                   CredentialVault *credentialVault,
                   QWidget *parent);

    /** @brief 返回当前控件组成的设置快照，主要供测试和扩展页复用。 */
    [[nodiscard]] TerminalSettings editedSettings() const;

public slots:
    /** @brief 校验并持久化当前设置；失败时保留对话框并展示原因。 */
    void accept() override;

    /** @brief 恢复打开前设置后关闭，确保实时预览可以完整撤销。 */
    void reject() override;

private:
    /** @brief 构建左侧分类导航，并与右侧页面栈双向同步。 */
    void buildNavigation();

    /**
     * @brief 按关键词过滤配色色板，并把可见卡片重新排成两列。
     * @param filter 关键词；为空表示展示全部配色。
     *
     * 直接隐藏网格里的卡片会在原位留下空洞（QGridLayout 不会自动收拢隐藏项），因此每次过滤
     * 都重新分配行列，让可见卡片始终紧密排列。
     */
    void relayoutColorSchemeGrid(const QString &filter);

    /** @brief 用推荐编程字体 + 本机全部等宽字体填充字体下拉框。 */
    void populateFontCombo();

    /** @brief 构建“外观”页：应用主题、终端配色色板与实时预览。 */
    QWidget *buildAppearancePage();

    /** @brief 构建“终端”页：字体度量、光标与会话行为。 */
    QWidget *buildTerminalPage();

    /** @brief 构建“连接与安全”页：凭据保险箱和异常断线自动重连策略。 */
    QWidget *buildConnectionPage();

    /** @brief 构建“快捷键”页：按稳定动作 ID 列出全部可配置动作。 */
    QWidget *buildShortcutPage();

    /** @brief 把页面包进滚动区，保证小屏幕上底部按钮不会被卡片挤出窗口。 */
    QWidget *wrapInScrollArea(QWidget *page);

    /** @brief 从设置快照刷新全部控件；刷新期间不会触发瞬态应用。 */
    void populateControls(const TerminalSettings &settings);

    /** @brief 把当前控件值实时应用到服务，并同步主题预览卡。 */
    void previewChanges();

    /** @brief 根据当前有效主题与字体刷新对话框内的示例终端。 */
    void updatePreviewCard(const TerminalSettings &settings);

    /** @brief 让“默认（跟随主题）”色板卡跟随当前主题模式重绘。 */
    void refreshDefaultSwatchPreview(const TerminalSettings &settings);

    TerminalSettingsService *m_service = nullptr;
    TerminalSettings m_originalSettings;
    bool m_populating = false;

    QListWidget *m_navigationList = nullptr;
    QStackedWidget *m_pages = nullptr;

    SegmentedControl *m_themeSelector = nullptr;
    /** @brief 终端配色色板卡，按配色预设 id 索引。 */
    QHash<QString, ThemeSwatchButton *> m_colorSchemeSwatches;
    /** @brief 配色 id 的展示顺序，过滤后重排网格时据此保持稳定次序。 */
    QStringList m_colorSchemeOrder;
    QWidget *m_colorSchemeGrid = nullptr;
    QLineEdit *m_colorSchemeFilter = nullptr;
    /** @brief 过滤结果为空时显示的提示，避免只留下一块空白区域。 */
    QLabel *m_colorSchemeEmptyLabel = nullptr;
    QComboBox *m_fontCombo = nullptr;
    QSpinBox *m_fontSizeSpin = nullptr;
    QSpinBox *m_lineHeightSpin = nullptr;
    QDoubleSpinBox *m_letterSpacingSpin = nullptr;
    SegmentedControl *m_cursorShapeSelector = nullptr;
    ToggleSwitch *m_cursorBlinkToggle = nullptr;
    QSpinBox *m_scrollbackSpin = nullptr;
    ToggleSwitch *m_audibleBellToggle = nullptr;
    /** @brief 是否在普通命令输入达到最小前缀长度后自动展示历史命令候选。 */
    ToggleSwitch *m_commandAutoCompletionToggle = nullptr;
    /** @brief 是否对已建立会话的异常网络断线启用有限自动重连。 */
    ToggleSwitch *m_automaticReconnectToggle = nullptr;
    /** @brief 是否要求用用户主密码保护自动保存的主机凭据。 */
    ToggleSwitch *m_vaultPasswordEnabledToggle = nullptr;
    /** @brief 每次异常断线周期的最大尝试次数。 */
    QSpinBox *m_reconnectMaxAttemptsSpin = nullptr;
    /** @brief 指数退避首轮延迟秒数。 */
    QSpinBox *m_reconnectBaseDelaySpin = nullptr;
    QLabel *m_previewLabel = nullptr;
    /** @brief 重连相关行的容器，关闭自动重连时整体置灰。 */
    QList<QWidget *> m_reconnectDependentRows;

    /** @brief 应用共享凭据保险库；测试旧构造入口时可为空。 */
    CredentialVault *m_credentialVault = nullptr;

    /** @brief 以稳定动作 ID 索引快捷键编辑器，保存时无需依赖界面显示文本。 */
    QHash<QString, QKeySequenceEdit *> m_shortcutEditors;
};

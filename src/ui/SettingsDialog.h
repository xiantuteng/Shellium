#pragma once

#include "settings/TerminalSettings.h"

#include <QDialog>
#include <QHash>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QKeySequenceEdit;
class QLabel;
class QSpinBox;
class TerminalSettingsService;

/**
 * @brief 编辑主题、终端外观、滚动行为、铃声和全局快捷键的设置对话框。
 *
 * 用户调整任一控件时会调用 TerminalSettingsService::applyTransient()，所有已订阅工作区立即
 * 预览效果；取消对话框会恢复打开前快照，确定则通过服务的单事务 save() 持久化。这样既有
 * 实时反馈，也不会让“取消”意外改写 SQLite。
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

    /** @brief 返回当前控件组成的设置快照，主要供测试和扩展页复用。 */
    [[nodiscard]] TerminalSettings editedSettings() const;

public slots:
    /** @brief 校验并持久化当前设置；失败时保留对话框并展示原因。 */
    void accept() override;

    /** @brief 恢复打开前设置后关闭，确保实时预览可以完整撤销。 */
    void reject() override;

private:
    /** @brief 从设置快照刷新全部控件；刷新期间不会触发瞬态应用。 */
    void populateControls(const TerminalSettings &settings);

    /** @brief 把当前控件值实时应用到服务，并同步主题预览卡。 */
    void previewChanges();

    /** @brief 根据当前有效主题与字体刷新对话框内的示例终端。 */
    void updatePreviewCard(const TerminalSettings &settings);

    TerminalSettingsService *m_service = nullptr;
    TerminalSettings m_originalSettings;
    bool m_populating = false;

    QComboBox *m_themeCombo = nullptr;
    QFontComboBox *m_fontCombo = nullptr;
    QSpinBox *m_fontSizeSpin = nullptr;
    QSpinBox *m_lineHeightSpin = nullptr;
    QDoubleSpinBox *m_letterSpacingSpin = nullptr;
    QComboBox *m_cursorShapeCombo = nullptr;
    QCheckBox *m_cursorBlinkCheck = nullptr;
    QSpinBox *m_scrollbackSpin = nullptr;
    QCheckBox *m_audibleBellCheck = nullptr;
    /** @brief 是否对已建立会话的异常网络断线启用有限自动重连。 */
    QCheckBox *m_automaticReconnectCheck = nullptr;
    /** @brief 每次异常断线周期的最大尝试次数。 */
    QSpinBox *m_reconnectMaxAttemptsSpin = nullptr;
    /** @brief 指数退避首轮延迟秒数。 */
    QSpinBox *m_reconnectBaseDelaySpin = nullptr;
    QLabel *m_previewLabel = nullptr;

    /** @brief 以稳定动作 ID 索引快捷键编辑器，保存时无需依赖界面显示文本。 */
    QHash<QString, QKeySequenceEdit *> m_shortcutEditors;
};

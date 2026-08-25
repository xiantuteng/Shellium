#pragma once

#include "ai/AiSettings.h"

#include <QObject>
#include <QString>

class ApplicationDatabase;
class CredentialVault;

/**
 * @brief 加载、校验并持久化 AI 助手配置的应用级服务。
 *
 * 端点地址和模型名不敏感，直接以字符串设置项存进 ApplicationDatabase；API Key 复用
 * CredentialVault 现有的通用密钥存取入口（CredentialType::Token），以固定 id 保存，
 * 因此加密强度、主密码保护和自动锁定与主机密码完全一致，不需要另起一套加密方案。
 */
class AiSettingsService final : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 创建服务。
     * @param database 已初始化的应用数据库，生命周期必须覆盖本对象，可为空以支持纯内存场景。
     * @param credentialVault 应用共享凭据保险库，生命周期必须覆盖本对象，可为空以支持纯内存场景。
     * @param parent Qt 父对象。
     */
    explicit AiSettingsService(ApplicationDatabase *database,
                               CredentialVault *credentialVault,
                               QObject *parent = nullptr);

    /**
     * @brief 从数据库和保险库加载当前配置。
     * @param errorMessage 保险库解锁失败等场景下接收原因；密钥缺失（首次使用）不算错误。
     * @return 端点/模型读取成功即返回 true，即使密钥暂时无法解密也不影响返回值——界面仍需
     *         展示已保存的端点和模型，只是密钥栏留空。
     */
    bool load(QString *errorMessage = nullptr);

    /**
     * @brief 保存配置：端点/模型写入数据库，密钥写入保险库。
     * @param settings 待保存的完整快照。
     * @param errorMessage 数据库或保险库写入失败时接收原因。
     * @return 全部字段持久化成功返回 true。
     *
     * apiKey 为空时会删除已保存的密钥而不是写入空密文，避免出现"看起来配置了但其实是
     * 空字符串"的密文记录。
     */
    bool save(const AiSettings &settings, QString *errorMessage = nullptr);

    /** @brief 返回最近一次成功加载或保存的配置快照。 */
    [[nodiscard]] AiSettings currentSettings() const { return m_current; }

signals:
    /** @brief 配置发生变化，供已打开的 AI 助手面板和设置页同步刷新。 */
    void settingsChanged(const AiSettings &settings);

private:
    /** @brief 保存 AI Key 使用的固定凭据 id；服务全局只有一份配置，不需要按主机区分。 */
    static QString apiKeyCredentialId();

    ApplicationDatabase *m_database = nullptr;
    CredentialVault *m_credentialVault = nullptr;
    AiSettings m_current;
};

#pragma once

#include "core/HostRecord.h"

#include <QByteArray>
#include <QList>
#include <QString>

class ApplicationDatabase;

/**
 * @brief OpenSSH config 与 Shellium JSON 主机配置的严格导入导出服务。
 *
 * 编解码器只处理主机和分组配置，从不读取 CredentialVault，也不会输出 credentialId、密码、
 * 私钥正文或凭据密文。遇到 Shellium 无法忠实表达的 OpenSSH 条件、通配符或未知指令时明确
 * 拒绝整个输入，避免导入一个语义已悄悄改变的连接配置。
 */
class ConnectionConfigCodec final {
public:
    /** @brief 当前 Shellium JSON 交换格式版本。 */
    static constexpr int JsonFormatVersion = 1;

    /**
     * @brief 解析 OpenSSH config。
     * @param content UTF-8 配置内容。
     * @param records 成功时接收扁平主机列表；每条记录均使用随机新 id。
     * @param errorMessage 失败时接收行号和原因；可为空。
     */
    static bool importOpenSsh(const QByteArray &content,
                              QList<HostRecord> *records,
                              QString *errorMessage = nullptr);

    /**
     * @brief 导出可被 OpenSSH 直接读取的配置。
     * @param resolvedHosts 已完成分组继承解析的主机；分组节点会被忽略。
     * @return UTF-8 配置；参数无效时返回空并写入错误。
     */
    [[nodiscard]] static QByteArray exportOpenSsh(const QList<HostRecord> &resolvedHosts,
                                                  QString *errorMessage = nullptr);

    /**
     * @brief 解析 Shellium JSON 配置并进行完整结构、父子引用和敏感字段检查。
     * @return 成功返回 true，records 保留输入 id 供后续父子映射。
     */
    static bool importJson(const QByteArray &content,
                           QList<HostRecord> *records,
                           QString *errorMessage = nullptr);

    /**
     * @brief 导出主机与分组为 JSON；凭据引用和全部秘密字段被刻意省略。
     */
    [[nodiscard]] static QByteArray exportJson(const QList<HostRecord> &records,
                                               QString *errorMessage = nullptr);

    /**
     * @brief 用新 UUID 保存一批已解析记录，并保持其内部父子关系。
     * @param database 已初始化数据库。
     * @param records importOpenSsh/importJson 的结果。
     * @param attachRootParentId 导入根节点要挂到的现有分组；为空表示数据库根层级。
     * @param errorMessage 失败原因输出；任一步失败均整体回滚。
     * @return 全部记录写入成功返回 true。
     *
     * 始终重新生成 id，避免导入文件覆盖本机同 id 主机；credentialId 同样强制清空，防止
     * 外部配置引用本机恰好同名的秘密记录。
     */
    static bool persistImported(ApplicationDatabase &database,
                                const QList<HostRecord> &records,
                                const QString &attachRootParentId = {},
                                QString *errorMessage = nullptr);

private:
    ConnectionConfigCodec() = delete;
};


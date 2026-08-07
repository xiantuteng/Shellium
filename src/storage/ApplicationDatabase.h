#pragma once

#include <QSqlDatabase>
#include <QString>

/**
 * @brief 管理 Shellium SQLite 数据库连接、事务与版本化建表。
 *
 * 每个实例使用独立 Qt SQL 连接名，测试可指向临时文件；生产环境默认存储在系统应用数据
 * 目录。数据库只保存结构化配置，凭据正文必须先由 Vault 加密，禁止明文写入。
 */
class ApplicationDatabase final {
public:
    explicit ApplicationDatabase(QString databasePath = {});
    ~ApplicationDatabase();

    ApplicationDatabase(const ApplicationDatabase &) = delete;
    ApplicationDatabase &operator=(const ApplicationDatabase &) = delete;

    /**
     * @brief 打开数据库并执行全部幂等迁移。
     * @param errorMessage 失败时接收可读错误；可为空。
     * @return 初始化成功返回 true。
     */
    bool initialize(QString *errorMessage = nullptr);

    /** @brief 返回已打开的 Qt SQL 连接，供 Repository 使用参数化查询。 */
    [[nodiscard]] QSqlDatabase connection() const;

    /** @brief 返回实际数据库文件路径，便于备份和问题诊断。 */
    [[nodiscard]] QString databasePath() const;

private:
    /** @brief 在当前事务中执行单条无参数迁移 SQL。 */
    bool executeMigration(const QString &sql, QString *errorMessage);

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase m_database;
};

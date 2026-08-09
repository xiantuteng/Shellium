#pragma once

#include <QMutex>
#include <QString>

/**
 * @brief 应用运行日志：会话状态、错误与提示的唯一落点。
 *
 * 界面上的瞬时提示条已被移除——它出现在标题栏右上角，既遮挡标签又会在用户读完之前消失，
 * 而这些消息真正的用途是事后排查。因此所有原本发往提示条的文本改为追加到本日志文件，
 * 界面不再为它们保留任何位置。
 *
 * 该类可能被界面线程之外的对象调用（SFTP、端口转发都会汇报状态），因此写入受互斥量保护。
 */
class ActivityLog
{
public:
    /** @brief 进程内唯一实例；日志文件在首次写入时才创建。 */
    static ActivityLog &instance();

    /** @brief 默认日志路径：应用数据目录下的 logs/shellium.log。 */
    static QString defaultFilePath();

    /**
     * @brief 覆盖日志文件路径。
     *
     * 仅供测试与命令行诊断使用；正常启动不会调用，日志始终落在用户数据目录。
     */
    void setFilePath(const QString &path);

    /** @brief 当前日志文件的绝对路径。 */
    QString filePath() const;

    /**
     * @brief 追加一条带时间戳的消息；空消息（清除提示条的旧语义）被忽略。
     * @return 写入成功返回 true；磁盘不可写时返回 false，调用方无需处理——日志失败绝不
     *         能反过来打断用户正在进行的会话操作。
     */
    bool append(const QString &message);

private:
    ActivityLog();

    /** @brief 单个日志文件的字节上限；超出后轮转为 .1，只保留一代历史。 */
    static constexpr qint64 kMaximumFileBytes = 1024 * 1024;

    void rotateIfNeededLocked();

    mutable QMutex m_mutex;
    QString m_filePath;
};

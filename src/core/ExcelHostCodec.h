#pragma once

#include <QList>
#include <QString>

/**
 * @brief 面向主机批量导入的最小 Excel 工作簿读写服务。
 *
 * Shellium 只需要处理一个首工作表和四个主机字段，因此这里没有引入重量级表格编辑器
 * 依赖，而是实现标准 .xlsx（ZIP + XML）中本功能所需的子集。服务层不接触数据库和保险库，
 * 只负责把工作簿转换成无敏感副作用的行数据；密码是否加密保存由 HostManagerWidget 交给
 * CredentialVault 决定，避免文件格式层绕过统一的凭据安全边界。
 */
namespace ExcelHostCodec {

/**
 * @brief Excel 模板中一行主机数据的领域表示。
 *
 * name 与 host 是必填字段；username 和 password 允许为空，分别表示沿用父分组配置或不
 * 保存密码。password 只在导入过程的进程内短暂存在，调用方不得把它直接写入数据库或日志。
 */
struct HostRow {
    QString name;     ///< 主机显示名称，对应 Excel 的 name 列。
    QString host;     ///< 主机地址或域名，对应 Excel 的 host 列。
    QString username; ///< 登录账号，对应 Excel 的 username 列；允许为空。
    QString password; ///< 登录密码，对应 Excel 的 password 列；允许为空且不得明文落盘。
};

/**
 * @brief 生成带英文列名的标准 Excel 模板。
 * @param filePath 要写入的 .xlsx 文件路径；父目录必须已存在且具有写权限。
 * @param errorMessage 写入失败或 ZIP/XML 生成失败时接收可读错误；可为空。
 * @return 模板成功写入并完整关闭时返回 true；失败时返回 false。
 *
 * 模板只包含标题行，不放置示例密码，避免用户误把示例敏感数据导入生产环境。
 */
bool writeTemplate(const QString &filePath, QString *errorMessage = nullptr);

/**
 * @brief 从 .xlsx 文件读取首个工作表中的主机行。
 * @param filePath 待读取的 Excel 文件路径。
 * @param rows 成功解析后的行数据输出；失败时保持原内容不变。
 * @param errorMessage 文件、ZIP、XML、表头或数据校验失败时接收原因；可为空。
 * @return 至少解析出一行有效主机记录时返回 true；空表或格式错误返回 false。
 *
 * 支持 Excel 常见的共享字符串和内联字符串两种单元格编码，并忽略完全空白的尾部行。
 * 表头按英文列名匹配且大小写不敏感，要求包含 name、host、username、password 四列，
 * 这样模板可以直接使用，也能兼容用户在 Excel 中调整列顺序的情况。
 */
bool readWorkbook(const QString &filePath,
                  QList<HostRow> *rows,
                  QString *errorMessage = nullptr);

} // namespace ExcelHostCodec


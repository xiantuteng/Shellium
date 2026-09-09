#include "core/ExcelHostCodec.h"

#include <QFile>
#include <QHash>
#include <QXmlStreamReader>

#include <minizip/unzip.h>
#include <minizip/zip.h>

namespace {

/** @brief xlsx 模板固定使用的首工作表路径；读取时也作为常见工作簿的兼容回退路径。 */
constexpr const char *kWorksheetPath = "xl/worksheets/sheet1.xml";

/** @brief 生成工作簿关系文件中需要的最小内容。 */
const QByteArray kWorkbookRelationships = R"xml(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>)xml";

/** @brief 生成首工作表关系文件中需要的最小内容。 */
const QByteArray kWorkbookInnerRelationships = R"xml(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
</Relationships>)xml";

/** @brief 生成模板工作簿元数据。 */
const QByteArray kWorkbook = R"xml(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <sheets><sheet name="Hosts" sheetId="1" r:id="rId1"/></sheets>
</workbook>)xml";

/** @brief 生成模板内容类型声明，保证 Excel 和 LibreOffice 都能识别文件。 */
const QByteArray kContentTypes = R"xml(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
  <Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
</Types>)xml";

/** @brief 生成首工作表的 XML；标题使用英文，符合导入协议的稳定字段名。 */
const QByteArray kWorksheet = R"xml(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <sheetData>
    <row r="1">
      <c r="A1" t="inlineStr"><is><t>name</t></is></c>
      <c r="B1" t="inlineStr"><is><t>host</t></is></c>
      <c r="C1" t="inlineStr"><is><t>username</t></is></c>
      <c r="D1" t="inlineStr"><is><t>password</t></is></c>
    </row>
  </sheetData>
</worksheet>)xml";

/**
 * @brief 根据 Excel 单元格引用计算零基列号。
 * @param reference 例如 A1、BC7；行号部分会被忽略。
 * @return 列号；引用为空或列字母非法时返回 -1。
 */
int columnIndex(const QString &reference)
{
    int index = 0;
    bool foundLetter = false;
    for (const QChar character : reference) {
        if (!character.isLetter()) {
            break;
        }
        const QChar upper = character.toUpper();
        if (upper < QLatin1Char('A') || upper > QLatin1Char('Z')) {
            return -1;
        }
        foundLetter = true;
        index = index * 26 + (upper.unicode() - QLatin1Char('A').unicode() + 1);
    }
    return foundLetter ? index - 1 : -1;
}

/**
 * @brief 从 ZIP 工作簿中读取指定条目。
 * @param archivePath xlsx 文件路径。
 * @param entryPath ZIP 内部条目名，使用正斜杠。
 * @param content 条目解压后的内容输出。
 * @param errorMessage 打开、定位或解压失败时接收错误。
 * @return 条目成功读取返回 true；不存在或内容损坏返回 false。
 */
bool readZipEntry(const QString &archivePath,
                  const QByteArray &entryPath,
                  QByteArray *content,
                  QString *errorMessage)
{
    Q_ASSERT(content != nullptr);
    const QByteArray nativePath = QFile::encodeName(archivePath);
    unzFile archive = unzOpen64(nativePath.constData());
    if (archive == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("无法打开 Excel 压缩包");
        }
        return false;
    }

    const int locateResult = unzLocateFile(archive, entryPath.constData(), 0);
    if (locateResult != UNZ_OK || unzOpenCurrentFile(archive) != UNZ_OK) {
        unzClose(archive);
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Excel 文件缺少必要工作表数据：%1")
                                .arg(QString::fromUtf8(entryPath));
        }
        return false;
    }

    QByteArray result;
    char buffer[16 * 1024];
    int bytesRead = 0;
    while ((bytesRead = unzReadCurrentFile(archive, buffer, sizeof(buffer))) > 0) {
        result.append(buffer, bytesRead);
    }
    const bool readSucceeded = bytesRead == 0;
    const int closeResult = unzCloseCurrentFile(archive);
    unzClose(archive);
    if (!readSucceeded || closeResult != UNZ_OK) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Excel 工作表解压失败，文件可能已损坏");
        }
        return false;
    }
    *content = result;
    return true;
}

/**
 * @brief 读取 Excel 共享字符串表。
 * @param xml sharedStrings.xml 内容。
 * @param strings 输出按索引排列的字符串表。
 * @param errorMessage XML 解析错误输出。
 * @return XML 合法且成功读取返回 true。
 *
 * 富文本共享字符串可能由多个 <r><t> 片段组成；实现会拼接所有 t 节点，保留用户看到的
 * 文本而忽略字体等展示属性。普通字符串只包含一个 t 节点，同样走这条路径。
 */
bool parseSharedStrings(const QByteArray &xml,
                        QStringList *strings,
                        QString *errorMessage)
{
    Q_ASSERT(strings != nullptr);
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement() || reader.name() != QLatin1String("si")) {
            continue;
        }
        QString value;
        while (!(reader.isEndElement() && reader.name() == QLatin1String("si"))
               && !reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement() && reader.name() == QLatin1String("t")) {
                value += reader.readElementText(QXmlStreamReader::IncludeChildElements);
            }
        }
        strings->append(value);
    }
    if (reader.hasError()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Excel 共享字符串 XML 无效：%1").arg(reader.errorString());
        }
        return false;
    }
    return true;
}

/**
 * @brief 解析首工作表为按行、按列排列的文本矩阵。
 * @param xml worksheet XML 内容。
 * @param sharedStrings 共享字符串表，可为空。
 * @param matrix 解析结果输出。
 * @param errorMessage XML 解析错误输出。
 * @return XML 合法且读取成功返回 true。
 */
bool parseWorksheet(const QByteArray &xml,
                    const QStringList &sharedStrings,
                    QList<QStringList> *matrix,
                    QString *errorMessage)
{
    Q_ASSERT(matrix != nullptr);
    QXmlStreamReader reader(xml);
    QStringList currentRow;
    int currentColumn = 0;
    bool inRow = false;
    bool inCell = false;
    int cellColumn = -1;
    QString cellType;
    QString cellValue;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("row")) {
            inRow = true;
            currentRow.clear();
            currentColumn = 0;
        } else if (reader.isStartElement() && reader.name() == QLatin1String("c") && inRow) {
            inCell = true;
            cellColumn = columnIndex(reader.attributes().value(QLatin1String("r")).toString());
            if (cellColumn < 0) {
                cellColumn = currentColumn;
            }
            currentColumn = cellColumn + 1;
            cellType = reader.attributes().value(QLatin1String("t")).toString();
            cellValue.clear();
        } else if (reader.isStartElement()
                   && (reader.name() == QLatin1String("v")
                       || reader.name() == QLatin1String("t"))
                   && inCell) {
            cellValue += reader.readElementText(QXmlStreamReader::IncludeChildElements);
        } else if (reader.isEndElement() && reader.name() == QLatin1String("c") && inCell) {
            while (currentRow.size() <= cellColumn) {
                currentRow.append(QString());
            }
            QString decodedValue = cellValue;
            if (cellType == QLatin1String("s")) {
                bool ok = false;
                const int sharedIndex = cellValue.toInt(&ok);
                if (!ok || sharedIndex < 0 || sharedIndex >= sharedStrings.size()) {
                    if (errorMessage != nullptr) {
                        *errorMessage = QStringLiteral("Excel 文件引用了无效的共享字符串索引");
                    }
                    return false;
                }
                decodedValue = sharedStrings.at(sharedIndex);
            }
            currentRow[cellColumn] = decodedValue;
            inCell = false;
        } else if (reader.isEndElement() && reader.name() == QLatin1String("row") && inRow) {
            matrix->append(currentRow);
            inRow = false;
        }
    }
    if (reader.hasError()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Excel 工作表 XML 无效：%1").arg(reader.errorString());
        }
        return false;
    }
    return true;
}

/** @brief 对列名去掉空白并转小写，以兼容用户在 Excel 中修改大小写或误加空格。 */
QString normalizeHeader(const QString &header)
{
    return header.trimmed().toLower();
}

/**
 * @brief 向 ZIP 文件写入一个条目。
 * @param archive 已打开的 minizip 写入句柄。
 * @param path ZIP 内部路径。
 * @param content 条目内容。
 * @return 写入并关闭当前条目成功返回 true。
 */
bool writeZipEntry(zipFile archive, const char *path, const QByteArray &content)
{
    zip_fileinfo info{};
    if (zipOpenNewFileInZip64(archive,
                              path,
                              &info,
                              nullptr,
                              0,
                              nullptr,
                              0,
                              nullptr,
                              Z_DEFLATED,
                              Z_DEFAULT_COMPRESSION,
                              1)
        != ZIP_OK) {
        return false;
    }
    const bool written = zipWriteInFileInZip(archive, content.constData(), content.size()) == ZIP_OK;
    const bool closed = zipCloseFileInZip(archive) == ZIP_OK;
    return written && closed;
}

} // namespace

namespace ExcelHostCodec {

bool writeTemplate(const QString &filePath, QString *errorMessage)
{
    const QByteArray nativePath = QFile::encodeName(filePath);
    zipFile archive = zipOpen64(nativePath.constData(), APPEND_STATUS_CREATE);
    if (archive == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("无法创建 Excel 模板文件");
        }
        return false;
    }

    const bool success = writeZipEntry(archive, "[Content_Types].xml", kContentTypes)
        && writeZipEntry(archive, "_rels/.rels", kWorkbookRelationships)
        && writeZipEntry(archive, "xl/workbook.xml", kWorkbook)
        && writeZipEntry(archive, "xl/_rels/workbook.xml.rels", kWorkbookInnerRelationships)
        && writeZipEntry(archive, kWorksheetPath, kWorksheet);
    const int closeResult = zipClose(archive, nullptr);
    if (!success || closeResult != ZIP_OK) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("写入 Excel 模板失败");
        }
        return false;
    }
    return true;
}

bool readWorkbook(const QString &filePath, QList<HostRow> *rows, QString *errorMessage)
{
    Q_ASSERT(rows != nullptr);
    const QList<HostRow> originalRows = *rows;

    QByteArray worksheetXml;
    QString zipError;
    if (!readZipEntry(filePath,
                      QByteArray(kWorksheetPath),
                      &worksheetXml,
                      &zipError)) {
        if (errorMessage != nullptr) {
            *errorMessage = zipError;
        }
        return false;
    }

    QStringList sharedStrings;
    QByteArray sharedStringXml;
    // sharedStrings.xml 不是所有合法 xlsx 都有；缺失时使用空表，工作表仍可用 inlineStr。
    if (readZipEntry(filePath,
                     QByteArray("xl/sharedStrings.xml"),
                     &sharedStringXml,
                     nullptr)
        && !parseSharedStrings(sharedStringXml, &sharedStrings, errorMessage)) {
        *rows = originalRows;
        return false;
    }

    QList<QStringList> matrix;
    if (!parseWorksheet(worksheetXml, sharedStrings, &matrix, errorMessage)) {
        *rows = originalRows;
        return false;
    }
    if (matrix.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Excel 文件没有工作表数据");
        }
        return false;
    }

    QHash<QString, int> columns;
    const QStringList &header = matrix.first();
    for (int index = 0; index < header.size(); ++index) {
        const QString normalized = normalizeHeader(header.at(index));
        if (!normalized.isEmpty()) {
            columns.insert(normalized, index);
        }
    }
    const QStringList requiredHeaders{QStringLiteral("name"),
                                     QStringLiteral("host"),
                                     QStringLiteral("username"),
                                     QStringLiteral("password")};
    for (const QString &required : requiredHeaders) {
        if (!columns.contains(required)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Excel 表头缺少必需字段：%1").arg(required);
            }
            return false;
        }
    }

    QList<HostRow> parsedRows;
    for (int rowIndex = 1; rowIndex < matrix.size(); ++rowIndex) {
        const QStringList &values = matrix.at(rowIndex);
        const auto rawValueAt = [&values, &columns](const QString &column) {
            const int index = columns.value(column, -1);
            return index >= 0 && index < values.size() ? values.at(index) : QString();
        };
        // 名称、地址和账号的外围空格通常来自 Excel 人工录入，应主动去除；密码则必须保留
        // 原始内容，因为首尾空格可能本来就是用户设置的一部分，静默 trim 会导致导入后
        // 认证失败且无法从界面判断密码已经被改变。
        HostRow row{rawValueAt(QStringLiteral("name")).trimmed(),
                    rawValueAt(QStringLiteral("host")).trimmed(),
                    rawValueAt(QStringLiteral("username")).trimmed(),
                    rawValueAt(QStringLiteral("password"))};
        if (row.name.isEmpty() && row.host.isEmpty() && row.username.isEmpty() && row.password.isEmpty()) {
            continue;
        }
        if (row.name.isEmpty() || row.host.isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Excel 第 %1 行的 name 和 host 不能为空").arg(rowIndex + 1);
            }
            return false;
        }
        parsedRows.append(row);
    }

    if (parsedRows.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Excel 文件中没有可导入的主机记录");
        }
        return false;
    }
    *rows = parsedRows;
    return true;
}

} // namespace ExcelHostCodec

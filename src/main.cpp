#include "app/MainWindow.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"
#include "ui/VaultDialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QMessageBox>
#include <QStyleFactory>
#include <QUuid>

namespace {

/**
 * @brief 向隔离的 UI 预览数据库写入一条主机或分组记录。
 * @param database 已初始化且必须指向内存的预览数据库。
 * @param record 仅用于视觉覆盖的非敏感示例记录。
 * @param errorMessage 写入失败时接收数据库错误；可为空。
 * @return 保存成功返回 true；失败返回 false，由调用方终止预览启动并显示原因。
 *
 * 单独封装保存动作的原因是预览数据包含多个层级：任何一条写入失败都应立即停止，不能继续
 * 展示一棵半完整的树，否则截图会把数据库故障误报成布局回归。该函数仅在明确开启
 * SHELLIUM_UI_PREVIEW 时调用，正常启动路径永远不会创建这些记录。
 */
bool saveUiPreviewRecord(ApplicationDatabase *database,
                         const HostRecord &record,
                         QString *errorMessage)
{
    Q_ASSERT(database != nullptr);
    return database->saveHost(record, errorMessage);
}

/**
 * @brief 构造覆盖主机页关键视觉状态的纯内存示例树。
 * @param database 已初始化的内存数据库；不得指向用户真实数据库。
 * @param errorMessage 任一记录保存失败时接收可读原因；可为空。
 * @return 七台主机及其分组全部写入成功返回 true。
 *
 * 数据刻意覆盖展开分组、空分组、继承账号/端口、环境标签与未分组主机。这样 UI 预览截图
 * 可以同时发现箭头、引导线、双行卡片、标签徽章、数量摘要和分区标题的回归，而不是只验证
 * 一个空白侧栏。地址均为文档/内网示例值，记录只存在本进程内存中，不包含任何凭据。
 */
bool seedHostUiPreview(ApplicationDatabase *database, QString *errorMessage)
{
    Q_ASSERT(database != nullptr);

    HostRecord wuhanGroup;
    wuhanGroup.id = QStringLiteral("preview-group-wuhan");
    wuhanGroup.kind = HostNodeKind::Folder;
    wuhanGroup.name = QStringLiteral("武汉");
    wuhanGroup.username = QStringLiteral("root");
    wuhanGroup.port = 22;
    if (!saveUiPreviewRecord(database, wuhanGroup, errorMessage)) {
        return false;
    }

    HostRecord preRelease;
    preRelease.id = QStringLiteral("preview-host-pre-release");
    preRelease.parentId = wuhanGroup.id;
    preRelease.kind = HostNodeKind::Host;
    preRelease.name = QStringLiteral("预发布");
    preRelease.host = QStringLiteral("10.194.135.11");
    if (!saveUiPreviewRecord(database, preRelease, errorMessage)) {
        return false;
    }

    HostRecord numberedHost = preRelease;
    numberedHost.id = QStringLiteral("preview-host-numbered");
    numberedHost.name = QStringLiteral("1");
    numberedHost.sortOrder = 1;
    if (!saveUiPreviewRecord(database, numberedHost, errorMessage)) {
        return false;
    }

    HostRecord shangraoGroup;
    shangraoGroup.id = QStringLiteral("preview-group-shangrao");
    shangraoGroup.kind = HostNodeKind::Folder;
    shangraoGroup.name = QStringLiteral("上饶E呼即办");
    shangraoGroup.username = QStringLiteral("root");
    shangraoGroup.port = 3822;
    shangraoGroup.tags = {QStringLiteral("测试")};
    shangraoGroup.sortOrder = 1;
    if (!saveUiPreviewRecord(database, shangraoGroup, errorMessage)) {
        return false;
    }

    HostRecord middleware;
    middleware.id = QStringLiteral("preview-host-middleware");
    middleware.parentId = shangraoGroup.id;
    middleware.kind = HostNodeKind::Host;
    middleware.name = QStringLiteral("192.168.1.79-中间件");
    middleware.host = QStringLiteral("115.159.35.31");
    if (!saveUiPreviewRecord(database, middleware, errorMessage)) {
        return false;
    }

    HostRecord emptyGroup;
    emptyGroup.id = QStringLiteral("preview-group-empty");
    emptyGroup.kind = HostNodeKind::Folder;
    emptyGroup.name = QStringLiteral("江夏镇");
    emptyGroup.sortOrder = 2;
    if (!saveUiPreviewRecord(database, emptyGroup, errorMessage)) {
        return false;
    }

    HostRecord dongxihuGroup;
    dongxihuGroup.id = QStringLiteral("preview-group-dongxihu");
    dongxihuGroup.kind = HostNodeKind::Folder;
    dongxihuGroup.name = QStringLiteral("东西湖城运");
    dongxihuGroup.username = QStringLiteral("geoadmin");
    dongxihuGroup.sortOrder = 3;
    if (!saveUiPreviewRecord(database, dongxihuGroup, errorMessage)) {
        return false;
    }

    HostRecord cityHost;
    cityHost.id = QStringLiteral("preview-host-city-50");
    cityHost.parentId = dongxihuGroup.id;
    cityHost.kind = HostNodeKind::Host;
    cityHost.name = QStringLiteral("10.70.165.50");
    cityHost.host = QStringLiteral("10.70.165.50");
    cityHost.port = 22022;
    if (!saveUiPreviewRecord(database, cityHost, errorMessage)) {
        return false;
    }

    HostRecord secondCityHost = cityHost;
    secondCityHost.id = QStringLiteral("preview-host-city-52");
    secondCityHost.name = QStringLiteral("10.70.165.52");
    secondCityHost.host = QStringLiteral("10.70.165.52");
    secondCityHost.port = 10022;
    secondCityHost.sortOrder = 1;
    if (!saveUiPreviewRecord(database, secondCityHost, errorMessage)) {
        return false;
    }

    HostRecord ungroupedHost;
    ungroupedHost.id = QStringLiteral("preview-host-ungrouped");
    ungroupedHost.kind = HostNodeKind::Host;
    ungroupedHost.name = QStringLiteral("10.38.217.99");
    ungroupedHost.host = QStringLiteral("10.38.217.99");
    ungroupedHost.username = QStringLiteral("root");
    ungroupedHost.port = 8000;
    ungroupedHost.sortOrder = 4;
    if (!saveUiPreviewRecord(database, ungroupedHost, errorMessage)) {
        return false;
    }

    HostRecord finalPreviewHost = ungroupedHost;
    finalPreviewHost.id = QStringLiteral("preview-host-secondary");
    finalPreviewHost.name = QStringLiteral("GPU 运算节点");
    finalPreviewHost.host = QStringLiteral("192.0.2.80");
    finalPreviewHost.port = 22;
    finalPreviewHost.sortOrder = 5;
    return saveUiPreviewRecord(database, finalPreviewHost, errorMessage);
}

} // namespace

/**
 * @brief Shellium 图形应用入口。
 * @param argc 系统传入的命令行参数数量。
 * @param argv 系统传入的命令行参数数组。
 * @return Qt 事件循环的退出码。
 */
int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Shellium"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.7"));
    QCoreApplication::setOrganizationName(QStringLiteral("Shellium"));

    // Qt 资源中的 PNG 使用与 macOS Bundle 相同的圆角方形设计，并保留透明外围安全区；它会
    // 被后续创建的 MainWindow、保险库对话框和错误消息框继承。运行时采用 512px 版本，既能
    // 覆盖高 DPI 窗口图标，又避免长期持有 1024px RGBA 缓冲；Finder 与 Dock 则继续读取包含
    // 多分辨率图层的原生 .icns，两条路径在不同展示尺寸下保持同一视觉比例。
    application.setWindowIcon(
        QIcon(QStringLiteral(":/resources/icons/shellium-app-icon-macos-512.png")));

    // UI 预览模式只供开发和自动化视觉验收使用。最重要的安全边界是：一旦开启就强制
    // 使用内存数据库，后续即使初始化逻辑发生变化，也不能读取或覆盖用户真实主机、凭据
    // 与工作区。环境变量比命令行参数更适合 macOS LaunchServices 启动的临时测试 Bundle。
    const bool uiPreviewMode = qEnvironmentVariableIsSet("SHELLIUM_UI_PREVIEW");

    // Fusion 为 macOS/Linux 提供一致的表单尺寸和基础交互；窗口颜色、圆角、导航与终端
    // 配色再由设置服务和 MainWindow 视觉系统覆盖，避免平台原生样式造成布局尺寸漂移。
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    ApplicationDatabase database(uiPreviewMode ? ApplicationDatabase::inMemoryPath()
                                               : QString{});
    QString databaseError;
    if (!database.initialize(&databaseError)) {
        QMessageBox::critical(nullptr,
                              QObject::tr("数据库初始化失败"),
                              QObject::tr("无法初始化 Shellium 本地数据：%1").arg(databaseError));
        return 1;
    }

    if (uiPreviewMode) {
        QString previewSeedError;
        if (!seedHostUiPreview(&database, &previewSeedError)) {
            QMessageBox::critical(nullptr,
                                  QObject::tr("UI 预览初始化失败"),
                                  QObject::tr("无法创建隔离的主机页预览数据：%1")
                                      .arg(previewSeedError));
            return 1;
        }

        // 可选主题仅写入本次内存数据库，用于持续集成分别截图浅色/深色界面。未知值回退为
        // 跟随系统，不让测试辅助参数影响正常启动或制造无效持久化设置。
        const QString requestedPreviewTheme =
            qEnvironmentVariable("SHELLIUM_UI_PREVIEW_THEME").trimmed().toLower();
        if (requestedPreviewTheme == QLatin1String("light")
            || requestedPreviewTheme == QLatin1String("dark")) {
            QString previewThemeError;
            if (!database.setSetting(QStringLiteral("appearance/theme"),
                                     requestedPreviewTheme,
                                     &previewThemeError)) {
                QMessageBox::critical(nullptr,
                                      QObject::tr("UI 预览初始化失败"),
                                      QObject::tr("无法写入隔离的预览主题：%1")
                                          .arg(previewThemeError));
                return 1;
            }
        }
    }

    CredentialVault credentialVault(database);
    if (uiPreviewMode) {
        // 一次性随机字符串仅用于让内存保险库进入“已解锁”状态，不是默认主密码，也不会
        // 写入磁盘。采用随机值而非代码常量可避免维护者误把预览路径复制到生产解锁逻辑。
        QString previewVaultError;
        const QString transientPassword = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!credentialVault.initialize(transientPassword, &previewVaultError)) {
            QMessageBox::critical(nullptr,
                                  QObject::tr("UI 预览初始化失败"),
                                  QObject::tr("无法初始化隔离的内存保险库：%1")
                                      .arg(previewVaultError));
            return 1;
        }
    } else {
        QString vaultSettingError;
        const std::optional<QString> configured =
            database.setting(QStringLiteral("security/vault_password_enabled"),
                             &vaultSettingError);
        if (!vaultSettingError.isEmpty()) {
            QMessageBox::critical(nullptr,
                                  QObject::tr("读取设置失败"),
                                  vaultSettingError);
            return 1;
        }
        // 旧版本没有开关键，但已经存在 vault_metadata 时代表用户明确创建过主密码，升级后
        // 必须继续要求解锁；全新安装则默认关闭，让用户进入设置后再自主决定是否启用。
        bool legacyVaultExists = false;
        if (!configured.has_value()) {
            QString configurationError;
            legacyVaultExists =
                database.vaultConfiguration(&configurationError).has_value();
            if (!configurationError.isEmpty()) {
                QMessageBox::critical(nullptr,
                                      QObject::tr("保险箱配置损坏"),
                                      configurationError);
                return 1;
            }
        }
        const bool passwordProtectionEnabled = configured.has_value()
            ? (*configured == QLatin1String("1")
               || configured->compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
            : legacyVaultExists;
        credentialVault.setPasswordProtectionEnabled(passwordProtectionEnabled);
        if (passwordProtectionEnabled) {
            // 忘记密码或暂时不想解锁时仍允许进入应用管理非敏感主机配置。取消只保持保险箱
            // 锁定；“忘记密码”重置成功则会把安全设置原子写为关闭，MainWindow 随后从数据库
            // 读取真实状态。需要旧凭据的连接仍会在使用时再次要求解锁，不会绕过认证。
            VaultDialog::ensureUnlocked(&credentialVault, nullptr, true);
        }
    }

    // 数据库与保险库服务都由 main() 持有，生命周期覆盖 MainWindow。显式注入保证主机树、
    // 最近连接和凭据解密共享同一份已迁移数据与同一把会话密钥。
    MainWindow window(&database, &credentialVault);
    if (uiPreviewMode) {
        window.setWindowTitle(QObject::tr("Shellium · UI Preview"));
    }
    window.show();
    return application.exec();
}

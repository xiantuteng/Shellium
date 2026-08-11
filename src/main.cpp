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
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.2"));
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
        // 正常模式首次启动必须创建主密码，后续启动必须成功解锁。这里不允许取消后继续进入
        // 一个看似可用但无法读取保存凭据的半初始化界面；关闭窗口时直接结束且不改数据库。
        if (!VaultDialog::ensureUnlocked(&credentialVault, nullptr, false)) {
            return 1;
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

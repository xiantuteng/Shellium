#include "app/MainWindow.h"
#include "security/Vault.h"
#include "storage/ApplicationDatabase.h"
#include "ui/VaultDialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QStyleFactory>
#include <QMessageBox>

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
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(QStringLiteral("Shellium"));

    // Fusion 在 macOS 和 Linux 上具有一致的控件尺寸与 Dock 行为，便于当前跨平台 MVP 验证。
    // 后续主题引擎上线后，将由用户设置覆盖该默认样式。
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    ApplicationDatabase database;
    QString databaseError;
    if (!database.initialize(&databaseError)) {
        QMessageBox::critical(nullptr,
                              QObject::tr("数据库初始化失败"),
                              QObject::tr("无法初始化 Shellium 本地数据：%1").arg(databaseError));
        return 1;
    }

    CredentialVault credentialVault(database);
    // 首次启动必须创建主密码，后续启动必须成功解锁。这里不允许取消后继续进入一个看似可用
    // 但无法读取保存凭据的半初始化界面；用户关闭窗口时直接结束进程，数据库不会被修改。
    if (!VaultDialog::ensureUnlocked(&credentialVault, nullptr, false)) {
        return 1;
    }

    // 数据库与保险库服务都由 main() 持有，生命周期覆盖 MainWindow。显式注入保证主机树、
    // 最近连接和凭据解密共享同一份已迁移数据与同一把会话密钥。
    MainWindow window(&database, &credentialVault);
    window.show();
    return application.exec();
}

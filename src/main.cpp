#include "app/MainWindow.h"
#include "storage/ApplicationDatabase.h"

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

    MainWindow window;
    window.show();
    return application.exec();
}

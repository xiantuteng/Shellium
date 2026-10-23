#include "update/UpdatePrompt.h"

#include "settings/TerminalSettings.h"

#include <QApplication>
#include <QDesktopServices>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>

namespace {

bool isSameOrDescendant(const QWidget *widget, const QWidget *ancestor)
{
    for (const QWidget *current = widget; current != nullptr; current = current->parentWidget()) {
        if (current == ancestor) {
            return true;
        }
    }
    return false;
}

// 后台检查命中时，用户可能正开着 exec() 的应用级模态框（例如设置对话框）。macOS 上
// open() 的 QMessageBox 是父窗口的 sheet，而 exec() 的 Cocoa 模态会话只放行模态框自己
// 和它的 sheet：挂在主窗口上的提示框收不到点击，模态框又被提示框按窗口级模态挡住，
// 整个程序卡死。所以父窗口被模态框挡住时改挂到模态框上。
QWidget *unblockedParent(QWidget *parent)
{
    QWidget *modal = QApplication::activeModalWidget();
    if (modal == nullptr || parent == nullptr || isSameOrDescendant(parent, modal)) {
        return parent;
    }
    return modal;
}

} // namespace

QMessageBox *UpdatePrompt::show(QWidget *parent,
                                const UpdateRelease &release,
                                const QString &currentVersion,
                                TerminalSettingsService *settingsService)
{
    auto *box = new QMessageBox(unblockedParent(parent));
    box->setObjectName(QStringLiteral("updatePrompt"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setIcon(QMessageBox::Information);
    box->setWindowTitle(tr("Update available"));
    box->setText(tr("Shellium %1 is available.").arg(release.version));
    QString details = tr("You are using version %1.").arg(currentVersion);
    if (release.publishedAt.isValid()) {
        details += QLatin1Char('\n')
            + tr("Released on %1.")
                  .arg(QLocale().toString(release.publishedAt.toLocalTime().date(),
                                          QLocale::LongFormat));
    }
    box->setInformativeText(details);

    QPushButton *downloadButton = box->addButton(tr("Download"), QMessageBox::AcceptRole);
    downloadButton->setObjectName(QStringLiteral("updatePromptDownload"));
    QPushButton *skipButton = nullptr;
    if (settingsService != nullptr) {
        skipButton = box->addButton(tr("Skip This Version"), QMessageBox::DestructiveRole);
        skipButton->setObjectName(QStringLiteral("updatePromptSkip"));
    }
    QPushButton *laterButton = box->addButton(tr("Remind Me Later"), QMessageBox::RejectRole);
    laterButton->setObjectName(QStringLiteral("updatePromptLater"));
    box->setDefaultButton(downloadButton);
    box->setEscapeButton(laterButton);

    const QString version = release.version;
    QObject::connect(box, &QMessageBox::buttonClicked, box,
                     [box, downloadButton, skipButton, settingsService, version](
                         QAbstractButton *clicked) {
                         if (clicked == downloadButton) {
                             QDesktopServices::openUrl(
                                 QUrl(QString::fromLatin1(UpdateChecker::kDownloadPage)));
                         } else if (clicked != nullptr && clicked == skipButton) {
                             QString error;
                             if (!skipVersion(settingsService, version, &error)) {
                                 QMessageBox::warning(box->parentWidget(),
                                                      tr("Could not save settings"), error);
                             }
                         }
                     });
    box->open();
    return box;
}

bool UpdatePrompt::skipVersion(TerminalSettingsService *settingsService,
                               const QString &version,
                               QString *errorMessage)
{
    if (settingsService == nullptr) {
        return false;
    }
    TerminalSettings settings = settingsService->currentSettings();
    settings.skippedUpdateVersion = version;
    return settingsService->save(settings, errorMessage);
}

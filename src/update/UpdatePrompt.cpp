#include "update/UpdatePrompt.h"

#include "settings/TerminalSettings.h"

#include <QDesktopServices>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>

QMessageBox *UpdatePrompt::show(QWidget *parent,
                                const UpdateRelease &release,
                                const QString &currentVersion,
                                TerminalSettingsService *settingsService)
{
    auto *box = new QMessageBox(parent);
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

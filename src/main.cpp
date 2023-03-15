#include <QApplication>
#include <QDialog>
#include <QSystemTrayIcon>
#include <QAction>
#include <QMenu>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QSettings>
#include <QSemaphore>

#include <QDebug>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <cstdlib>
#include <iostream>

#include <libportal-qt5/portal-qt5.h>

#include <SingleApplication>
#include "SettingsDialog.h"

#define VERSION "v0.5.4"
#define APP_NAME "SyncThingy"

class SyncThingy : public QDialog {

public:
    explicit SyncThingy(QSettings& settings) : settings(settings) {
        initSettings();
        workaroundFuckingStupidGTKbug();
        setupUi();
        checkRunning();
        requestBackgroundPermission();
    }

    bool syncthingProcessRunning() {
        return syncthingProcess != nullptr && syncthingProcess->state() == QProcess::Running;
    }

//public slots:
    void stopProcess() {
        qDebug() << "quit triggered \n";

        if (syncthingProcessRunning()) {
            syncthingProcess->terminate();
            syncthingProcess->waitForFinished();
            auto exitcode = syncthingProcess->exitCode();

            if (exitcode == 0) {
                const auto msg = QString("exit code: ").append(exitcode);
                _showMessage("Syncthing stopped", msg, trayIcon->icon(), 5000);
            } else {
                const auto msg = QString("Syncthing failed to start! exit code (").append(exitcode).append(")");
                trayIcon->showMessage("ERROR", msg, QSystemTrayIcon::Critical, 0);
            }
        }
    };

    static void secondaryStarted() {
        qDebug() << "Secondary SyncThingy instance started!!";
        //_showMessage("INFO", "SEC STARTED", trayIcon->icon(), 0);
        //trayIcon->show();
    }

private:
    QSettings& settings;
    QTimer* timer;
    QProcess* syncthingProcess = nullptr;

    QSemaphore* semaphore = new QSemaphore(1);
    QSystemTrayIcon* trayIcon = new QSystemTrayIcon(this);

    QNetworkAccessManager networkManager;

    void workaroundFuckingStupidGTKbug() {
        // Yes I know this ridiculously stupid, but I have to work around a stupid GTK / GNOME bug
        // see https://github.com/zocker-160/SyncThingy/issues/8
        resize(0, 0);
    }

    void setupUi() {
        updateIcon();

        auto openGitHubAction = new QAction(QString(APP_NAME).append(" ").append(VERSION), this);
        auto showBrowserAction = new QAction("Open WebUI", this);
        auto openConfigAction = new QAction("Open Config", this);
        auto openSettingsAction = new QAction("Settings...", this);
        auto exitAction = new QAction("Exit", this);
        auto menu = new QMenu();

        openGitHubAction->setIcon(QIcon::fromTheme("help-about"));
        showBrowserAction->setIcon(QIcon::fromTheme("help-browser"));
        openConfigAction->setIcon(QIcon::fromTheme("text"));
        openSettingsAction->setIcon(QIcon::fromTheme("preferences-desktop-personal"));
        exitAction->setIcon(QIcon::fromTheme("application-exit"));

        connect(openGitHubAction, &QAction::triggered, this, &SyncThingy::showGitHub);
        connect(showBrowserAction, &QAction::triggered, this, &SyncThingy::showBrowser);
        connect(openSettingsAction, &QAction::triggered, this, &SyncThingy::showSettingsDialog);
        connect(openConfigAction, &QAction::triggered, this, &SyncThingy::openConfig);
        connect(exitAction, &QAction::triggered, QApplication::instance(), &QApplication::quit);
        connect(trayIcon, &QSystemTrayIcon::activated, this, &SyncThingy::handleActivation);

        menu->addAction(openGitHubAction);
        menu->addSeparator();
        menu->addSeparator();
        menu->addAction(showBrowserAction);
        menu->addAction(openSettingsAction);

        //menu->addSeparator();
        //menu->addAction(openConfigAction);

        menu->addSeparator();
        menu->addAction(exitAction);

        trayIcon->setContextMenu(menu);
        trayIcon->show();
    }

    QNetworkReply* _requestSyncthingHealth() {
        const auto pingEndpoint = QString(settings.value(C_URL).toString()).append("/rest/noauth/health");
        QNetworkRequest request(pingEndpoint);
        request.setRawHeader("Content-Type", "application/json; charset=utf-8");
        request.setRawHeader("Accept", "application/json");

        return networkManager.get(request);
    }

    static bool _isSyncthingRunning(QNetworkReply* reply) {
        auto jsonDoc = QJsonDocument::fromJson(reply->readAll());
        auto status = jsonDoc.object().value("status");

        if (not status.isUndefined() && status.toString() == "OK")
            return true;
        else
            return false;
    }

    void checkRunning() {
        auto reply = _requestSyncthingHealth();
        connect(reply, &QNetworkReply::finished, [=]{
            if (_isSyncthingRunning(reply)) {
                qDebug() << "Syncthing instance seems to be already running, Tray Icon only";
            } else {
                qDebug() << "Syncthing does not seem to be running, starting own instance";
                setupProcess();
            }
            setupTimer();
            reply->deleteLater();
        });
    }

    void setupProcess() {
        if (not checkSyncthingAvailable()) {
            const char* msg = "Syncthing could not be found in PATH!";

            std::cout << "ERROR: "<< msg << std::endl;
            trayIcon->showMessage("ERROR", msg, QSystemTrayIcon::Critical, 0);

            return;
        }

        QStringList arguments;
        arguments << "serve" << "--no-browser" << "--logfile=default";

        syncthingProcess = new QProcess(this);
        syncthingProcess->start("syncthing", arguments);
        syncthingProcess->waitForStarted();

        _showMessage("Syncthing started", "", trayIcon->icon(), 3000);
    }

    void setupTimer() {
        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &SyncThingy::checkSyncthingRunning);
        timer->start(5000);
    }

    void updateIcon() {
        const char* flatpakID = std::getenv("FLATPAK_ID");
        QString prefix;

        if (flatpakID == nullptr) {
            prefix = "syncthing";
        } else {
            qDebug() << "running inside Flatpak \n";
            prefix = flatpakID;
        }

        QString iconType = settings.value(C_ICON).toString();
        if (iconType == C_ICON_WHITE or iconType == C_ICON_BLACK)
            iconType = prefix + "." + iconType;
        else
            iconType = prefix;

        qDebug() << "Using Icon:" << iconType;
        trayIcon->setIcon(QIcon::fromTheme(iconType));
    }

    void initSettings() {
        if (not settings.contains(C_URL)) {
            settings.setValue(C_URL, "http://127.0.0.1:8384");
            settings.setValue(C_ICON, "default");
            settings.sync();
        }
        // new setting in 0.4 which needs to be true by default
        if (not settings.contains(C_AUTOSTART)) {
            settings.setValue(C_AUTOSTART, true);
            settings.sync();
        }
        // new setting in 0.5 which needs to be true by default
        if (not settings.contains(C_NOTIFICATION)) {
            settings.setValue(C_NOTIFICATION, true);
            settings.sync();
        }
    }

    void requestBackgroundPermission() {
        qDebug() << "Requesting background permission...";

        auto commandline = g_ptr_array_new();
        g_ptr_array_add(commandline, (gpointer) "SyncThingy");

        char reason[] = "Reason: Ability to sync data in the background.";
        auto flag = settings.value(C_AUTOSTART).toBool() ? XDP_BACKGROUND_FLAG_AUTOSTART : XDP_BACKGROUND_FLAG_NONE;

        xdp_portal_request_background(
            XdpQt::globalPortalObject(),
            nullptr,
            reason,
            commandline,
            flag,
            nullptr,
            SyncThingy::backgroundRequestCallback,
            this
        );
    }

    static void backgroundRequestCallback(GObject* object, GAsyncResult* result, void* data) {
        GError* error = nullptr;
        auto ret = xdp_portal_request_background_finish(
                XdpQt::globalPortalObject(), result, &error);

        auto tray = static_cast<SyncThingy*>(data);

        if (ret)
            qDebug() << "Background / Autostart permission granted";
        else {
            qDebug() << "Background / Autostart permission revoked";
            tray->trayIcon->showMessage(
                "Background permission revoked",
                "SyncThingy might not work as expected!",
                tray->trayIcon->icon(),
                0
            );
        }
    }

    static bool checkSyncthingAvailable() {
        int ret = system("which syncthing");
        return ret == 0;
    }

//private slots:
    void showBrowser() {
        qDebug() << "opening Syncthing webui using xdg-open";
        system(QString("xdg-open ").append(settings.value(C_URL).toString()).toStdString().c_str());
    };

    void openConfig() {
        qDebug() << "opening config file using xdg-open";
        system(QString("xdg-open ").append(settings.fileName()).toStdString().c_str());
    }

    static void showGitHub() {
        qDebug() << "opening GitHub page using xdg-open";
        system("xdg-open https://github.com/zocker-160/SyncThingy");
    }

    void handleActivation(QSystemTrayIcon::ActivationReason reason) {
        switch (reason) {
            case QSystemTrayIcon::Trigger:
                showBrowser();
        }
    }

    void showSettingsDialog() {
        if (semaphore->tryAcquire()) {
            show(); // see workaroundFuckingStupidGTKbug()

            qDebug() << "open settings";

            SettingsDialog options(settings, trayIcon->icon(), this);
            if (options.exec() == QDialog::Accepted) {
                updateIcon();
                requestBackgroundPermission();
            }

            hide(); // see workaroundFuckingStupidGTKbug()
            semaphore->release();
        } else {
            qDebug() << "settings dialog is already open";
        }
    }

    void checkSyncthingRunning() {
        //qDebug() << "run check";

        if (not syncthingProcessRunning()) {
            auto reply = _requestSyncthingHealth();
            connect(reply, &QNetworkReply::finished, [=] {
                if (not _isSyncthingRunning(reply))
                    QApplication::quit();
                else
                    reply->deleteLater();
            });
        }
    }

    void _showMessage(const QString& title, const QString& msg, const QIcon& icon, int msecs = 10000) {
        if (settings.value(C_NOTIFICATION).toBool())
            trayIcon->showMessage(title, msg, icon, msecs);
    }
};

int main(int argc, char *argv[]) {
    SingleApplication app(argc, argv, true);
    SingleApplication::setApplicationName(APP_NAME);
    SingleApplication::setApplicationVersion(VERSION);

    if (app.isSecondary()) {
        qDebug() << "this instance is secondary, exiting with 4";
        return 4;
    }

    QSettings settings(APP_NAME, "settings");
    SyncThingy sth(settings);

    QObject::connect(&app, &SingleApplication::aboutToQuit, &sth, &SyncThingy::stopProcess);
    QObject::connect(&app, &SingleApplication::instanceStarted, &sth, &SyncThingy::secondaryStarted);

    return SingleApplication::exec();
}

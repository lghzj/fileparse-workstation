#pragma once

#include "app/UploadManager.h"
#include "app/RuntimeConfig.h"
#include "api/ApiClient.h"
#include "api/WebSocketClient.h"
#include "storage/ConfigStore.h"
#include "storage/LocalDatabase.h"
#include "system/WatchManager.h"

#include <QLineEdit>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QSystemTrayIcon>
#include <QSpinBox>

class QCloseEvent;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void applyLegacyStyle();
    QWidget *buildStatusTab();
    QWidget *buildConnectionTab();
    QWidget *buildLogsTab();
    QWidget *buildDiagnosticsTab();
    QWidget *wrapPage(QWidget *content);
    void configureTable(QTableWidget *table) const;
    QPushButton *buildNavButton(const QString &text, int pageIndex);
    void setCurrentPage(int pageIndex);
    void setupTray();
    void loadSettings();
    WorkstationSettings currentSettings() const;
    bool validateSettings(bool requireToken) const;
    void applySettingsToClients();
    void appendLog(const QString &message);
    void toggleWorkstation();
    void stopWorkstation();
    void setStatusValue(QLabel *label, const QString &text, const QString &color);
    void updateStatusCards();
    void refreshDeviceTable();
    QJsonObject runDiagnostics(bool checkNetwork, bool showMessage);
    void renderDiagnostics(const QJsonObject &result);
    void chooseAndUploadFile();
    void applyConfigPayload(const QJsonObject &payload);
    void uploadRequest(const UploadRequest &request, bool manual = false);
    void refreshUploadTable();
    void retrySelectedUpload();
    void clearFailedUploads();
    void exportDiagnostics();
    void openLogDirectory();
    void quitApplication();
    static QString translatedStatus(const QString &status);
    static QString formattedBytes(qint64 bytes);
    static QJsonObject sanitizedPayload(QJsonObject payload);

    ConfigStore configStore_;
    LocalDatabase database_;
    ApiClient apiClient_;
    UploadManager uploadManager_;
    WebSocketClient webSocketClient_;
    WatchManager watchManager_;
    RuntimeConfig runtimeConfig_;
    bool allowQuit_ = false;
    bool workstationRunning_ = false;

    QLineEdit *baseUrlEdit_ = nullptr;
    QLineEdit *ipEdit_ = nullptr;
    QLineEdit *macEdit_ = nullptr;
    QLineEdit *tokenEdit_ = nullptr;
    QLineEdit *hostnameEdit_ = nullptr;
    QSpinBox *deviceIdSpin_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QPushButton *registerButton_ = nullptr;
    QPushButton *pullConfigButton_ = nullptr;
    QPushButton *connectWsButton_ = nullptr;
    QPushButton *startWatchButton_ = nullptr;
    QPushButton *uploadButton_ = nullptr;
    QPushButton *refreshUploadsButton_ = nullptr;
    QPushButton *retrySelectedButton_ = nullptr;
    QPushButton *clearFailedButton_ = nullptr;
    QPushButton *exportDiagnosticsButton_ = nullptr;
    QPushButton *openLogDirectoryButton_ = nullptr;
    QPushButton *primaryActionButton_ = nullptr;
    QLabel *runtimeValueLabel_ = nullptr;
    QLabel *runtimeHintLabel_ = nullptr;
    QLabel *deviceValueLabel_ = nullptr;
    QLabel *deviceHintLabel_ = nullptr;
    QLabel *failedValueLabel_ = nullptr;
    QLabel *failedHintLabel_ = nullptr;
    QLabel *configVersionLabel_ = nullptr;
    QLabel *logPathLabel_ = nullptr;
    QLabel *diagnosticsStatusLabel_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QVector<QPushButton *> navButtons_;
    QTableWidget *uploadTable_ = nullptr;
    QTableWidget *deviceTable_ = nullptr;
    QTableWidget *diagnosticsTable_ = nullptr;
    QPlainTextEdit *logEdit_ = nullptr;
    QSystemTrayIcon *trayIcon_ = nullptr;
    QMenu *trayMenu_ = nullptr;
};

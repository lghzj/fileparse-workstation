#include "MainWindow.h"

#include "system/FileHasher.h"
#include "system/DiagnosticExporter.h"
#include "system/LogManager.h"
#include "system/SystemDiagnostics.h"

#include <algorithm>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QApplication>
#include <QCloseEvent>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QHeaderView>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStyle>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), uploadManager_(&apiClient_, &database_, this) {
    buildUi();
    setupTray();
    loadSettings();

    QString dbError;
    bool databaseReady = false;
    if (!database_.open(&dbError)) {
        appendLog("SQLite init failed: " + dbError);
    } else {
        databaseReady = true;
        const int recovered = database_.recoverInterruptedUploads(&dbError);
        if (recovered > 0) {
            appendLog(QString("recovered interrupted uploads: %1").arg(recovered));
        } else if (recovered < 0) {
            appendLog("recover uploads failed: " + dbError);
        }
        refreshUploadTable();
    }

    uploadManager_.setUploadsEnabled(!currentSettings().token.trimmed().isEmpty());
    if (databaseReady) {
        uploadManager_.startRetryTimer();
    }

    connect(saveButton_, &QPushButton::clicked, this, [this]() {
        if (!validateSettings(false)) {
            return;
        }
        configStore_.save(currentSettings());
        applySettingsToClients();
        appendLog("settings saved");
        appendLog("registering workstation after save...");
        saveButton_->setEnabled(false);
        registerButton_->setEnabled(false);
        apiClient_.registerWorkstation();
    });

    connect(registerButton_, &QPushButton::clicked, this, [this]() {
        if (!validateSettings(false)) {
            return;
        }
        configStore_.save(currentSettings());
        applySettingsToClients();
        appendLog("registering workstation...");
        registerButton_->setEnabled(false);
        apiClient_.registerWorkstation();
    });

    connect(pullConfigButton_, &QPushButton::clicked, this, [this]() {
        if (!validateSettings(false)) {
            return;
        }
        configStore_.save(currentSettings());
        applySettingsToClients();
        if (currentSettings().token.trimmed().isEmpty()) {
            loadSettings();
            refreshUploadTable();
            appendLog("配置已重新载入");
            QMessageBox::information(this, "重新载入", "本地配置已重新载入。");
            return;
        }
        appendLog("pulling config...");
        pullConfigButton_->setEnabled(false);
        apiClient_.pullConfig();
    });

    connect(connectWsButton_, &QPushButton::clicked, this, [this]() {
        if (!validateSettings(true)) {
            return;
        }
        configStore_.save(currentSettings());
        applySettingsToClients();
        webSocketClient_.start();
    });

    connect(startWatchButton_, &QPushButton::clicked, this, &MainWindow::toggleWorkstation);

    connect(uploadButton_, &QPushButton::clicked, this, [this]() {
        if (!validateSettings(true)) {
            return;
        }
        configStore_.save(currentSettings());
        applySettingsToClients();
        chooseAndUploadFile();
    });

    connect(refreshUploadsButton_, &QPushButton::clicked, this, &MainWindow::refreshUploadTable);
    connect(retrySelectedButton_, &QPushButton::clicked, this, &MainWindow::retrySelectedUpload);
    connect(clearFailedButton_, &QPushButton::clicked, this, &MainWindow::clearFailedUploads);
    connect(exportDiagnosticsButton_, &QPushButton::clicked, this, &MainWindow::exportDiagnostics);

    connect(&apiClient_, &ApiClient::registerSucceeded, this, [this](const QJsonObject &payload) {
        registerButton_->setEnabled(true);
        saveButton_->setEnabled(true);
        const bool hadToken = !currentSettings().token.trimmed().isEmpty();
        bool receivedToken = false;
        if (payload.contains("workstationToken")) {
            tokenEdit_->setText(payload.value("workstationToken").toString());
            configStore_.save(currentSettings());
            applySettingsToClients();
            receivedToken = !tokenEdit_->text().trimmed().isEmpty();
        }
        appendLog("register ok: " + QString::fromUtf8(QJsonDocument(sanitizedPayload(payload)).toJson(QJsonDocument::Compact)));
        if (startPendingAfterRegister_) {
            startPendingAfterRegister_ = false;
            if (currentSettings().token.trimmed().isEmpty()) {
                startWatchButton_->setEnabled(true);
                QMessageBox::warning(
                    this,
                    "需要重置 Token",
                    "平台已有该 MAC 的工作站记录，但本地没有可用 Token。请在平台重置该工作站 Token 后重新注册。"
                );
                return;
            }
            appendLog("pulling config before start...");
            startPendingAfterConfig_ = true;
            pullConfigButton_->setEnabled(false);
            apiClient_.pullConfig();
            return;
        }
        if (receivedToken) {
            QMessageBox::information(this, "注册成功", "工作站已注册，Token 已保存。请点击“重新载入”同步监听配置。");
        } else if (hadToken) {
            QMessageBox::information(this, "注册成功", "工作站已存在，已沿用本地保存的 Token。请点击“重新载入”同步监听配置。");
        } else {
            QMessageBox::warning(
                this,
                "注册已更新",
                "平台已有该 MAC 的工作站记录，本次注册不会重新返回明文 Token。已尝试从旧版配置导入 Token；如果仍无法启动，请在平台重置该工作站 Token 后重新注册。"
            );
        }
    });

    connect(&apiClient_, &ApiClient::configPulled, this, [this](const QJsonObject &payload) {
        pullConfigButton_->setEnabled(true);
        applyConfigPayload(payload);
        appendLog("config pulled");
        if (startPendingAfterConfig_) {
            startPendingAfterConfig_ = false;
            startWatchButton_->setEnabled(true);
            webSocketClient_.start();
            watchManager_.setRuntimeConfig(runtimeConfig_);
            watchManager_.start();
            workstationRunning_ = true;
            appendLog("工作站已启动");
            updateStatusCards();
            return;
        }
        QMessageBox::information(this, "配置已更新", QString("平台配置已同步，当前监听目录数：%1。").arg(runtimeConfig_.devices.size()));
    });

    connect(&apiClient_, &ApiClient::requestFailed, this, [this](const QString &operation, const QString &message) {
        if (operation == "register") {
            registerButton_->setEnabled(true);
            saveButton_->setEnabled(true);
            startPendingAfterRegister_ = false;
        }
        if (operation == "pullConfig") {
            pullConfigButton_->setEnabled(true);
            startPendingAfterConfig_ = false;
        }
        startWatchButton_->setEnabled(true);
        appendLog(operation + " failed: " + message);
        const QString title = operation == "register" ? "注册失败" : (operation == "pullConfig" ? "配置同步失败" : "请求失败");
        QMessageBox::warning(this, title, message);
    });

    connect(&uploadManager_, &UploadManager::logMessage, this, &MainWindow::appendLog);
    connect(&uploadManager_, &UploadManager::recordsChanged, this, &MainWindow::refreshUploadTable);

    connect(&webSocketClient_, &WebSocketClient::logMessage, this, &MainWindow::appendLog);
    connect(&webSocketClient_, &WebSocketClient::connectionStateChanged, this, [this](const QString &state) {
        connectWsButton_->setText(state == "connected" ? "通道已连接" : "连接通道");
        if (runtimeHintLabel_ != nullptr) {
            setStatusValue(runtimeHintLabel_, state == "connected" ? "平台在线" : "平台离线", state == "connected" ? "#2563eb" : "#64748b");
        }
        statusBar()->showMessage("WebSocket: " + state, 5000);
        updateStatusCards();
    });
    connect(&webSocketClient_, &WebSocketClient::configReceived, this, [this](const QJsonObject &payload) {
        applyConfigPayload(payload);
        appendLog("config received from websocket");
    });
    connect(&webSocketClient_, &WebSocketClient::taskResultReceived, this, [this](const QJsonObject &payload) {
        QString errorMessage;
        if (!database_.markTaskResult(payload, &errorMessage)) {
            appendLog("mark task result failed: " + errorMessage);
        }
        appendLog("task result: " + QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
        refreshUploadTable();
    });
    connect(&webSocketClient_, &WebSocketClient::doctorRunRequested, this, [this](const QString &messageId, const QString &requestId, bool checkNetwork) {
        appendLog(QString("doctor.run received requestId=%1 checkNetwork=%2").arg(requestId, checkNetwork ? "true" : "false"));
        const QJsonObject result = runDiagnostics(checkNetwork, false);
        webSocketClient_.sendDoctorResult(messageId, requestId, result);
    });
    connect(&watchManager_, &WatchManager::logMessage, this, &MainWindow::appendLog);
    connect(&watchManager_, &WatchManager::fileReady, this, [this](const UploadRequest &request) {
        uploadRequest(request);
    });
    updateStatusCards();
    refreshDeviceTable();
}

void MainWindow::buildUi() {
    setWindowTitle("NetStar 解析工作站");
    resize(1360, 820);
    applyLegacyStyle();

    auto *root = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *navBar = new QWidget(root);
    navBar->setObjectName("navBar");
    auto *navLayout = new QHBoxLayout(navBar);
    navLayout->setContentsMargins(22, 12, 22, 12);
    navLayout->setSpacing(8);
    navLayout->addWidget(buildNavButton("首页", 0));
    navLayout->addWidget(buildNavButton("注册", 1));
    navLayout->addWidget(buildNavButton("日志", 2));
    navLayout->addWidget(buildNavButton("网络诊断", 3));
    navLayout->addStretch(1);

    auto *statusCluster = new QWidget(navBar);
    statusCluster->setObjectName("topStatusBar");
    auto *statusLayout = new QHBoxLayout(statusCluster);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->setSpacing(24);

    auto addStatusItem = [statusCluster, statusLayout](QLabel **valueTarget) {
        auto *value = new QLabel("-", statusCluster);
        value->setObjectName("statusValue");
        statusLayout->addWidget(value, 0, Qt::AlignVCenter);
        *valueTarget = value;
    };

    addStatusItem(&runtimeValueLabel_);
    auto *channelValueLabel = new QLabel("平台离线", statusCluster);
    channelValueLabel->setObjectName("statusValue");
    runtimeHintLabel_ = channelValueLabel;
    statusLayout->addWidget(channelValueLabel, 0, Qt::AlignVCenter);
    addStatusItem(&deviceValueLabel_);
    addStatusItem(&failedValueLabel_);
    navLayout->addWidget(statusCluster, 0, Qt::AlignVCenter);
    navLayout->addStretch(1);

    primaryActionButton_ = new QPushButton("启动工作站", navBar);
    primaryActionButton_->setObjectName("primaryButton");
    primaryActionButton_->setMinimumWidth(132);
    connect(primaryActionButton_, &QPushButton::clicked, this, &MainWindow::toggleWorkstation);
    navLayout->addWidget(primaryActionButton_, 0, Qt::AlignRight | Qt::AlignVCenter);

    pages_ = new QStackedWidget(root);
    pages_->addWidget(buildStatusTab());
    pages_->addWidget(buildConnectionTab());
    pages_->addWidget(buildLogsTab());
    pages_->addWidget(buildDiagnosticsTab());

    rootLayout->addWidget(navBar);
    rootLayout->addWidget(pages_, 1);
    setCentralWidget(root);
    setCurrentPage(0);
}

void MainWindow::applyLegacyStyle() {
    setStyleSheet(R"(
        QMainWindow, QWidget {
            background: #f7f9fb;
            color: #1f2d3d;
            font-size: 13px;
            font-family: "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", "Arial";
        }
        QTabWidget {
            background: #f7f9fb;
        }
        QTabWidget::pane {
            border: 0;
            background: #f7f9fb;
        }
        QTabBar::tab {
            min-width: 104px;
            min-height: 42px;
            padding: 8px 18px;
            margin: 0;
            border: 1px solid transparent;
            border-radius: 0;
            background: #172536;
            color: #aebdca;
            font-weight: 700;
        }
        QTabBar::tab:selected {
            background: #0f766e;
            color: #ffffff;
            border-color: #0f766e;
        }
        QTabBar::tab:hover:!selected {
            background: #213247;
            color: #ffffff;
        }
        QWidget#navBar {
            background: #ffffff;
            border-bottom: 1px solid #e5ebf0;
        }
        QWidget#topStatusBar {
            background: #ffffff;
        }
        QLabel#statusValue {
            color: #102033;
            font-size: 13px;
            font-weight: 800;
        }
        QPushButton#navButton {
            min-height: 32px;
            padding: 0 17px;
            border: 1px solid transparent;
            border-radius: 8px;
            background: #f1f5f9;
            color: #475569;
            text-align: center;
            font-weight: 800;
        }
        QPushButton#navButton:hover {
            background: #e8eef5;
            color: #102033;
        }
        QPushButton#navButton[active="true"] {
            background: #0f766e;
            color: #ffffff;
            border-color: #0f766e;
        }
        QGroupBox {
            border: 0;
            border-radius: 0;
            margin-top: 18px;
            padding: 0;
            background: transparent;
            font-weight: 700;
            color: #18324a;
        }
        QGroupBox[panelRole="settings"] {
            border: 1px solid #e1e8ef;
            border-radius: 10px;
            background: #ffffff;
            padding: 16px 16px 14px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 0;
            padding: 0 0 4px 0;
            color: #102033;
            font-size: 14px;
            font-weight: 800;
        }
        QLabel#sectionTitle {
            color: #102033;
            font-size: 14px;
            font-weight: 800;
        }
        QWidget#heroPanel {
            border: 1px solid #dde5ec;
            border-radius: 10px;
            background: #ffffff;
        }
        QWidget#statusStrip {
            border: 0;
            background: transparent;
        }
        QWidget#summaryCard {
            border: 1px solid #e1e8ef;
            border-left: 3px solid #94a3b8;
            border-radius: 8px;
            background: #ffffff;
        }
        QWidget#actionBar {
            background: transparent;
        }
        QWidget#formShell {
            background: transparent;
        }
        QWidget#summaryCard[tone="running"], QWidget#summaryCard[tone="ready"] {
            border-left-color: #0f766e;
            background: #ffffff;
        }
        QWidget#summaryCard[tone="alert"] {
            border-left-color: #dc2626;
            background: #ffffff;
        }
        QLabel { background: transparent; }
        QLabel#heroTitle {
            color: #102033;
            font-size: 18px;
            font-weight: 800;
        }
        QLabel#heroMeta {
            color: #64748b;
            font-size: 11px;
        }
        QLabel#summaryTitle {
            color: #64748b;
            font-size: 11px;
            font-weight: 700;
        }
        QLabel#summaryValue {
            color: #11324d;
            font-size: 19px;
            font-weight: 800;
        }
        QLabel#summaryHint {
            color: #92a0ae;
            font-size: 11px;
        }
        QLabel#fieldValue {
            min-height: 32px;
            background: #f8fafc;
            color: #334155;
            border: 1px solid #d7e0e8;
            border-radius: 8px;
            padding: 6px 12px;
        }
        QPushButton {
            min-height: 30px;
            padding: 2px 12px;
            border: 1px solid #cfd9e3;
            border-radius: 7px;
            background: #ffffff;
            color: #18324a;
            font-weight: 700;
        }
        QPushButton:hover {
            background: #f4f8fa;
            border-color: #87a1b7;
        }
        QPushButton#primaryButton {
            background: #0f766e;
            color: #ffffff;
            border-color: #0f766e;
        }
        QPushButton#primaryButton:hover {
            background: #0d9488;
            border-color: #0d9488;
        }
        QLineEdit, QPlainTextEdit, QSpinBox {
            border: 1px solid #d7e0e8;
            border-radius: 8px;
            background: #ffffff;
            color: #24384d;
            selection-background-color: #c8e7e3;
        }
        QLineEdit, QSpinBox {
            min-height: 34px;
            padding: 0 12px;
        }
        QLineEdit:focus, QSpinBox:focus, QPlainTextEdit:focus {
            border-color: #7ba7b8;
        }
        QPlainTextEdit { padding: 8px 10px; }
        QLineEdit:read-only, QPlainTextEdit:read-only {
            background: #f8fafc;
            color: #475569;
        }
        QTableWidget {
            border: 1px solid #e1e8ef;
            border-radius: 8px;
            background: #ffffff;
            gridline-color: #edf2f5;
            alternate-background-color: #f8fafc;
            selection-background-color: #dbeafe;
            selection-color: #102033;
        }
        QHeaderView::section {
            background: #f1f5f9;
            padding: 8px 8px;
            border: 0;
            border-right: 1px solid #dde5eb;
            color: #4f6274;
            font-weight: 700;
        }
        QTableWidget::item { padding: 5px 8px; }
        QTableWidget::item:selected {
            background: #dbeafe;
            color: #102033;
        }
        QGroupBox QPushButton {
            min-height: 28px;
            padding: 2px 11px;
        }
        QPlainTextEdit#logConsole {
            border: 1px solid #d7e0e8;
            border-radius: 10px;
            background: #0f172a;
            color: #dbeafe;
            selection-background-color: #334155;
            font-family: "Menlo", "Monaco", "Courier New";
        }
        QScrollArea {
            border: 0;
            background: #f7f9fb;
        }
        QScrollArea > QWidget > QWidget {
            background: #f7f9fb;
        }
    )");
}

QWidget *MainWindow::buildStatusTab() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 12, 18, 16);
    layout->setSpacing(12);

    auto *deviceGroup = new QGroupBox("监听配置", page);
    auto *deviceLayout = new QVBoxLayout(deviceGroup);
    deviceLayout->setContentsMargins(0, 6, 0, 0);
    deviceLayout->setSpacing(6);
    deviceTable_ = new QTableWidget(0, 4, deviceGroup);
    deviceTable_->setHorizontalHeaderLabels({"设备", "目录", "类型", "状态"});
    configureTable(deviceTable_);
    deviceTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    deviceTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    deviceTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    deviceTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    deviceTable_->setColumnWidth(0, 180);
    deviceTable_->setColumnWidth(2, 100);
    deviceTable_->setColumnWidth(3, 100);
    deviceTable_->setMinimumHeight(130);
    deviceTable_->setMaximumHeight(190);
    deviceTable_->clearSelection();
    deviceTable_->setCurrentCell(-1, -1);
    deviceLayout->addWidget(deviceTable_, 1);
    layout->addWidget(deviceGroup, 0);

    auto *recentGroup = new QWidget(page);
    auto *recentLayout = new QVBoxLayout(recentGroup);
    recentLayout->setContentsMargins(0, 0, 0, 0);
    recentLayout->setSpacing(8);
    auto *recentHeader = new QHBoxLayout();
    recentHeader->setContentsMargins(0, 0, 0, 0);
    recentHeader->setSpacing(8);
    auto *recentTitle = new QLabel("文件记录", recentGroup);
    recentTitle->setObjectName("sectionTitle");
    recentHeader->addWidget(recentTitle);
    recentHeader->addStretch(1);
    auto *actions = new QHBoxLayout();
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    refreshUploadsButton_ = new QPushButton("刷新", recentGroup);
    retrySelectedButton_ = new QPushButton("重试上传", recentGroup);
    clearFailedButton_ = new QPushButton("清除失败记录", recentGroup);
    uploadButton_ = new QPushButton("上传文件", recentGroup);
    actions->addWidget(uploadButton_);
    actions->addWidget(refreshUploadsButton_);
    actions->addWidget(retrySelectedButton_);
    actions->addWidget(clearFailedButton_);
    recentHeader->addLayout(actions);
    recentLayout->addLayout(recentHeader);

    uploadTable_ = new QTableWidget(0, 6, recentGroup);
    uploadTable_->setHorizontalHeaderLabels({"目录", "文件名", "大小", "状态", "上传时间", "操作"});
    configureTable(uploadTable_);
    uploadTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    uploadTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    uploadTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    uploadTable_->setColumnWidth(0, 360);
    uploadTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    uploadTable_->setColumnWidth(1, 320);
    uploadTable_->setColumnWidth(2, 90);
    uploadTable_->setColumnWidth(3, 90);
    uploadTable_->setColumnWidth(4, 180);
    uploadTable_->setMinimumHeight(330);
    recentLayout->addWidget(uploadTable_, 1);
    layout->addWidget(recentGroup, 1);

    return wrapPage(page);
}

QWidget *MainWindow::buildConnectionTab() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 20, 24, 22);
    layout->setSpacing(16);

    auto *shell = new QWidget(page);
    shell->setObjectName("formShell");
    shell->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *shellLayout = new QVBoxLayout(shell);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(16);

    auto *accessGroup = new QGroupBox("接入配置", page);
    accessGroup->setProperty("panelRole", "settings");
    auto *accessForm = new QFormLayout(accessGroup);
    accessForm->setHorizontalSpacing(16);
    accessForm->setVerticalSpacing(10);
    accessForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    accessForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    baseUrlEdit_ = new QLineEdit(accessGroup);
    ipEdit_ = new QLineEdit(accessGroup);
    ipEdit_->setPlaceholderText("本机 IP，可留空");
    tokenEdit_ = new QLineEdit(accessGroup);
    tokenEdit_->setVisible(false);
    accessForm->addRow("平台地址", baseUrlEdit_);
    accessForm->addRow("IP 地址", ipEdit_);
    shellLayout->addWidget(accessGroup);

    auto *infoGroup = new QGroupBox("工作站信息", page);
    infoGroup->setProperty("panelRole", "settings");
    auto *infoForm = new QFormLayout(infoGroup);
    infoForm->setHorizontalSpacing(16);
    infoForm->setVerticalSpacing(10);
    infoForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    infoForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    macEdit_ = new QLineEdit(infoGroup);
    hostnameEdit_ = new QLineEdit(infoGroup);
    macEdit_->setReadOnly(true);
    hostnameEdit_->setReadOnly(true);
    configVersionLabel_ = new QLabel("-", infoGroup);
    configVersionLabel_->setObjectName("fieldValue");
    infoForm->addRow("MAC", macEdit_);
    infoForm->addRow("主机名", hostnameEdit_);
    infoForm->addRow("监听目录数", configVersionLabel_);
    shellLayout->addWidget(infoGroup);

    deviceIdSpin_ = new QSpinBox(page);
    deviceIdSpin_->setRange(1, 1000000000);
    deviceIdSpin_->setVisible(false);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 4, 0, 0);
    buttonRow->setSpacing(10);
    pullConfigButton_ = new QPushButton("重新载入", page);
    registerButton_ = new QPushButton("注册", page);
    registerButton_->setVisible(false);
    saveButton_ = new QPushButton("保存", page);
    saveButton_->setObjectName("primaryButton");
    buttonRow->addWidget(pullConfigButton_);
    buttonRow->addWidget(saveButton_);
    buttonRow->addStretch(1);
    shellLayout->addLayout(buttonRow);

    connectWsButton_ = new QPushButton("连接通道", page);
    connectWsButton_->setVisible(false);
    startWatchButton_ = new QPushButton("启动监听", page);
    startWatchButton_->setVisible(false);
    layout->addWidget(shell, 0, Qt::AlignTop);
    layout->addStretch(1);
    return wrapPage(page);
}

QWidget *MainWindow::buildLogsTab() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    auto *logGroup = new QGroupBox("详细日志", page);
    auto *logLayout = new QVBoxLayout(logGroup);
    logPathLabel_ = new QLabel(LogManager::logFilePath(), logGroup);
    logPathLabel_->setObjectName("fieldValue");
    logPathLabel_->setWordWrap(true);
    logPathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    logLayout->addWidget(logPathLabel_);

    auto *buttonRow = new QHBoxLayout();
    openLogDirectoryButton_ = new QPushButton("打开日志目录", logGroup);
    connect(openLogDirectoryButton_, &QPushButton::clicked, this, &MainWindow::openLogDirectory);
    buttonRow->addWidget(openLogDirectoryButton_);
    buttonRow->addStretch(1);
    logLayout->addLayout(buttonRow);

    logEdit_ = new QPlainTextEdit(logGroup);
    logEdit_->setObjectName("logConsole");
    logEdit_->setReadOnly(true);
    logEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
    logLayout->addWidget(logEdit_, 1);
    layout->addWidget(logGroup, 1);
    return wrapPage(page);
}

QWidget *MainWindow::buildDiagnosticsTab() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    auto *group = new QGroupBox("网络诊断", page);
    group->setProperty("panelRole", "settings");
    auto *groupLayout = new QVBoxLayout(group);
    diagnosticsStatusLabel_ = new QLabel("未检查", group);
    diagnosticsStatusLabel_->setObjectName("fieldValue");
    groupLayout->addWidget(diagnosticsStatusLabel_);

    auto *buttonRow = new QHBoxLayout();
    auto *runButton = new QPushButton("诊断", group);
    runButton->setObjectName("primaryButton");
    exportDiagnosticsButton_ = new QPushButton("导出诊断包", group);
    buttonRow->addWidget(runButton);
    buttonRow->addWidget(exportDiagnosticsButton_);
    buttonRow->addStretch(1);
    groupLayout->addLayout(buttonRow);

    diagnosticsTable_ = new QTableWidget(0, 3, group);
    diagnosticsTable_->setHorizontalHeaderLabels({"检查项", "状态", "说明"});
    configureTable(diagnosticsTable_);
    diagnosticsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    groupLayout->addWidget(diagnosticsTable_, 1);
    connect(runButton, &QPushButton::clicked, this, [this]() { runDiagnostics(true, true); });
    layout->addWidget(group, 1);
    return wrapPage(page);
}

QWidget *MainWindow::wrapPage(QWidget *content) {
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    return scroll;
}

void MainWindow::configureTable(QTableWidget *table) const {
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(34);
    table->horizontalHeader()->setMinimumHeight(38);
    table->horizontalHeader()->setSectionsMovable(true);
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
}

QPushButton *MainWindow::buildNavButton(const QString &text, int pageIndex) {
    auto *button = new QPushButton(text, this);
    button->setObjectName("navButton");
    button->setCheckable(true);
    button->setProperty("active", false);
    connect(button, &QPushButton::clicked, this, [this, pageIndex]() { setCurrentPage(pageIndex); });
    navButtons_.append(button);
    return button;
}

void MainWindow::setCurrentPage(int pageIndex) {
    if (pages_ == nullptr || pageIndex < 0 || pageIndex >= pages_->count()) {
        return;
    }
    pages_->setCurrentIndex(pageIndex);
    for (int index = 0; index < navButtons_.size(); ++index) {
        QPushButton *button = navButtons_[index];
        const bool active = index == pageIndex;
        button->setChecked(active);
        button->setProperty("active", active);
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
}

void MainWindow::setupTray() {
    const QIcon icon(":/assets/assets/netstar-workstation.png");
    setWindowIcon(icon);

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        appendLog("系统托盘不可用");
        return;
    }

    trayMenu_ = new QMenu(this);
    QAction *showAction = trayMenu_->addAction("打开");
    QAction *startAction = trayMenu_->addAction("启动监听");
    QAction *stopAction = trayMenu_->addAction("停止监听");
    QAction *logsAction = trayMenu_->addAction("打开日志目录");
    trayMenu_->addSeparator();
    QAction *quitAction = trayMenu_->addAction("退出");

    connect(showAction, &QAction::triggered, this, [this]() {
        showNormal();
        raise();
        activateWindow();
    });
    connect(startAction, &QAction::triggered, this, &MainWindow::toggleWorkstation);
    connect(stopAction, &QAction::triggered, this, &MainWindow::stopWorkstation);
    connect(logsAction, &QAction::triggered, this, &MainWindow::openLogDirectory);
    connect(quitAction, &QAction::triggered, this, &MainWindow::quitApplication);

    trayIcon_ = new QSystemTrayIcon(icon, this);
    trayIcon_->setToolTip("NetStar Parse Hub");
    trayIcon_->setContextMenu(trayMenu_);
    connect(trayIcon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            if (isVisible()) {
                hide();
            } else {
                showNormal();
                raise();
                activateWindow();
            }
        }
    });
    trayIcon_->show();
}

void MainWindow::loadSettings() {
    const WorkstationSettings settings = configStore_.load();
    baseUrlEdit_->setText(settings.baseUrl);
    if (ipEdit_ != nullptr) {
        ipEdit_->setText(settings.ip);
    }
    macEdit_->setText(settings.mac);
    if (settings.mac.trimmed().isEmpty()) {
        macEdit_->setReadOnly(false);
        macEdit_->setPlaceholderText("未检测到 MAC，请手动填写后注册");
    }
    tokenEdit_->setText(settings.token);
    hostnameEdit_->setText(settings.hostname);
    apiClient_.setSettings(settings);
    webSocketClient_.setSettings(settings);

    QString configError;
    RuntimeConfig cachedConfig = configStore_.loadRuntimeConfig(&configError);
    if (!configError.isEmpty()) {
        appendLog("cached config ignored: " + configError);
        return;
    }
    if (!cachedConfig.devices.isEmpty()) {
        runtimeConfig_ = cachedConfig;
        webSocketClient_.setHeartbeatIntervalSeconds(runtimeConfig_.heartbeatIntervalSeconds);
        watchManager_.setRuntimeConfig(runtimeConfig_);
        appendLog(QString("cached config loaded version=%1 items=%2")
                      .arg(runtimeConfig_.configVersion)
                      .arg(runtimeConfig_.devices.size()));
    }
    refreshDeviceTable();
    updateStatusCards();
}

WorkstationSettings MainWindow::currentSettings() const {
    WorkstationSettings settings;
    settings.baseUrl = baseUrlEdit_->text();
    settings.mac = macEdit_->text().trimmed();
    settings.ip = ipEdit_ != nullptr ? ipEdit_->text().trimmed() : QString();
    settings.token = tokenEdit_->text().trimmed();
    settings.hostname = hostnameEdit_->text().trimmed();
    return settings;
}

bool MainWindow::validateSettings(bool requireToken) const {
    const WorkstationSettings settings = currentSettings();
    const QUrl url(settings.baseUrl.trimmed());
    const bool validHttpUrl = url.isValid() && (url.scheme() == "http" || url.scheme() == "https") && !url.host().isEmpty();
    if (!validHttpUrl) {
        QMessageBox::warning(nullptr, "配置不完整", "平台地址必须是完整的 HTTP 或 HTTPS 地址。");
        return false;
    }
    if (settings.mac.isEmpty()) {
        QMessageBox::warning(nullptr, "配置不完整", "未读取到 MAC 地址，请确认网络接口可用后重试。");
        return false;
    }
    if (settings.hostname.isEmpty()) {
        QMessageBox::warning(nullptr, "配置不完整", "未读取到主机名，请确认系统信息可用后重试。");
        return false;
    }
    if (requireToken && settings.token.isEmpty()) {
        QMessageBox::warning(nullptr, "需要注册", "请先在“注册”页面完成工作站注册，再启动工作站。");
        return false;
    }
    return true;
}

void MainWindow::applySettingsToClients() {
    const WorkstationSettings settings = currentSettings();
    apiClient_.setSettings(settings);
    webSocketClient_.setSettings(settings);
    uploadManager_.setUploadsEnabled(!settings.token.trimmed().isEmpty());
}

void MainWindow::appendLog(const QString &message) {
    logEdit_->appendPlainText(QString("[%1] %2").arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), message));
    statusBar()->showMessage(message.left(180), 5000);
    LogManager::appendLine(message);
}

void MainWindow::toggleWorkstation() {
    if (workstationRunning_) {
        stopWorkstation();
        return;
    }
    if (!validateSettings(false)) {
        return;
    }
    configStore_.save(currentSettings());
    applySettingsToClients();
    startPendingAfterRegister_ = true;
    startPendingAfterConfig_ = false;
    startWatchButton_->setEnabled(false);
    registerButton_->setEnabled(false);
    saveButton_->setEnabled(false);
    appendLog("registering workstation before start...");
    apiClient_.registerWorkstation();
    updateStatusCards();
}

void MainWindow::stopWorkstation() {
    watchManager_.stop();
    webSocketClient_.stop();
    workstationRunning_ = false;
    appendLog("工作站已停止");
    updateStatusCards();
}

void MainWindow::setStatusValue(QLabel *label, const QString &text, const QString &color) {
    if (label == nullptr) {
        return;
    }
    label->setText(text);
    label->setStyleSheet(QString("color: %1; font-weight: 700;").arg(color));
}

void MainWindow::updateStatusCards() {
    if (runtimeValueLabel_ != nullptr) {
        setStatusValue(runtimeValueLabel_, workstationRunning_ ? "运行中" : "未启动", workstationRunning_ ? "#0f766e" : "#64748b");
    }
    if (runtimeHintLabel_ != nullptr && !workstationRunning_) {
        setStatusValue(runtimeHintLabel_, "平台离线", "#64748b");
    }
    if (deviceValueLabel_ != nullptr) {
        const int enabledCount = std::count_if(runtimeConfig_.devices.cbegin(), runtimeConfig_.devices.cend(), [](const DeviceConfig &device) {
            return device.enabled;
        });
        const bool hasEnabledDevice = enabledCount > 0;
        setStatusValue(deviceValueLabel_, QString("监听 %1/%2").arg(enabledCount).arg(runtimeConfig_.devices.size()), hasEnabledDevice ? "#0f766e" : "#64748b");
    }
    if (configVersionLabel_ != nullptr) {
        configVersionLabel_->setText(QString("%1 个 / 版本 %2").arg(runtimeConfig_.devices.size()).arg(runtimeConfig_.configVersion));
    }
    if (primaryActionButton_ != nullptr) {
        primaryActionButton_->setText(workstationRunning_ ? "停止工作站" : "启动工作站");
    }
    if (startWatchButton_ != nullptr) {
        startWatchButton_->setText(workstationRunning_ ? "停止监听" : "启动监听");
    }
}

void MainWindow::refreshDeviceTable() {
    if (deviceTable_ == nullptr) {
        return;
    }
    deviceTable_->setRowCount(runtimeConfig_.devices.size());
    for (int row = 0; row < runtimeConfig_.devices.size(); ++row) {
        const DeviceConfig &device = runtimeConfig_.devices[row];
        QStringList values;
        if (deviceTable_->columnCount() == 4) {
            values = QStringList({
                device.deviceName.isEmpty() ? QString::number(device.deviceId) : device.deviceName,
                device.watchPath,
                device.fileType,
                device.enabled ? "是" : "否",
            });
        } else {
            values = QStringList({
                QString::number(device.deviceId),
                device.deviceCode,
                device.deviceName,
                device.watchPath,
                device.fileType,
                QString::number(device.stableSeconds),
                device.enabled ? "是" : "否",
            });
        }
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values[column]);
            item->setToolTip(values[column]);
            deviceTable_->setItem(row, column, item);
        }
    }
    deviceTable_->clearSelection();
    deviceTable_->setCurrentCell(-1, -1);
}

QJsonObject MainWindow::runDiagnostics(bool checkNetwork, bool showMessage) {
    const QJsonObject result = SystemDiagnostics::run(currentSettings(), runtimeConfig_, checkNetwork);
    renderDiagnostics(result);
    appendLog("diagnostics completed: " + result.value("status").toString());
    if (showMessage) {
        QMessageBox::information(this, "诊断完成", "网络诊断已完成。");
    }
    return result;
}

void MainWindow::renderDiagnostics(const QJsonObject &result) {
    if (diagnosticsStatusLabel_ != nullptr) {
        diagnosticsStatusLabel_->setText(result.value("status").toString() == "ok" ? "总体：正常" : "总体：失败");
    }
    if (diagnosticsTable_ == nullptr) {
        return;
    }

    const QHash<QString, QString> names = {
        {"config", "配置文件"},
        {"token", "注册状态"},
        {"watchPath", "监听目录"},
        {"api", "平台连接"},
        {"stateDb", "状态库"},
        {"logDir", "日志目录"},
        {"diagnostics", "诊断"},
    };
    const QHash<QString, QString> statuses = {
        {"ok", "正常"},
        {"failed", "失败"},
        {"skipped", "跳过"},
    };
    const QHash<QString, QHash<QString, QString>> messages = {
        {"config", {{"ok", "配置文件已加载"}, {"failed", "配置文件读取失败"}}},
        {"token", {{"ok", "工作站已注册"}, {"failed", "工作站尚未注册"}}},
        {"watchPath", {{"ok", "监听目录可访问"}, {"failed", "监听目录不可访问"}}},
        {"api", {{"ok", "平台连接正常"}, {"failed", "平台连接异常"}, {"skipped", "未执行网络检查"}}},
        {"stateDb", {{"ok", "状态库可用"}, {"failed", "状态库异常"}}},
        {"logDir", {{"ok", "日志目录可写"}, {"failed", "日志目录不可写"}}},
    };

    const QJsonArray checks = result.value("checks").toArray();
    diagnosticsTable_->setRowCount(checks.size());
    for (int row = 0; row < checks.size(); ++row) {
        const QJsonObject check = checks[row].toObject();
        const QString rawName = check.value("name").toString();
        const QString rawStatus = check.value("status").toString();
        const QString displayName = names.value(rawName, rawName.isEmpty() ? "检查项" : rawName);
        const QString displayStatus = statuses.value(rawStatus, rawStatus);
        const QString displayMessage = messages.value(rawName).value(rawStatus, check.value("message").toString());
        const QStringList values = {displayName, displayStatus, displayMessage};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values[column]);
            item->setToolTip(check.value("message").toString());
            diagnosticsTable_->setItem(row, column, item);
        }
    }
    diagnosticsTable_->resizeColumnsToContents();
    diagnosticsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
}

void MainWindow::chooseAndUploadFile() {
    const QString path = QFileDialog::getOpenFileName(this, "Choose file to upload");
    if (path.isEmpty()) {
        return;
    }

    QString errorMessage;
    const QString hash = FileHasher::sha256(path, &errorMessage);
    if (!errorMessage.isEmpty()) {
        QMessageBox::warning(this, "Hash failed", errorMessage);
        return;
    }

    QFileInfo info(path);
    UploadRequest request;
    request.deviceId = deviceIdSpin_->value();
    request.localPath = info.absoluteFilePath();
    request.fileName = info.fileName();
    request.fileSize = info.size();
    request.fileMtime = info.lastModified();
    request.fileHash = hash;

    appendLog(QString("uploading %1 sha256=%2").arg(request.fileName, request.fileHash));
    uploadRequest(request, true);
}

void MainWindow::applyConfigPayload(const QJsonObject &payload) {
    QString errorMessage;
    RuntimeConfig parsed = RuntimeConfig::fromJson(payload, &errorMessage);
    if (!errorMessage.isEmpty()) {
        appendLog("config rejected: " + errorMessage);
        return;
    }
    runtimeConfig_ = parsed;
    configStore_.saveRuntimeConfig(runtimeConfig_);
    webSocketClient_.setHeartbeatIntervalSeconds(runtimeConfig_.heartbeatIntervalSeconds);
    watchManager_.setRuntimeConfig(runtimeConfig_);
    appendLog(QString("config version=%1 items=%2 heartbeat=%3s")
                  .arg(runtimeConfig_.configVersion)
                  .arg(runtimeConfig_.devices.size())
                  .arg(runtimeConfig_.heartbeatIntervalSeconds));
    refreshDeviceTable();
    updateStatusCards();
}

void MainWindow::uploadRequest(const UploadRequest &request, bool manual) {
    uploadManager_.submitUpload(request, manual);
}

void MainWindow::refreshUploadTable() {
    if (uploadTable_ == nullptr) {
        return;
    }
    QString errorMessage;
    const QVector<StoredUpload> uploads = database_.recentUploads(200, &errorMessage);
    if (!errorMessage.isEmpty()) {
        appendLog("refresh uploads failed: " + errorMessage);
        return;
    }

    int failedCount = 0;
    uploadTable_->setRowCount(uploads.size());
    for (int row = 0; row < uploads.size(); ++row) {
        const StoredUpload &upload = uploads[row];
        if (upload.status.contains("failed", Qt::CaseInsensitive) || upload.status.contains("error", Qt::CaseInsensitive)) {
            ++failedCount;
        }
        const QFileInfo fileInfo(upload.request.localPath);
        const QStringList values = {
            fileInfo.dir().absolutePath(),
            upload.request.fileName,
            formattedBytes(upload.request.fileSize),
            translatedStatus(upload.status),
            upload.updatedAt,
            upload.lastErrorMessage.isEmpty() ? (upload.status.contains("failed", Qt::CaseInsensitive) ? "重试" : "-") : upload.lastErrorMessage.left(36),
        };
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values[column]);
            if (column == 0) {
                item->setData(Qt::UserRole, QVariant::fromValue(upload.request.localPath));
                item->setData(Qt::UserRole + 1, upload.request.deviceId);
                item->setData(Qt::UserRole + 2, upload.request.fileSize);
                item->setData(Qt::UserRole + 3, upload.request.fileMtime.toUTC().toString(Qt::ISODateWithMs));
                item->setData(Qt::UserRole + 4, upload.request.fileHash);
                item->setData(Qt::UserRole + 5, upload.request.fileName);
            }
            uploadTable_->setItem(row, column, item);
        }
    }
    if (failedValueLabel_ != nullptr) {
        setStatusValue(failedValueLabel_, QString("异常 %1").arg(failedCount), failedCount == 0 ? "#334155" : "#dc2626");
    }
}

void MainWindow::retrySelectedUpload() {
    const int row = uploadTable_->currentRow();
    if (row < 0 || uploadTable_->item(row, 0) == nullptr) {
        appendLog("no upload selected for retry");
        return;
    }

    QTableWidgetItem *anchor = uploadTable_->item(row, 0);
    UploadRequest request;
    request.localPath = anchor->data(Qt::UserRole).toString();
    request.deviceId = anchor->data(Qt::UserRole + 1).toInt();
    request.fileSize = anchor->data(Qt::UserRole + 2).toLongLong();
    request.fileMtime = QDateTime::fromString(anchor->data(Qt::UserRole + 3).toString(), Qt::ISODateWithMs);
    request.fileHash = anchor->data(Qt::UserRole + 4).toString();
    request.fileName = anchor->data(Qt::UserRole + 5).toString();

    QFileInfo info(request.localPath);
    if (!info.exists() || !info.isFile()) {
        appendLog("selected file is missing: " + request.localPath);
        return;
    }
    uploadRequest(request, true);
}

void MainWindow::clearFailedUploads() {
    QString errorMessage;
    const int removed = database_.clearFailedUploads(&errorMessage);
    if (removed < 0) {
        appendLog("clear failed uploads failed: " + errorMessage);
        return;
    }
    appendLog(QString("cleared failed upload records: %1").arg(removed));
    refreshUploadTable();
}

void MainWindow::exportDiagnostics() {
    const QString directory = QFileDialog::getExistingDirectory(this, "Choose diagnostics output folder");
    if (directory.isEmpty()) {
        return;
    }

    QString errorMessage;
    const QVector<StoredUpload> uploads = database_.recentUploads(500, &errorMessage);
    if (!errorMessage.isEmpty()) {
        appendLog("export diagnostics failed: " + errorMessage);
        return;
    }

    QString resultPath;
    if (!DiagnosticExporter::exportBundle(directory, currentSettings(), runtimeConfig_, uploads, &resultPath, &errorMessage)) {
        appendLog("export diagnostics failed: " + errorMessage);
        QMessageBox::warning(this, "Export failed", errorMessage);
        return;
    }
    appendLog("diagnostics exported: " + resultPath);
    QMessageBox::information(this, "Diagnostics exported", resultPath);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (allowQuit_ || trayIcon_ == nullptr || !trayIcon_->isVisible()) {
        event->accept();
        return;
    }
    hide();
    trayIcon_->showMessage("NetStar Parse Hub", "工作站仍在系统托盘运行。", QSystemTrayIcon::Information, 2500);
    event->ignore();
}

void MainWindow::quitApplication() {
    allowQuit_ = true;
    watchManager_.stop();
    webSocketClient_.stop();
    uploadManager_.stopRetryTimer();
    workstationRunning_ = false;
    QApplication::quit();
}

void MainWindow::openLogDirectory() {
    const QFileInfo info(LogManager::logFilePath());
    QDir dir = info.dir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

QString MainWindow::translatedStatus(const QString &status) {
    const QString normalized = status.trimmed().toLower();
    if (normalized == "uploaded" || normalized == "success" || normalized == "completed") {
        return "已上传";
    }
    if (normalized == "uploading" || normalized == "pending" || normalized == "queued") {
        return "上传中";
    }
    if (normalized == "failed" || normalized == "upload_failed" || normalized.contains("error")) {
        return "上传失败";
    }
    if (normalized == "parse_success" || normalized == "parsed") {
        return "解析成功";
    }
    if (normalized == "parse_failed") {
        return "解析失败";
    }
    return status.isEmpty() ? "-" : status;
}

QString MainWindow::formattedBytes(qint64 bytes) {
    static constexpr double kib = 1024.0;
    static constexpr double mib = 1024.0 * 1024.0;
    static constexpr double gib = 1024.0 * 1024.0 * 1024.0;
    if (bytes >= gib) {
        return QString::number(bytes / gib, 'f', 2) + " GB";
    }
    if (bytes >= mib) {
        return QString::number(bytes / mib, 'f', 2) + " MB";
    }
    if (bytes >= kib) {
        return QString::number(bytes / kib, 'f', 1) + " KB";
    }
    return QString::number(bytes) + " B";
}

QJsonObject MainWindow::sanitizedPayload(QJsonObject payload) {
    if (payload.contains("workstationToken")) {
        const QString token = payload.value("workstationToken").toString();
        payload["workstationToken"] = token.size() <= 8 ? "***" : token.left(4) + "***" + token.right(4);
    }
    return payload;
}

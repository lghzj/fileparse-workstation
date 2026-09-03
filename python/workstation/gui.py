from __future__ import annotations

import asyncio
import json
import logging
import sys
from datetime import datetime
from pathlib import Path
from zipfile import ZipFile

from workstation.cli import pull_config, register, retry_failed, upload
from workstation.client import SystemApiClient
from workstation.config import ensure_config, load_config, update_base_config
from workstation.state import StateStore
from workstation.windows_integration import doctor
from workstation.qt_compat import exec_application, load_qt

logger = logging.getLogger(__name__)
SINGLE_INSTANCE_SERVER_NAME = "netstar-parse-hub-workstation"
INTERNAL_TOOLS_PASSWORD = "199922"


def _load_qt():
    return load_qt()


def _resource_root() -> Path:
    if getattr(sys, "frozen", False):
        return Path(getattr(sys, "_MEIPASS", Path(sys.executable).resolve().parent))
    return Path(__file__).resolve().parents[1]


def _app_icon_path() -> Path | None:
    candidate = _resource_root() / "logo" / "workstation-icon-mac-v2.png"
    return candidate if candidate.exists() else None


def _runtime_program_path() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve()
    return Path(sys.executable)


def run_gui(config_path: Path, state_db: Path, log_file: Path) -> int:
    ensure_config(config_path)
    qt = _load_qt()
    app = qt["QApplication"](sys.argv)
    socket = qt["QLocalSocket"]()
    socket.connectToServer(SINGLE_INSTANCE_SERVER_NAME)
    if socket.waitForConnected(200):
        socket.write(b"activate")
        socket.flush()
        socket.waitForBytesWritten(200)
        return 0

    qt["QLocalServer"].removeServer(SINGLE_INSTANCE_SERVER_NAME)
    single_instance_server = qt["QLocalServer"]()
    single_instance_server.listen(SINGLE_INSTANCE_SERVER_NAME)
    icon_path = _app_icon_path()
    if icon_path is not None:
        app.setWindowIcon(qt["QIcon"](str(icon_path)))
    window = WorkstationMainWindow(qt, config_path, state_db, log_file)
    window.single_instance_server = single_instance_server

    def activate_existing_window() -> None:
        while single_instance_server.hasPendingConnections():
            client = single_instance_server.nextPendingConnection()
            client.deleteLater()
        window.show()

    single_instance_server.newConnection.connect(activate_existing_window)
    window.show()
    return exec_application(app)


class WorkstationMainWindow:
    def __init__(self, qt: dict, config_path: Path, state_db: Path, log_file: Path) -> None:
        self.qt = qt
        self.config_path = config_path
        self.state_db = state_db
        self.log_file = log_file
        self.process = None
        self.window = qt["QMainWindow"]()
        self.window.setWindowTitle("NetStar Parse Hub")
        self.window.resize(1320, 860)
        icon_path = _app_icon_path()
        if icon_path is not None:
            self.window.setWindowIcon(qt["QIcon"](str(icon_path)))
        self.window.setStyleSheet(
            """
            QMainWindow, QWidget {
                background: #eff3f6;
                color: #203040;
                font-size: 13px;
                font-family: "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei";
            }
            QTabWidget::pane {
                border: 1px solid #d6dee6;
                border-radius: 18px;
                background: #f7fafc;
                top: -1px;
            }
            QTabBar::tab {
                min-width: 84px;
                min-height: 16px;
                padding: 8px 16px;
                margin: 0 6px 0 0;
                border: 1px solid transparent;
                border-top-left-radius: 10px;
                border-top-right-radius: 10px;
                background: #e5ebf0;
                color: #5f7182;
                font-weight: 700;
            }
            QTabBar::tab:selected {
                background: #f7fafc;
                color: #11324d;
                border-color: #d6dee6;
            }
            QGroupBox {
                border: 1px solid #d9e2e9;
                border-radius: 18px;
                margin-top: 12px;
                padding: 12px 12px 10px;
                background: #ffffff;
                font-weight: 700;
                color: #16344c;
            }
            QGroupBox[panelRole="step"] {
                border: 1px solid #d5dee7;
                background: #fcfdfe;
            }
            QGroupBox[panelRole="settings"] {
                border: 1px solid #d5dee7;
                background: #f8fbfc;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 18px;
                padding: 0 7px;
            }
            QWidget#heroPanel {
                border: 1px solid #ced9e2;
                border-radius: 18px;
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #18354b, stop:1 #224d60);
            }
            QWidget#statusStrip {
                border: 1px solid #d9e1e8;
                border-radius: 16px;
                background: #f8fbfc;
            }
            QWidget#summaryCard {
                border: 1px solid #dbe3ea;
                border-radius: 14px;
                background: #ffffff;
            }
            QWidget#summaryCard[tone="runtime_running"] {
                border-left: 4px solid #1f8f68;
                background: #f8fcfa;
            }
            QWidget#summaryCard[tone="runtime_stopped"] {
                border-left: 4px solid #7b8b99;
                background: #fafbfd;
            }
            QWidget#summaryCard[tone="token_ready"] {
                border-left: 4px solid #2b73b6;
                background: #f8fbff;
            }
            QWidget#summaryCard[tone="token_pending"] {
                border-left: 4px solid #94a3b8;
                background: #fbfcfd;
            }
            QWidget#summaryCard[tone="items_ready"] {
                border-left: 4px solid #3f8c57;
                background: #f8fcf9;
            }
            QWidget#summaryCard[tone="items_empty"] {
                border-left: 4px solid #a2aeb9;
                background: #fbfcfd;
            }
            QWidget#summaryCard[tone="failed_clear"] {
                border-left: 4px solid #2f8b6b;
                background: #f8fcfa;
            }
            QWidget#summaryCard[tone="failed_alert"] {
                border-left: 4px solid #d35d5d;
                background: #fff8f8;
            }
            QLabel#summaryTitle {
                color: #6a7887;
                font-size: 11px;
                font-weight: 700;
                background: transparent;
            }
            QLabel#summaryValue {
                color: #11324d;
                font-size: 18px;
                font-weight: 800;
                background: transparent;
            }
            QLabel#summaryHint {
                color: #92a0ae;
                font-size: 11px;
                background: transparent;
            }
            QLabel#heroTitle {
                color: #ffffff;
                font-size: 20px;
                font-weight: 800;
                background: transparent;
            }
            QLabel#heroMeta {
                color: rgba(236, 244, 249, 0.86);
                font-size: 10px;
                background: transparent;
            }
            QLabel#fieldValue {
                min-height: 32px;
                background: #f7fafb;
                color: #24384d;
                border: 1px solid #dbe4ea;
                border-radius: 10px;
                padding: 6px 12px;
            }
            QLabel#pageTitle {
                color: #102a43;
                font-size: 20px;
                font-weight: 800;
                background: transparent;
            }
            QLabel#pageIntro {
                color: #64778a;
                font-size: 12px;
                background: transparent;
            }
            QLabel#stepHint {
                color: #6a7d8f;
                font-size: 11px;
                background: transparent;
            }
            QLabel#tableHint {
                color: #7c8d9d;
                font-size: 11px;
                background: transparent;
            }
            QLabel#detailLabel {
                color: #728294;
                font-size: 11px;
                font-weight: 700;
                background: transparent;
            }
            QLabel#detailValue {
                min-height: 28px;
                color: #1f3548;
                font-size: 13px;
                font-weight: 700;
                background: #f7fafc;
                border: 1px solid #dbe4ea;
                border-radius: 10px;
                padding: 6px 10px;
            }
            QLabel#detailStatusBadge {
                min-height: 28px;
                padding: 0 12px;
                border-radius: 14px;
                font-size: 12px;
                font-weight: 800;
                color: #49657d;
                background: #eef4f8;
            }
            QLabel#detailStatusBadge[tone="success"] {
                color: #1e7a57;
                background: #e8f7ef;
            }
            QLabel#detailStatusBadge[tone="failure"] {
                color: #b54747;
                background: #fdeeee;
            }
            QLabel#detailStatusBadge[tone="running"] {
                color: #49657d;
                background: #eef4f8;
            }
            QLabel {
                background: transparent;
            }
            QPushButton {
                min-height: 32px;
                padding: 4px 12px;
                border: 1px solid #ccd7e0;
                border-radius: 10px;
                background: #ffffff;
                color: #17324d;
                font-weight: 700;
            }
            QPushButton:hover {
                background: #f4f8fa;
                border-color: #87a1b7;
            }
            QPushButton#primaryButton {
                background: #0d6f68;
                color: #ffffff;
                border-color: #0d6f68;
            }
            QPushButton#primaryButton:hover {
                background: #11857c;
                border-color: #11857c;
            }
            QLineEdit, QPlainTextEdit, QComboBox {
                border: 1px solid #d2dce4;
                border-radius: 10px;
                background: #ffffff;
                color: #24384d;
                selection-background-color: #cae4df;
            }
            QLineEdit, QComboBox {
                min-height: 32px;
                padding: 0 10px;
            }
            QPlainTextEdit {
                padding: 8px 10px;
            }
            QLineEdit:read-only, QPlainTextEdit:read-only {
                background: #f8fafb;
                color: #3a4f63;
            }
            QComboBox::drop-down {
                width: 28px;
                border: 0;
            }
            QComboBox::down-arrow {
                width: 10px;
                height: 10px;
            }
            QTableWidget {
                border: 1px solid #dbe4ea;
                border-radius: 12px;
                background: #ffffff;
                gridline-color: #edf2f5;
                alternate-background-color: #f8fbfc;
            }
            QHeaderView::section {
                background: #edf2f5;
                padding: 9px 8px;
                border: 0;
                border-right: 1px solid #dde5eb;
                color: #4f6274;
                font-weight: 700;
            }
            QTableWidget::item {
                padding: 6px 8px;
            }
            QPushButton#tableGhostButton {
                min-height: 26px;
                padding: 2px 10px;
                border: 1px solid #d2dce4;
                border-radius: 8px;
                background: #ffffff;
                color: #33536b;
                font-weight: 700;
            }
            QPushButton#tableGhostButton:hover {
                background: #f1f6f9;
                border-color: #8fa5b8;
            }
            QPlainTextEdit#resultConsole {
                border: 1px solid #153549;
                border-radius: 14px;
                background: #102735;
                color: #d8e6ef;
                selection-background-color: #325f74;
                font-family: "Menlo", "Consolas", "Monaco", "Courier New";
            }
            QPlainTextEdit#logConsole {
                border: 1px solid #d7e0e8;
                border-radius: 14px;
                background: #f4f7fa;
                color: #24384d;
                selection-background-color: #dbe8f2;
                font-family: "Menlo", "Consolas", "Monaco", "Courier New";
            }
            QScrollArea {
                border: 0;
                background: transparent;
            }
            QScrollArea > QWidget > QWidget {
                background: transparent;
            }
            """
        )

        self.summary_labels: dict[str, object] = {}
        self.summary_cards: dict[str, object] = {}
        self.settings_inputs: dict[str, object] = {}
        self.diagnostics_table = None
        self.diagnostics_status_label = None
        self.log_error_toggle = None
        self.log_filter_input = None
        self.log_view = None
        self.primary_action_button = None
        self.runtime_status_label = None
        self.tabs = None
        self.recent_files_table = None
        self.device_table = None
        self.recent_file_rows: list[dict] = []
        self.recent_state_rows: list[dict] = []
        self.recent_files_page = 0
        self.recent_files_page_size = 20
        self.recent_files_pager_label = None
        self.test_inputs: dict[str, object] = {}
        self.test_advanced_row_widgets: list[object] = []
        self.test_advanced_toggle_button = None
        self.test_result_view = None
        self.plugin_select = None
        self.tray_icon = None
        self._parse_notice_boxes: list[object] = []
        self._result_dialogs: list[object] = []
        self._allow_close = False
        self._notified_results: set[str] = set()
        self._plugin_options: list[dict] = []
        self._test_tools_unlocked = False
        self._test_tab_widget = None
        self._test_tab_index: int | None = None
        self._runtime_starting = False
        self._runtime_stopping = False

        self._build()
        self._setup_tray()
        self._setup_config_watcher()
        self._setup_refresh_timer()
        self.refresh_all()

    def show(self) -> None:
        self.window.show()
        self.window.raise_()
        self.window.activateWindow()

    def _build(self) -> None:
        tabs = self.qt["QTabWidget"]()
        self.tabs = tabs
        tabs.addTab(self._build_status_tab(), "首页")
        tabs.addTab(self._build_connection_settings_tab(), "注册")
        tabs.addTab(self._build_log_settings_tab(), "日志")
        tabs.addTab(self._build_diagnostics_settings_tab(), "网络诊断")
        self.window.setCentralWidget(tabs)
        self.window.closeEvent = self._handle_close_event
        self._setup_internal_tools_shortcut()

    def _build_status_tab(self):
        widget = self.qt["QWidget"]()
        layout = self.qt["QVBoxLayout"](widget)
        layout.setContentsMargins(24, 18, 24, 18)
        layout.setSpacing(10)

        layout.addWidget(self._build_hero_panel())
        layout.addWidget(self._build_status_strip())
        layout.addWidget(self._build_device_panel())
        layout.addWidget(self._build_recent_panel(), 1)
        return self._wrap_page(widget)

    def _build_hero_panel(self):
        panel = self.qt["QWidget"]()
        panel.setObjectName("heroPanel")
        panel.setMinimumHeight(84)
        panel.setMaximumHeight(92)
        layout = self.qt["QHBoxLayout"](panel)
        layout.setContentsMargins(20, 12, 20, 12)
        layout.setSpacing(10)

        info_column = self.qt["QVBoxLayout"]()
        info_column.setSpacing(4)
        title = self.qt["QLabel"]("NetStar Parse Hub")
        title.setObjectName("heroTitle")
        meta = self.qt["QLabel"]("文件采集与解析工作站")
        meta.setObjectName("heroMeta")
        meta.setWordWrap(True)
        info_column.addWidget(title)
        info_column.addWidget(meta)

        action_column = self.qt["QVBoxLayout"]()
        action_column.setSpacing(6)
        action_column.addStretch(1)
        start_button = self.qt["QPushButton"]("启动工作站")
        start_button.setObjectName("primaryButton")
        start_button.setMinimumHeight(30)
        start_button.clicked.connect(self.toggle_workstation)
        start_button.setMinimumWidth(120)
        action_column.addWidget(start_button, 0, self.qt["Qt"].AlignRight)
        self.primary_action_button = start_button
        action_column.addStretch(1)

        layout.addLayout(info_column, 1)
        layout.addLayout(action_column)
        return panel

    def _build_status_strip(self):
        panel = self.qt["QWidget"]()
        panel.setObjectName("statusStrip")
        row = self.qt["QHBoxLayout"](panel)
        row.setContentsMargins(10, 8, 10, 8)
        row.setSpacing(8)
        for key, title in [
            ("runtime", "运行状态"),
            ("items", "设备数"),
            ("failed", "异常文件"),
        ]:
            row.addWidget(self._build_summary_card(key, title))
        return panel

    def _build_summary_card(self, key: str, title: str):
        box = self.qt["QWidget"]()
        box.setObjectName("summaryCard")
        row = self.qt["QVBoxLayout"](box)
        row.setContentsMargins(10, 6, 10, 6)
        row.setSpacing(1)

        card_title = self.qt["QLabel"](title)
        card_title.setObjectName("summaryTitle")
        card_title.setAlignment(self.qt["Qt"].AlignLeft)
        value = self.qt["QLabel"]("-")
        value.setObjectName("summaryValue")
        value.setAlignment(self.qt["Qt"].AlignLeft)
        value.setMinimumHeight(24)
        hint = self.qt["QLabel"](" ")
        hint.setObjectName("summaryHint")
        hint.setAlignment(self.qt["Qt"].AlignLeft)

        row.addWidget(card_title)
        row.addWidget(value)
        row.addWidget(hint)

        self.summary_labels[key] = value
        self.summary_labels[f"{key}_hint"] = hint
        self.summary_cards[key] = box
        if key == "runtime":
            self.runtime_status_label = value
        return box

    def _build_recent_panel(self):
        group = self.qt["QGroupBox"]("文件记录")
        layout = self.qt["QVBoxLayout"](group)
        layout.setContentsMargins(4, 2, 4, 4)
        layout.setSpacing(4)
        action_row = self.qt["QHBoxLayout"]()
        action_row.setContentsMargins(0, 0, 0, 0)
        action_row.setSpacing(6)
        clear_failed_button = self.qt["QPushButton"]("清空异常")
        clear_failed_button.setMinimumHeight(28)
        clear_failed_button.clicked.connect(self.clear_failed_state_records)
        clear_recent_button = self.qt["QPushButton"]("重置")
        clear_recent_button.setMinimumHeight(28)
        clear_recent_button.clicked.connect(self.clear_recent_state_records)
        action_row.addWidget(clear_failed_button)
        action_row.addWidget(clear_recent_button)
        action_row.addStretch(1)
        prev_button = self.qt["QPushButton"]("上一页")
        prev_button.setMinimumHeight(28)
        prev_button.clicked.connect(self.show_recent_files_prev_page)
        pager_label = self.qt["QLabel"]("第 1 / 1 页")
        pager_label.setObjectName("tableHint")
        next_button = self.qt["QPushButton"]("下一页")
        next_button.setMinimumHeight(28)
        next_button.clicked.connect(self.show_recent_files_next_page)
        self.recent_files_pager_label = pager_label
        action_row.addWidget(prev_button)
        action_row.addWidget(pager_label)
        action_row.addWidget(next_button)
        layout.addLayout(action_row)
        self.recent_files_table = self.qt["QTableWidget"](0, 6)
        self.recent_files_table.setHorizontalHeaderLabels(["目录", "文件名", "大小", "状态", "上传时间", "操作"])
        self._configure_table(self.recent_files_table)
        header = self.recent_files_table.horizontalHeader()
        header.setSectionsMovable(True)
        header.setSectionResizeMode(0, self.qt["HEADER_STRETCH"])
        header.setSectionResizeMode(1, self.qt["HEADER_INTERACTIVE"])
        header.setSectionResizeMode(2, self.qt["HEADER_INTERACTIVE"])
        header.setSectionResizeMode(3, self.qt["HEADER_INTERACTIVE"])
        header.setSectionResizeMode(4, self.qt["HEADER_INTERACTIVE"])
        header.setSectionResizeMode(5, self.qt["HEADER_RESIZE_TO_CONTENTS"])
        self.recent_files_table.setColumnWidth(1, 260)
        self.recent_files_table.setColumnWidth(2, 110)
        self.recent_files_table.setColumnWidth(3, 120)
        self.recent_files_table.setColumnWidth(4, 180)
        self.recent_files_table.setColumnWidth(5, 92)
        layout.addWidget(self.recent_files_table)
        return group

    def _build_device_panel(self):
        group = self.qt["QGroupBox"]("监控设备")
        group.setMaximumHeight(170)
        layout = self.qt["QVBoxLayout"](group)
        layout.setContentsMargins(4, 2, 4, 4)
        layout.setSpacing(4)
        self.device_table = self.qt["QTableWidget"](0, 7)
        self.device_table.setHorizontalHeaderLabels(
            ["设备 ID", "设备编码", "设备名称", "监听目录", "文件类型", "稳定秒数", "启用"]
        )
        self._configure_table(self.device_table)
        self.device_table.setMinimumHeight(110)
        header = self.device_table.horizontalHeader()
        header.setSectionsMovable(True)
        header.setSectionResizeMode(0, self.qt["HEADER_RESIZE_TO_CONTENTS"])
        header.setSectionResizeMode(1, self.qt["HEADER_INTERACTIVE"])
        header.setSectionResizeMode(2, self.qt["HEADER_INTERACTIVE"])
        header.setSectionResizeMode(3, self.qt["HEADER_STRETCH"])
        header.setSectionResizeMode(4, self.qt["HEADER_RESIZE_TO_CONTENTS"])
        header.setSectionResizeMode(5, self.qt["HEADER_RESIZE_TO_CONTENTS"])
        header.setSectionResizeMode(6, self.qt["HEADER_RESIZE_TO_CONTENTS"])
        self.device_table.setColumnWidth(1, 150)
        self.device_table.setColumnWidth(2, 180)
        layout.addWidget(self.device_table)
        return group

    def _build_connection_settings_tab(self):
        widget = self.qt["QWidget"]()
        layout = self.qt["QVBoxLayout"](widget)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(16)

        access_group = self.qt["QGroupBox"]("接入配置")
        access_group.setProperty("panelRole", "settings")
        access_form = self.qt["QFormLayout"](access_group)
        self._configure_form_layout(access_form, wide=True)
        for key, label, editable in [
            ("apiBaseUrl", "平台地址", True),
            ("ip", "IP 地址", True),
        ]:
            if editable:
                widget_value = self.qt["QLineEdit"]()
                self._set_expanding(widget_value)
            else:
                widget_value = self.qt["QLabel"]("-")
                widget_value.setObjectName("fieldValue")
                widget_value.setWordWrap(True)
                widget_value.setTextInteractionFlags(self.qt["Qt"].TextSelectableByMouse)
                self._set_expanding(widget_value)
            self.settings_inputs[key] = widget_value
            access_form.addRow(label, widget_value)
        layout.addWidget(access_group)

        info_group = self.qt["QGroupBox"]("工作站信息")
        info_group.setProperty("panelRole", "settings")
        info_form = self.qt["QFormLayout"](info_group)
        self._configure_form_layout(info_form, wide=True)
        for key, label, editable in [
            ("mac", "MAC", False),
            ("hostname", "主机名", False),
            ("items", "监听目录数", False),
        ]:
            if editable:
                widget_value = self.qt["QLineEdit"]()
                self._set_expanding(widget_value)
            else:
                widget_value = self.qt["QLabel"]("-")
                widget_value.setObjectName("fieldValue")
                widget_value.setWordWrap(True)
                widget_value.setTextInteractionFlags(self.qt["Qt"].TextSelectableByMouse)
                self._set_expanding(widget_value)
            self.settings_inputs[key] = widget_value
            info_form.addRow(label, widget_value)
        layout.addWidget(info_group)

        button_row = self.qt["QHBoxLayout"]()
        reload_button = self.qt["QPushButton"]("重新载入")
        reload_button.clicked.connect(self.refresh_settings)
        save_button = self.qt["QPushButton"]("保存")
        save_button.setObjectName("primaryButton")
        save_button.clicked.connect(self.save_settings)
        button_row.addWidget(reload_button)
        button_row.addWidget(save_button)
        button_row.addStretch(1)
        layout.addLayout(button_row)
        layout.addStretch(1)
        return self._wrap_page(widget)

    def _build_test_tab(self):
        widget = self.qt["QWidget"]()
        layout = self.qt["QVBoxLayout"](widget)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(16)

        grid = self.qt["QGridLayout"]()
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setHorizontalSpacing(16)
        grid.setVerticalSpacing(16)
        grid.addWidget(self._build_test_plugin_group(), 0, 0)
        grid.addWidget(self._build_test_config_group(), 0, 1)
        grid.addWidget(self._build_test_upload_group(), 1, 0)
        grid.addWidget(self._build_test_result_group(), 1, 1)
        grid.setColumnStretch(0, 1)
        grid.setColumnStretch(1, 1)
        layout.addLayout(grid)
        layout.addStretch(1)
        return self._wrap_page(widget)

    def _build_test_config_group(self):
        group = self.qt["QGroupBox"]("2 绑定设备")
        group.setProperty("panelRole", "step")
        group.setMinimumHeight(320)
        layout = self.qt["QVBoxLayout"](group)
        layout.setSpacing(12)
        note = self.qt["QLabel"]("选择插件并生成监听配置。")
        note.setObjectName("stepHint")
        note.setWordWrap(True)
        layout.addWidget(note)
        advanced_toggle = self.qt["QPushButton"]("显示高级配置")
        advanced_toggle.setCheckable(True)
        advanced_toggle.clicked.connect(self.toggle_test_advanced_fields)
        self.test_advanced_toggle_button = advanced_toggle
        layout.addWidget(advanced_toggle, 0, self.qt["Qt"].AlignLeft)
        form = self.qt["QFormLayout"]()
        self._configure_form_layout(form)
        plugin_select = self.qt["QComboBox"]()
        plugin_select.addItem("请选择插件", "")
        self._set_expanding(plugin_select)
        plugin_select.currentIndexChanged.connect(self._plugin_selection_changed)
        self.plugin_select = plugin_select
        self.test_inputs["pluginSelector"] = plugin_select
        form.addRow("插件", plugin_select)

        for key, label, default in [
            ("deviceId", "设备 ID", ""),
            ("deviceCode", "设备编码", ""),
            ("deviceName", "设备名称", ""),
            ("watchPath", "监听目录", ""),
        ]:
            field = self._build_test_line_edit()
            field.setText(default)
            self.test_inputs[key] = field
            form.addRow(label, field)
        for key, label, default in [
            ("fileType", "文件类型", "csv"),
            ("stableSeconds", "稳定秒数", "2"),
        ]:
            field = self._build_test_line_edit()
            field.setText(default)
            self.test_inputs[key] = field
            label_widget = self.qt["QLabel"](label)
            form.addRow(label_widget, field)
            self.test_advanced_row_widgets.extend([label_widget, field])
        layout.addLayout(form)

        button_row = self.qt["QHBoxLayout"]()
        refresh_button = self.qt["QPushButton"]("刷新插件列表")
        refresh_button.clicked.connect(self.refresh_test_plugins)
        clear_button = self.qt["QPushButton"]("清空")
        clear_button.clicked.connect(self.clear_test_binding_form)
        save_button = self.qt["QPushButton"]("保存并推送")
        save_button.setObjectName("primaryButton")
        save_button.clicked.connect(self.save_and_push_test_binding_config)
        button_row.addWidget(refresh_button)
        button_row.addWidget(clear_button)
        button_row.addWidget(save_button)
        button_row.addStretch(1)
        layout.addLayout(button_row)
        layout.addStretch(1)
        return group

    def _build_test_plugin_group(self):
        group = self.qt["QGroupBox"]("1 上传插件")
        group.setProperty("panelRole", "step")
        group.setMinimumHeight(320)
        layout = self.qt["QVBoxLayout"](group)
        layout.setSpacing(12)
        note = self.qt["QLabel"]("从插件包自动读取展示信息和结果示例。")
        note.setObjectName("stepHint")
        note.setWordWrap(True)
        layout.addWidget(note)
        form = self.qt["QFormLayout"]()
        self._configure_form_layout(form)
        zip_path = self._build_test_line_edit()
        plugin_name = self._build_readonly_line_edit()
        display_name = self._build_readonly_line_edit()
        result_description = self._build_readonly_line_edit()
        result_example = self.qt["QPlainTextEdit"]()
        result_example.setPlaceholderText('{"sampleNo":"S001","items":[{"L":97.71}]}')
        result_example.setMinimumHeight(112)
        result_example.setMaximumHeight(112)
        result_example.setReadOnly(True)
        result_example.setSizePolicy(
            self.qt["QSizePolicy"](
                self.qt["SIZE_EXPANDING"],
                self.qt["SIZE_FIXED"],
            )
        )
        self.test_inputs["pluginZipPath"] = zip_path
        self.test_inputs["pluginName"] = plugin_name
        self.test_inputs["pluginDisplayName"] = display_name
        self.test_inputs["pluginResultDescription"] = result_description
        self.test_inputs["pluginResultExample"] = result_example

        zip_row_widget = self.qt["QWidget"]()
        zip_row = self.qt["QHBoxLayout"](zip_row_widget)
        zip_row.setContentsMargins(0, 0, 0, 0)
        zip_row.setSpacing(8)
        zip_row.addWidget(zip_path)
        browse_button = self.qt["QPushButton"]("选择 zip")
        browse_button.setMinimumWidth(88)
        browse_button.clicked.connect(self.browse_plugin_zip)
        zip_row.addWidget(browse_button)
        self._set_expanding(zip_row_widget)
        form.addRow("插件 zip", zip_row_widget)
        form.addRow("技术名", plugin_name)
        form.addRow("展示名", display_name)
        form.addRow("结果说明", result_description)
        form.addRow("结果示例", result_example)
        layout.addLayout(form)

        button_row = self.qt["QHBoxLayout"]()
        clear_button = self.qt["QPushButton"]("清空")
        clear_button.clicked.connect(self.clear_test_plugin_form)
        upload_button = self.qt["QPushButton"]("上传插件库")
        upload_button.setObjectName("primaryButton")
        upload_button.clicked.connect(self.upload_test_plugin)
        enable_button = self.qt["QPushButton"]("启用插件")
        enable_button.clicked.connect(self.enable_test_plugin)
        button_row.addWidget(clear_button)
        button_row.addWidget(upload_button)
        button_row.addWidget(enable_button)
        button_row.addStretch(1)
        layout.addLayout(button_row)
        layout.addStretch(1)
        return group

    def _build_test_upload_group(self):
        group = self.qt["QGroupBox"]("3 上传测试文件")
        group.setProperty("panelRole", "step")
        group.setMinimumHeight(220)
        layout = self.qt["QVBoxLayout"](group)
        layout.setSpacing(12)
        note = self.qt["QLabel"]("上传测试文件并触发完整解析链路。")
        note.setObjectName("stepHint")
        note.setWordWrap(True)
        layout.addWidget(note)
        form = self.qt["QFormLayout"]()
        self._configure_form_layout(form)
        device_field = self._build_test_line_edit()
        device_field.setText("")
        file_field = self._build_test_line_edit()
        self.test_inputs["uploadDeviceId"] = device_field
        self.test_inputs["uploadFilePath"] = file_field
        file_row_widget = self.qt["QWidget"]()
        file_row = self.qt["QHBoxLayout"](file_row_widget)
        file_row.setContentsMargins(0, 0, 0, 0)
        file_row.setSpacing(8)
        file_row.addWidget(file_field)
        browse_button = self.qt["QPushButton"]("选择文件")
        browse_button.setMinimumWidth(88)
        browse_button.clicked.connect(self.browse_test_upload_file)
        file_row.addWidget(browse_button)
        self._set_expanding(file_row_widget)
        form.addRow("设备 ID", device_field)
        form.addRow("测试文件", file_row_widget)
        layout.addLayout(form)

        button_row = self.qt["QHBoxLayout"]()
        clear_button = self.qt["QPushButton"]("清空")
        clear_button.clicked.connect(self.clear_test_upload_form)
        upload_button = self.qt["QPushButton"]("上传测试文件")
        upload_button.setObjectName("primaryButton")
        upload_button.clicked.connect(self.upload_test_file)
        button_row.addWidget(clear_button)
        button_row.addWidget(upload_button)
        button_row.addStretch(1)
        layout.addLayout(button_row)
        layout.addStretch(1)
        return group

    def _build_test_result_group(self):
        group = self.qt["QGroupBox"]("4 查看结果")
        group.setProperty("panelRole", "step")
        group.setMinimumHeight(220)
        layout = self.qt["QVBoxLayout"](group)
        layout.setSpacing(12)
        note = self.qt["QLabel"]("这里显示请求回包和解析结果。")
        note.setObjectName("stepHint")
        note.setWordWrap(True)
        layout.addWidget(note)
        self.test_result_view = self.qt["QPlainTextEdit"]()
        self.test_result_view.setObjectName("resultConsole")
        self.test_result_view.setReadOnly(True)
        self.test_result_view.setLineWrapMode(self.qt["NO_LINE_WRAP"])
        self.test_result_view.setMinimumHeight(150)
        layout.addWidget(self.test_result_view)
        action_row = self.qt["QHBoxLayout"]()
        clear_result_button = self.qt["QPushButton"]("清空结果")
        clear_result_button.clicked.connect(lambda: self.test_result_view.setPlainText(""))
        reset_form_button = self.qt["QPushButton"]("重置测试表单")
        reset_form_button.clicked.connect(self.reset_test_forms)
        clear_recent_button = self.qt["QPushButton"]("清空最近记录")
        clear_recent_button.clicked.connect(self.clear_recent_state_records)
        action_row.addWidget(clear_result_button)
        action_row.addWidget(reset_form_button)
        action_row.addWidget(clear_recent_button)
        action_row.addStretch(1)
        layout.addLayout(action_row)
        return group

    def _build_test_line_edit(self):
        field = self.qt["QLineEdit"]()
        self._set_expanding(field)
        return field

    def _build_readonly_line_edit(self):
        field = self._build_test_line_edit()
        field.setReadOnly(True)
        return field

    def _configure_form_layout(self, form, wide: bool = False) -> None:
        form.setContentsMargins(8, 6, 8, 6)
        form.setHorizontalSpacing(16 if wide else 14)
        form.setVerticalSpacing(10)
        form.setLabelAlignment(self.qt["Qt"].AlignLeft | self.qt["Qt"].AlignVCenter)
        form.setFormAlignment(self.qt["Qt"].AlignTop)
        form.setFieldGrowthPolicy(self.qt["FORM_GROW_ALL_NON_FIXED"])
        form.setRowWrapPolicy(self.qt["FORM_DONT_WRAP_ROWS"])

    def _set_expanding(self, widget) -> None:
        policy = self.qt["QSizePolicy"](
            self.qt["SIZE_EXPANDING"],
            self.qt["SIZE_FIXED"],
        )
        widget.setSizePolicy(policy)

    def _wrap_page(self, content):
        scroll = self.qt["QScrollArea"]()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(self.qt["FRAME_NO_FRAME"])
        scroll.setHorizontalScrollBarPolicy(self.qt["Qt"].ScrollBarAlwaysOff)
        scroll.setWidget(content)
        return scroll

    def _build_log_settings_tab(self):
        widget = self.qt["QWidget"]()
        layout = self.qt["QVBoxLayout"](widget)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(16)
        log_group = self.qt["QGroupBox"]("详细日志")
        log_layout = self.qt["QVBoxLayout"](log_group)

        path_label = self.qt["QLabel"](str(self.log_file))
        path_label.setObjectName("fieldValue")
        path_label.setWordWrap(True)
        path_label.setTextInteractionFlags(self.qt["Qt"].TextSelectableByMouse)
        log_layout.addWidget(path_label)

        button_row = self.qt["QHBoxLayout"]()
        filter_input = self.qt["QLineEdit"]()
        filter_input.setPlaceholderText("筛选日志关键字")
        filter_input.textChanged.connect(lambda *_: self.refresh_log_view())
        self.log_filter_input = filter_input
        error_toggle = self.qt["QPushButton"]("只看错误")
        error_toggle.setCheckable(True)
        error_toggle.toggled.connect(lambda *_: self.refresh_log_view())
        self.log_error_toggle = error_toggle
        open_button = self.qt["QPushButton"]("打开日志目录")
        open_button.clicked.connect(self.open_log_folder)
        button_row.addWidget(filter_input, 1)
        button_row.addWidget(error_toggle)
        button_row.addWidget(open_button)
        log_layout.addLayout(button_row)

        self.log_view = self.qt["QPlainTextEdit"]()
        self.log_view.setObjectName("logConsole")
        self.log_view.setReadOnly(True)
        self.log_view.setLineWrapMode(self.qt["NO_LINE_WRAP"])
        log_layout.addWidget(self.log_view)

        layout.addWidget(log_group)
        layout.setStretch(0, 1)
        return self._wrap_page(widget)

    def _build_diagnostics_settings_tab(self):
        widget = self.qt["QWidget"]()
        layout = self.qt["QVBoxLayout"](widget)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(16)
        diagnostics_group = self.qt["QGroupBox"]("网络诊断")
        diagnostics_group.setProperty("panelRole", "settings")
        diagnostics_layout = self.qt["QVBoxLayout"](diagnostics_group)

        self.diagnostics_status_label = self.qt["QLabel"]("未检查")
        self.diagnostics_status_label.setObjectName("fieldValue")
        diagnostics_layout.addWidget(self.diagnostics_status_label)

        button_row = self.qt["QHBoxLayout"]()
        run_button = self.qt["QPushButton"]("诊断")
        run_button.setObjectName("primaryButton")
        run_button.clicked.connect(lambda: self.run_diagnostics(check_network=True))
        button_row.addWidget(run_button)
        button_row.addStretch(1)
        diagnostics_layout.addLayout(button_row)

        self.diagnostics_table = self.qt["QTableWidget"](0, 3)
        self.diagnostics_table.setHorizontalHeaderLabels(["检查项", "状态", "说明"])
        self._configure_table(self.diagnostics_table)
        diagnostics_layout.addWidget(self.diagnostics_table)

        layout.addWidget(diagnostics_group)
        layout.setStretch(0, 1)
        return self._wrap_page(widget)

    def _setup_tray(self) -> None:
        if not self.qt["QSystemTrayIcon"].isSystemTrayAvailable():
            return
        tray = self.qt["QSystemTrayIcon"](self.window)
        tray.setToolTip("NetStar Parse Hub")
        icon_path = _app_icon_path()
        if icon_path is not None:
            tray.setIcon(self.qt["QIcon"](str(icon_path)))
        else:
            tray.setIcon(self.window.style().standardIcon(self.qt["STYLE_COMPUTER_ICON"]))
        menu = self.qt["QMenu"]()
        open_action = self.qt["QAction"]("打开", self.window)
        open_action.triggered.connect(self.show)
        start_action = self.qt["QAction"]("启动监听", self.window)
        start_action.triggered.connect(self.start_runtime)
        stop_action = self.qt["QAction"]("停止监听", self.window)
        stop_action.triggered.connect(self.stop_runtime)
        logs_action = self.qt["QAction"]("打开日志目录", self.window)
        logs_action.triggered.connect(self.open_log_folder)
        exit_action = self.qt["QAction"]("退出", self.window)
        exit_action.triggered.connect(self.exit_application)
        for action in (open_action, start_action, stop_action, logs_action):
            menu.addAction(action)
        menu.addSeparator()
        menu.addAction(exit_action)
        tray.setContextMenu(menu)
        tray.activated.connect(self._tray_activated)
        tray.show()
        self.tray_icon = tray

    def _setup_refresh_timer(self) -> None:
        timer = self.qt["QTimer"](self.window)
        timer.setInterval(5000)
        timer.timeout.connect(self._timer_refresh)
        timer.start()
        self.refresh_timer = timer

    def _setup_config_watcher(self) -> None:
        watcher = self.qt["QFileSystemWatcher"](self.window)
        watcher.addPath(str(self.config_path))
        watcher.fileChanged.connect(self._config_file_changed)
        self.config_watcher = watcher

    def _config_file_changed(self, path: str) -> None:
        if self.config_path.exists():
            watched_paths = set(self.config_watcher.files())
            if path not in watched_paths:
                self.config_watcher.addPath(str(self.config_path))
        self.refresh_status()
        self.refresh_settings()

    def _timer_refresh(self) -> None:
        try:
            self.refresh_status()
            self.refresh_state()
            self.refresh_log_view()
        except Exception:
            logger.exception("GUI timer refresh failed")

    def _tray_activated(self, reason) -> None:
        if reason == self.qt["TRAY_DOUBLE_CLICK"]:
            self.show()

    def _handle_close_event(self, event) -> None:
        if self._allow_close:
            event.accept()
            return
        event.ignore()
        self.window.hide()
        if self.tray_icon is not None:
            self.tray_icon.showMessage(
                "NetStar",
                "工作站仍在系统托盘运行。",
                self.qt["TRAY_INFO"],
                3000,
            )

    def refresh_all(self) -> None:
        self.refresh_status()
        self.refresh_settings()
        self.refresh_state()
        self.refresh_log_view()
        self.run_diagnostics(check_network=False, show_message=False)
        if self._test_tools_unlocked:
            self._try_refresh_test_plugins()
            self.toggle_test_advanced_fields(False)

    def refresh_status(self) -> None:
        try:
            config = load_config(self.config_path)
        except Exception as exc:
            self._show_error(exc)
            return

        if self._runtime_starting:
            runtime_text = "正在启动"
        elif self._runtime_stopping:
            runtime_text = "正在停止"
        else:
            runtime_text = "运行中" if self.process is not None else "已停止"
        items_count = len(config.items)

        self.summary_labels["runtime"].setText(runtime_text)
        self.summary_labels["runtime_hint"].setText("后台监听和上传进程")
        self._set_summary_card_tone("runtime", "runtime_running" if self.process is not None else "runtime_stopped")
        self.summary_labels["items"].setText(str(items_count))
        self.summary_labels["items_hint"].setText("当前启用监控设备")
        self._set_summary_card_tone("items", "items_ready" if items_count else "items_empty")
        self._refresh_device_items(config.items)

        if self.primary_action_button is not None:
            if self._runtime_starting:
                self.primary_action_button.setText("正在启动...")
            elif self._runtime_stopping:
                self.primary_action_button.setText("正在停止...")
            else:
                self.primary_action_button.setText("停止工作站" if self.process is not None else "启动工作站")
            self.primary_action_button.setEnabled(not self._runtime_starting and not self._runtime_stopping)

    def refresh_settings(self) -> None:
        try:
            config = load_config(self.config_path)
        except Exception as exc:
            self._show_error(exc)
            return
        values = {
            "apiBaseUrl": config.api_base_url,
            "mac": config.mac,
            "ip": config.ip or "",
            "hostname": config.hostname or "",
            "items": str(len(config.items)),
        }
        for key, value in values.items():
            widget = self.settings_inputs[key]
            if isinstance(widget, self.qt["QLineEdit"]):
                widget.setText(value)
            else:
                widget.setText(value)

    def refresh_state(self) -> None:
        store = StateStore(self.state_db)
        store.init()
        rows = store.list_records(limit=500)
        self.recent_state_rows = rows
        self._notify_new_results(rows)
        self._refresh_file_summary(rows)
        self._refresh_recent_files(rows)

    def _refresh_file_summary(self, rows: list[dict]) -> None:
        failed_count = sum(1 for row in rows if row.get("status") in {"upload_failed", "parse_failed"})
        self.summary_labels["failed"].setText(str(failed_count))
        self.summary_labels["failed_hint"].setText("上传失败 + 解析失败")
        self._set_summary_card_tone("failed", "failed_alert" if failed_count else "failed_clear")

    def _refresh_recent_files(self, rows: list[dict]) -> None:
        if self.recent_files_table is None:
            return
        total = len(rows)
        page_size = max(1, self.recent_files_page_size)
        max_page = max(0, (total - 1) // page_size)
        self.recent_files_page = min(self.recent_files_page, max_page)
        start = self.recent_files_page * page_size
        end = start + page_size
        recent_rows = rows[start:end]
        self.recent_file_rows = recent_rows
        if self.recent_files_pager_label is not None:
            self.recent_files_pager_label.setText(f"第 {self.recent_files_page + 1} / {max_page + 1} 页")
        self.recent_files_table.setRowCount(len(recent_rows))
        for row_index, row in enumerate(recent_rows):
            local_path = row.get("local_path") or ""
            path_obj = Path(local_path) if local_path else None
            self._set_row(
                self.recent_files_table,
                row_index,
                [
                    str(path_obj.parent) if path_obj else "",
                    row.get("file_name") or (path_obj.name if path_obj else ""),
                    row.get("file_size"),
                    self._display_recent_status(row.get("status")),
                    row.get("uploaded_time"),
                    "",
                ],
            )
            self.recent_files_table.setCellWidget(row_index, 5, self._build_recent_actions(row_index))

    def show_recent_files_prev_page(self) -> None:
        if self.recent_files_page <= 0:
            return
        self.recent_files_page -= 1
        self._refresh_recent_files(self.recent_state_rows)

    def show_recent_files_next_page(self) -> None:
        if not self.recent_state_rows:
            return
        max_page = max(0, (len(self.recent_state_rows) - 1) // max(1, self.recent_files_page_size))
        if self.recent_files_page >= max_page:
            return
        self.recent_files_page += 1
        self._refresh_recent_files(self.recent_state_rows)

    def _refresh_device_items(self, items: list[dict]) -> None:
        if self.device_table is None:
            return
        self.device_table.setRowCount(len(items))
        for row_index, item in enumerate(items):
            self._set_row(
                self.device_table,
                row_index,
                [
                    item.get("deviceId"),
                    item.get("deviceCode"),
                    item.get("deviceName"),
                    item.get("watchPath"),
                    item.get("fileType"),
                    item.get("stableSeconds"),
                    "是" if item.get("enabled", True) else "否",
                ],
            )

    def refresh_log_view(self) -> None:
        if self.log_view is None:
            return
        if not self.log_file.exists():
            self.log_view.setPlainText("日志文件暂未生成。")
            return
        try:
            content = self.log_file.read_text(encoding="utf-8", errors="replace")
        except Exception as exc:
            self.log_view.setPlainText(f"读取日志失败：{exc}")
            return
        content = self._filter_log_content(content)
        if len(content) > 20000:
            content = content[-20000:]
        if self.log_view.toPlainText() != content:
            scrollbar = self.log_view.verticalScrollBar()
            was_at_bottom = scrollbar.value() >= scrollbar.maximum() - 4
            self.log_view.setPlainText(content)
            if was_at_bottom:
                scrollbar.setValue(scrollbar.maximum())

    def _notify_new_results(self, rows: list[dict]) -> None:
        for row in rows:
            status = row.get("status")
            data_no = row.get("data_no")
            if status not in {"parse_success", "parse_failed"} or not data_no:
                continue
            key = f"{data_no}:{status}"
            if key in self._notified_results:
                continue
            self._notified_results.add(key)
            file_name = row.get("file_name") or row.get("local_path") or "文件"
            title = f"{file_name}解析完成" if status == "parse_success" else f"{file_name}解析失败"
            message = f"{file_name}已完成解析" if status == "parse_success" else f"{file_name}解析失败，请检查结果"
            icon = (
                self.qt["TRAY_INFO"]
                if status == "parse_success"
                else self.qt["TRAY_WARNING"]
            )
            if self.tray_icon is not None:
                self.tray_icon.showMessage(title, message, icon, 5000)
            self._show_parse_notice(title, message, is_error=status == "parse_failed")

    def _configure_table(self, table) -> None:
        table.setAlternatingRowColors(True)
        table.setSelectionBehavior(self.qt["SELECT_ROWS"])
        table.setSelectionMode(self.qt["SINGLE_SELECTION"])
        table.setWordWrap(False)
        table.verticalHeader().setVisible(False)
        table.verticalHeader().setDefaultSectionSize(40)
        table.setShowGrid(False)
        table.horizontalHeader().setStretchLastSection(True)
        table.horizontalHeader().setSectionResizeMode(self.qt["HEADER_RESIZE_TO_CONTENTS"])

    def _set_row(self, table, row: int, values: list) -> None:
        for column, value in enumerate(values):
            display_text = self._display_value(value)
            item = self.qt["QTableWidgetItem"](display_text)
            item.setFlags(item.flags() & ~self.qt["Qt"].ItemIsEditable)
            if table is self.recent_files_table:
                if column == 2:
                    item.setTextAlignment(self.qt["Qt"].AlignRight | self.qt["Qt"].AlignVCenter)
                elif column in {3, 4}:
                    item.setTextAlignment(self.qt["Qt"].AlignCenter)
                if column == 3:
                    self._style_recent_status_item(item, display_text)
            elif column == 1:
                self._style_generic_status_item(item, display_text)
            if display_text:
                item.setToolTip(display_text)
            table.setItem(row, column, item)

    def _build_recent_actions(self, row_index: int):
        cell = self.qt["QWidget"]()
        layout = self.qt["QHBoxLayout"](cell)
        layout.setContentsMargins(4, 2, 4, 2)
        layout.setSpacing(6)
        result_button = self.qt["QPushButton"]("详情")
        result_button.setObjectName("tableGhostButton")
        result_button.clicked.connect(lambda *_: self.show_recent_result(row_index))
        layout.addWidget(result_button)
        layout.addStretch(1)
        return cell

    def _style_recent_status_item(self, item, text: str) -> None:
        palette = {
            "成功": ("#e8f7ef", "#1e7a57"),
            "失败": ("#fdeeee", "#b54747"),
            "处理中": ("#eef4f8", "#49657d"),
        }
        background, foreground = palette.get(text, ("#f4f7fa", "#526577"))
        item.setBackground(self._make_qcolor(background))
        item.setForeground(self._make_qcolor(foreground))

    def _style_generic_status_item(self, item, text: str) -> None:
        palette = {
            "正常": ("#e8f7ef", "#1e7a57"),
            "成功": ("#e8f7ef", "#1e7a57"),
            "失败": ("#fdeeee", "#b54747"),
            "跳过": ("#f2f5f8", "#677b8c"),
            "处理中": ("#eef4f8", "#49657d"),
        }
        background, foreground = palette.get(text, ("#f4f7fa", "#526577"))
        item.setBackground(self._make_qcolor(background))
        item.setForeground(self._make_qcolor(foreground))

    def _make_qcolor(self, value: str):
        return self.qt["QColor"](value)

    def _filter_log_content(self, content: str) -> str:
        lines = content.splitlines()
        keyword = ""
        if self.log_filter_input is not None:
            keyword = self.log_filter_input.text().strip().lower()
        only_error = bool(self.log_error_toggle is not None and self.log_error_toggle.isChecked())
        filtered_lines: list[str] = []
        for line in lines:
            lower_line = line.lower()
            if only_error and not any(token in lower_line for token in ("error", "failed", "traceback", "exception")):
                continue
            if keyword and keyword not in lower_line:
                continue
            filtered_lines.append(line)
        return "\n".join(filtered_lines)

    def _set_summary_card_tone(self, key: str, tone: str) -> None:
        card = self.summary_cards.get(key)
        if card is None:
            return
        card.setProperty("tone", tone)
        card.style().unpolish(card)
        card.style().polish(card)
        card.update()

    def _display_value(self, value) -> str:
        if value is None:
            return ""
        if isinstance(value, bool):
            return "是" if value else "否"
        if isinstance(value, str):
            formatted = self._format_datetime_text(value)
            if formatted is not None:
                return formatted
            return self._display_status(value)
        return str(value)

    def _format_datetime_text(self, value: str) -> str | None:
        try:
            return datetime.fromisoformat(value).strftime("%Y-%m-%d %H:%M:%S")
        except ValueError:
            return None

    def _display_status(self, status: str | None) -> str:
        labels = {
            "ok": "正常",
            "skipped": "跳过",
            "failed": "失败",
            "uploading": "上传中",
            "uploaded": "已上传",
            "upload_failed": "上传失败",
            "parse_success": "解析成功",
            "parse_failed": "解析失败",
            "pending": "等待处理",
            "running": "处理中",
            "success": "成功",
            "failure": "失败",
        }
        return labels.get(status or "", status or "")

    def _display_recent_status(self, status: str | None) -> str:
        if status == "uploading":
            return "上传中"
        if status == "uploaded":
            return "解析中"
        if status == "parse_success":
            return "成功"
        if status in {"upload_failed", "parse_failed"}:
            return "失败"
        return "处理中"

    def _setup_internal_tools_shortcut(self) -> None:
        shortcut_action = self.qt["QAction"](self.window)
        shortcut_action.setShortcut(self.qt["QKeySequence"]("Ctrl+Shift+T"))
        shortcut_action.triggered.connect(self._prompt_unlock_internal_tools)
        self.window.addAction(shortcut_action)

    def _prompt_unlock_internal_tools(self) -> None:
        if self._test_tools_unlocked:
            self._lock_internal_tools()
            return

        password, accepted = self.qt["QInputDialog"].getText(
            self.window,
            "解锁测试页",
            "请输入实施密码：",
            self.qt["PASSWORD_ECHO"],
        )
        if not accepted:
            return
        if password != INTERNAL_TOOLS_PASSWORD:
            self._show_error(RuntimeError("实施密码错误"))
            return
        self._unlock_internal_tools()

    def _unlock_internal_tools(self) -> None:
        if self.tabs is None:
            return
        if self._test_tab_widget is None:
            self._test_tab_widget = self._build_test_tab()
        if self._test_tab_index is None:
            self._test_tab_index = self.tabs.insertTab(2, self._test_tab_widget, "测试")
        self._test_tools_unlocked = True
        self.tabs.setCurrentIndex(self._test_tab_index)
        self.toggle_test_advanced_fields(False)

    def _lock_internal_tools(self) -> None:
        if self.tabs is not None and self._test_tab_widget is not None:
            tab_index = self.tabs.indexOf(self._test_tab_widget)
            if tab_index >= 0:
                self.tabs.removeTab(tab_index)
        self._test_tab_index = None
        self._test_tools_unlocked = False
        self.reset_test_forms(silent=True)
        if self.tabs is not None:
            self.tabs.setCurrentIndex(0)

    def toggle_test_advanced_fields(self, checked: bool | None = None) -> None:
        is_visible = bool(checked)
        if self.test_advanced_toggle_button is not None and self.test_advanced_toggle_button.isChecked() != is_visible:
            self.test_advanced_toggle_button.setChecked(is_visible)
        if self.test_advanced_toggle_button is not None:
            self.test_advanced_toggle_button.setText("收起高级配置" if is_visible else "显示高级配置")
        for widget in self.test_advanced_row_widgets:
            widget.setVisible(is_visible)

    def reset_test_forms(self, *, silent: bool = False) -> None:
        if self._test_tab_widget is None:
            return
        self.clear_test_plugin_form()
        self.clear_test_binding_form()
        self.clear_test_upload_form()
        if self.test_result_view is not None:
            self.test_result_view.setPlainText("")
        self.toggle_test_advanced_fields(False)
        if not silent:
            self._show_info("测试表单已重置")

    def clear_recent_state_records(self) -> None:
        store = StateStore(self.state_db)
        store.init()
        cleared = store.clear_records()
        self._notified_results.clear()
        self.refresh_state()
        self._show_info(f"最近记录已清空，共移除 {cleared} 条")

    def clear_failed_state_records(self) -> None:
        store = StateStore(self.state_db)
        store.init()
        cleared = store.clear_failed_records()
        self._notified_results.clear()
        self.refresh_state()
        self._show_info(f"异常文件已清空，共移除 {cleared} 条")

    def show_recent_result(self, row_index: int) -> None:
        if row_index >= len(self.recent_file_rows):
            return
        row = self.recent_file_rows[row_index]
        payload = {
            "数据编号": row.get("data_no"),
            "文件名": row.get("file_name"),
            "状态": self._display_recent_status(row.get("status")),
            "上传时间": self._display_value(row.get("uploaded_time")),
            "完成时间": self._display_value(row.get("finished_time")),
            "错误代码": row.get("last_error_code"),
            "错误信息": row.get("last_error_message"),
            "本地路径": row.get("local_path"),
        }
        filtered_payload = {key: value for key, value in payload.items() if value not in (None, "", [])}
        self._show_recent_detail_dialog(filtered_payload)

    def open_recent_file_folder(self, row_index: int) -> None:
        if row_index >= len(self.recent_file_rows):
            return
        local_path = self.recent_file_rows[row_index].get("local_path") or ""
        if not local_path:
            return
        path_obj = Path(local_path)
        target_dir = path_obj.parent if path_obj.parent.exists() else path_obj
        if not str(target_dir):
            return
        opened = self.qt["QDesktopServices"].openUrl(self.qt["QUrl"].fromLocalFile(str(target_dir)))
        if opened:
            return
        process = self.qt["QProcess"](self.window)
        if sys.platform == "darwin":
            process.startDetached("open", [str(target_dir)])
            return
        if sys.platform.startswith("win"):
            process.startDetached("explorer", [str(target_dir)])
            return
        process.startDetached("xdg-open", [str(target_dir)])

    def _show_recent_detail_dialog(self, payload: dict) -> None:
        dialog = self.qt["QWidget"](self.window, self.qt["Qt"].Window)
        dialog.setWindowTitle("文件详情")
        dialog.resize(760, 520)
        dialog.setAttribute(self.qt["WA_DELETE_ON_CLOSE"], True)
        dialog.setStyleSheet(
            """
            QWidget {
                background: #f7fafc;
                color: #203040;
            }
            """
        )
        layout = self.qt["QVBoxLayout"](dialog)
        layout.setContentsMargins(18, 18, 18, 18)
        layout.setSpacing(12)

        title = self.qt["QLabel"]("文件详情")
        title.setObjectName("pageTitle")
        layout.addWidget(title)

        summary_group = self.qt["QGroupBox"]("摘要")
        summary_layout = self.qt["QGridLayout"](summary_group)
        summary_layout.setContentsMargins(8, 8, 8, 8)
        summary_layout.setHorizontalSpacing(12)
        summary_layout.setVerticalSpacing(10)

        file_name = payload.get("文件名", "-")
        status_text = str(payload.get("状态", "处理中"))
        summary_items = [
            ("文件名", file_name),
            ("数据编号", payload.get("数据编号", "-")),
            ("上传时间", payload.get("上传时间", "-")),
            ("完成时间", payload.get("完成时间", "-")),
        ]
        for index, (label_text, value_text) in enumerate(summary_items):
            label_widget = self.qt["QLabel"](label_text)
            label_widget.setObjectName("detailLabel")
            value_widget = self.qt["QLabel"](str(value_text))
            value_widget.setObjectName("detailValue")
            value_widget.setWordWrap(True)
            summary_layout.addWidget(label_widget, index, 0)
            summary_layout.addWidget(value_widget, index, 1)

        status_label = self.qt["QLabel"](status_text)
        status_label.setObjectName("detailStatusBadge")
        status_label.setProperty("tone", self._detail_status_tone(status_text))
        summary_layout.addWidget(self.qt["QLabel"]("状态"), 0, 2)
        summary_layout.itemAtPosition(0, 2).widget().setObjectName("detailLabel")
        summary_layout.addWidget(status_label, 0, 3)
        path_label = self.qt["QLabel"]("本地路径")
        path_label.setObjectName("detailLabel")
        path_value = self.qt["QLabel"](str(payload.get("本地路径", "-")))
        path_value.setObjectName("detailValue")
        path_value.setWordWrap(True)
        summary_layout.addWidget(path_label, 1, 2)
        summary_layout.addWidget(path_value, 1, 3, 3, 1)
        summary_layout.setColumnStretch(1, 1)
        summary_layout.setColumnStretch(3, 1)
        layout.addWidget(summary_group)

        raw_payload = dict(payload)
        for key in ("文件名", "数据编号", "状态", "上传时间", "完成时间", "本地路径"):
            raw_payload.pop(key, None)
        result_view = self.qt["QPlainTextEdit"]()
        result_view.setObjectName("resultConsole")
        result_view.setReadOnly(True)
        result_view.setPlaceholderText("没有额外明细。")
        result_view.setPlainText(json.dumps(raw_payload, ensure_ascii=False, indent=2) if raw_payload else "")
        layout.addWidget(result_view)

        button_row = self.qt["QHBoxLayout"]()
        copy_button = self.qt["QPushButton"]("复制详情")
        copy_button.clicked.connect(lambda: self.qt["QApplication"].clipboard().setText(json.dumps(payload, ensure_ascii=False, indent=2)))
        close_button = self.qt["QPushButton"]("关闭")
        close_button.clicked.connect(dialog.close)
        button_row.addWidget(copy_button)
        button_row.addStretch(1)
        button_row.addWidget(close_button)
        layout.addLayout(button_row)

        self._result_dialogs.append(dialog)

        def cleanup_dialog() -> None:
            if dialog in self._result_dialogs:
                self._result_dialogs.remove(dialog)

        dialog.destroyed.connect(lambda *_: cleanup_dialog())
        dialog.show()
        dialog.raise_()
        dialog.activateWindow()

    def _detail_status_tone(self, status_text: str) -> str:
        if status_text == "成功":
            return "success"
        if status_text == "失败":
            return "failure"
        return "running"

    def register_workstation(self) -> None:
        self._run_action(lambda: register(self.config_path), "工作站已注册")

    def save_settings(self) -> None:
        self._run_action(self._save_settings, "配置已保存")

    def _save_settings(self) -> None:
        existing = load_config(self.config_path)
        update_base_config(
            self.config_path,
            api_base_url=self.settings_inputs["apiBaseUrl"].text(),
            ws_url=None,
            mac=existing.mac,
            ip=self.settings_inputs["ip"].text(),
            hostname=existing.hostname,
            workstation_token=existing.workstation_token,
            heartbeat_interval_seconds=existing.heartbeat_interval_seconds,
        )

    def pull_remote_config(self) -> None:
        self._run_action(lambda: pull_config(self.config_path), "平台配置已更新")

    def toggle_workstation(self) -> None:
        if self.process is not None:
            self.stop_runtime()
            self._show_info("工作站已停止")
            return
        self._run_action(self.start_workstation, "工作站已启动")

    def start_workstation(self) -> None:
        self._save_settings()
        register(self.config_path)
        pull_config(self.config_path)
        self.start_runtime(show_message=False)

    def sync_status(self) -> None:
        from workstation.cli import query_status

        self._run_action(lambda: query_status(self.config_path, self.state_db, []), "解析状态已同步")

    def retry_failed_uploads(self) -> None:
        self._run_action(lambda: asyncio.run(retry_failed(self.config_path, self.state_db)), "失败上传重试完成")

    def browse_plugin_zip(self) -> None:
        file_name, _ = self.qt["QFileDialog"].getOpenFileName(self.window, "选择插件 zip", "", "Zip Files (*.zip)")
        if file_name:
            self.test_inputs["pluginZipPath"].setText(file_name)
            self._load_plugin_manifest_preview(Path(file_name))

    def browse_test_upload_file(self) -> None:
        file_name, _ = self.qt["QFileDialog"].getOpenFileName(self.window, "选择测试文件")
        if file_name:
            self.test_inputs["uploadFilePath"].setText(file_name)

    def save_test_binding_config(self) -> None:
        self._run_action(self._save_test_binding_config, "测试绑定配置已保存")

    def save_and_push_test_binding_config(self) -> None:
        self._run_action(self._save_and_push_test_binding_config, "测试绑定配置已保存并推送")

    def _save_test_binding_config(self) -> None:
        client = self._build_system_client()
        workstation_id = self._require_workstation_id()
        plugin_id = self._selected_plugin_id()
        current = client.get_workstation_config(workstation_id)
        config_version = current.get("configVersion")
        new_item = {
            "deviceId": self._test_int("deviceId"),
            "deviceCode": self.test_inputs["deviceCode"].text().strip(),
            "deviceName": self.test_inputs["deviceName"].text().strip(),
            "pluginId": plugin_id,
            "watchPath": self.test_inputs["watchPath"].text().strip(),
            "fileType": self.test_inputs["fileType"].text().strip(),
            "stableSeconds": self._test_int("stableSeconds"),
            "enabled": True,
        }
        items = list(current.get("items") or [])
        deduped_items = [
            item
            for item in items
            if not (
                str(item.get("deviceId")) == str(new_item["deviceId"])
                and str(item.get("watchPath") or "").strip() == new_item["watchPath"]
            )
        ]
        deduped_items.append(new_item)
        result = client.save_workstation_config(
            workstation_id,
            {"configVersion": config_version, "items": deduped_items},
        )
        self._set_test_result(result)
        return result

    def _save_and_push_test_binding_config(self) -> None:
        saved = self._save_test_binding_config()
        pushed = self._push_test_binding_config()
        device_id = self.test_inputs["deviceId"].text().strip()
        if device_id:
            self.test_inputs["uploadDeviceId"].setText(device_id)
        self.clear_test_binding_form(preserve_plugin=True)
        self._set_test_result({"saved": saved, "pushed": pushed})

    def push_test_binding_config(self) -> None:
        self._run_action(self._push_test_binding_config, "测试配置已推送")

    def _push_test_binding_config(self) -> None:
        client = self._build_system_client()
        result = client.push_workstation_config(self._require_workstation_id())
        self._set_test_result(result)
        return result

    def upload_test_plugin(self) -> None:
        self._run_action(self._upload_test_plugin, "插件库已上传")

    def _upload_test_plugin(self) -> None:
        client = self._build_system_client()
        zip_path = Path(self.test_inputs["pluginZipPath"].text().strip())
        if not zip_path.is_file():
            raise RuntimeError("插件 zip 文件不存在")
        upload_path = self._prepare_plugin_zip_for_upload(zip_path)
        result = client.upload_plugin_package(upload_path)
        plugin_name = result.get("pluginName")
        if plugin_name:
            self.test_inputs["pluginName"].setText(str(plugin_name))
        display_name = result.get("displayName")
        if display_name:
            self.test_inputs["pluginDisplayName"].setText(str(display_name))
        plugin_id = result.get("pluginId")
        if plugin_id:
            self._select_plugin_by_id(int(plugin_id))
        result_description = result.get("resultDescription")
        if result_description:
            self.test_inputs["pluginResultDescription"].setText(str(result_description))
        result_example = result.get("resultExample")
        if result_example is not None:
            self.test_inputs["pluginResultExample"].setPlainText(json.dumps(result_example, ensure_ascii=False, indent=2))
        self._try_refresh_test_plugins(selected_plugin_name=result.get("pluginName"))
        self._set_test_result(result)

    def enable_test_plugin(self) -> None:
        self._run_action(self._enable_test_plugin, "插件已启用")

    def _enable_test_plugin(self) -> None:
        client = self._build_system_client()
        plugin_name = self.test_inputs["pluginName"].text().strip()
        if not plugin_name:
            raise RuntimeError("插件名不能为空")
        result = client.enable_plugin(plugin_name)
        self._set_test_result(result)
        self.clear_test_plugin_form(preserve_selected_plugin=True)

    def upload_test_file(self) -> None:
        self._run_action(self._upload_test_file, "测试文件已上传")

    def _upload_test_file(self) -> None:
        device_id = self._test_int("uploadDeviceId")
        file_path = Path(self.test_inputs["uploadFilePath"].text().strip())
        if not file_path.is_file():
            raise RuntimeError("测试文件不存在")
        result = asyncio.run(upload(self.config_path, self.state_db, device_id, file_path))
        self._set_test_result(result)
        self.clear_test_upload_form(preserve_device_id=True)

    def _build_system_client(self) -> SystemApiClient:
        config = load_config(self.config_path)
        client_id = (config.system_client_id or "business-system").strip()
        access_token = (config.system_token or "").strip()
        operator = (config.system_operator or "admin").strip()
        if not client_id:
            raise RuntimeError("server.json 中的 systemClientId 不能为空")
        if not access_token:
            raise RuntimeError("server.json 中的 systemToken 不能为空")
        return SystemApiClient(config.api_base_url, client_id, access_token, operator or None)

    def _load_plugin_manifest_preview(self, zip_path: Path) -> None:
        try:
            with ZipFile(zip_path) as archive:
                manifest = json.loads(archive.read("mainfest.json").decode("utf-8"))
        except Exception:
            return
        if isinstance(manifest, dict):
            self.test_inputs["pluginName"].setText(str(manifest.get("pluginName") or ""))
            self.test_inputs["pluginDisplayName"].setText(str(manifest.get("displayName") or ""))
            self.test_inputs["pluginResultDescription"].setText(str(manifest.get("resultDescription") or ""))
            result_example = manifest.get("resultExample")
            if result_example is not None:
                self.test_inputs["pluginResultExample"].setPlainText(json.dumps(result_example, ensure_ascii=False, indent=2))
            else:
                self.test_inputs["pluginResultExample"].setPlainText("")

    def _prepare_plugin_zip_for_upload(self, source_path: Path) -> Path:
        return source_path

    def _test_int(self, key: str) -> int:
        raw_value = self.test_inputs[key].text().strip()
        if not raw_value:
            raise RuntimeError(f"{key} 不能为空")
        try:
            return int(raw_value)
        except ValueError as exc:
            raise RuntimeError(f"{key} 必须是整数") from exc

    def _require_workstation_id(self) -> int:
        config = load_config(self.config_path)
        if config.workstation_id is None:
            raise RuntimeError("工作站尚未注册，缺少 workstationId")
        return int(config.workstation_id)

    def _set_test_result(self, payload) -> None:
        if self.test_result_view is None:
            return
        self.test_result_view.setPlainText(json.dumps(payload, ensure_ascii=False, indent=2))

    def clear_test_plugin_form(self, preserve_selected_plugin: bool = False) -> None:
        self.test_inputs["pluginZipPath"].setText("")
        if preserve_selected_plugin:
            return
        self.test_inputs["pluginName"].setText("")
        self.test_inputs["pluginDisplayName"].setText("")
        self.test_inputs["pluginResultDescription"].setText("")
        self.test_inputs["pluginResultExample"].setPlainText("")

    def clear_test_binding_form(self, preserve_plugin: bool = False) -> None:
        if not preserve_plugin and self.plugin_select is not None:
            self.plugin_select.setCurrentIndex(0)
        self.test_inputs["deviceId"].setText("")
        self.test_inputs["deviceCode"].setText("")
        self.test_inputs["deviceName"].setText("")
        self.test_inputs["watchPath"].setText("")
        self.test_inputs["fileType"].setText("csv")
        self.test_inputs["stableSeconds"].setText("2")
        self.toggle_test_advanced_fields(False)

    def clear_test_upload_form(self, preserve_device_id: bool = False) -> None:
        if not preserve_device_id:
            self.test_inputs["uploadDeviceId"].setText("")
        self.test_inputs["uploadFilePath"].setText("")

    def refresh_test_plugins(self) -> None:
        self._run_action(self._refresh_test_plugins, "插件列表已更新")

    def _try_refresh_test_plugins(self, selected_plugin_name: str | None = None) -> None:
        try:
            self._refresh_test_plugins(selected_plugin_name=selected_plugin_name)
        except Exception:
            logger.debug("refresh test plugins skipped", exc_info=True)

    def _refresh_test_plugins(self, selected_plugin_name: str | None = None) -> None:
        client = self._build_system_client()
        plugins = client.list_plugins()
        self._plugin_options = plugins
        if self.plugin_select is None:
            return
        self.plugin_select.blockSignals(True)
        self.plugin_select.clear()
        self.plugin_select.addItem("请选择插件", "")
        selected_index = 0
        for index, plugin in enumerate(plugins, start=1):
            plugin_name = str(plugin.get("pluginName") or "")
            display_name = str(plugin.get("displayName") or plugin_name)
            plugin_id = plugin.get("pluginId")
            self.plugin_select.addItem(f"{display_name} ({plugin_name})", plugin_id)
            if selected_plugin_name and plugin_name == selected_plugin_name:
                selected_index = index
        self.plugin_select.setCurrentIndex(selected_index)
        self.plugin_select.blockSignals(False)
        self._plugin_selection_changed()

    def _plugin_selection_changed(self) -> None:
        if self.plugin_select is None:
            return
        plugin_id = self.plugin_select.currentData()
        plugin = None
        for item in self._plugin_options:
            if str(item.get("pluginId")) == str(plugin_id):
                plugin = item
                break
        if plugin is None:
            return
        plugin_name = str(plugin.get("pluginName") or "")
        self.test_inputs["pluginName"].setText(plugin_name)
        self.test_inputs["pluginDisplayName"].setText(str(plugin.get("displayName") or plugin_name))
        self.test_inputs["pluginResultDescription"].setText(str(plugin.get("resultDescription") or ""))
        result_example = plugin.get("resultExample")
        if result_example is None:
            self.test_inputs["pluginResultExample"].setPlainText("")
        else:
            self.test_inputs["pluginResultExample"].setPlainText(json.dumps(result_example, ensure_ascii=False, indent=2))

    def _select_plugin_by_id(self, plugin_id: int) -> None:
        if self.plugin_select is None:
            return
        for index in range(self.plugin_select.count()):
            if str(self.plugin_select.itemData(index)) == str(plugin_id):
                self.plugin_select.setCurrentIndex(index)
                return

    def _selected_plugin_id(self) -> int:
        if self.plugin_select is None:
            raise RuntimeError("插件选择框不存在")
        plugin_id = self.plugin_select.currentData()
        if plugin_id in (None, "", 0):
            raise RuntimeError("请选择插件")
        return int(plugin_id)

    def run_diagnostics(self, *, check_network: bool, show_message: bool = True) -> None:
        try:
            result = doctor(self.config_path, self.state_db, self.log_file, check_network=check_network)
        except Exception as exc:
            logger.exception("GUI diagnostics failed")
            self.diagnostics_status_label.setText("总体：失败")
            self.diagnostics_table.setRowCount(1)
            self._set_row(self.diagnostics_table, 0, ["诊断", "failed", "诊断执行失败"])
            self.diagnostics_table.resizeColumnsToContents()
            if show_message:
                self._show_error(exc)
            return

        self.diagnostics_status_label.setText(f"总体：{self._display_status(result['status'])}")
        checks = [self._sanitize_diagnostic_check(check) for check in result.get("checks", [])]
        self.diagnostics_table.setRowCount(len(checks))
        for row_index, check in enumerate(checks):
            self._set_row(
                self.diagnostics_table,
                row_index,
                [check.get("name"), check.get("status"), check.get("message")],
            )
        self.diagnostics_table.resizeColumnsToContents()
        if show_message:
            self._show_info("诊断完成")

    def _sanitize_diagnostic_check(self, check: dict) -> dict:
        name = str(check.get("name") or "")
        status = check.get("status")
        labels = {
            "config": "配置文件",
            "token": "注册状态",
            "watchPath": "监听目录",
            "api": "平台连接",
            "stateDb": "状态库",
            "logDir": "日志目录",
            "diagnostics": "诊断",
        }
        messages = {
            "config": {
                "ok": "配置文件已加载",
                "failed": "配置文件读取失败",
            },
            "token": {
                "ok": "工作站已注册",
                "failed": "工作站尚未注册",
            },
            "watchPath": {
                "ok": "监听目录可访问",
                "failed": "监听目录不可访问",
            },
            "api": {
                "ok": "平台连接正常",
                "failed": "平台连接异常",
                "skipped": "未执行网络检查",
            },
            "stateDb": {
                "ok": "状态库可用",
                "failed": "状态库异常",
            },
            "logDir": {
                "ok": "日志目录可写",
                "failed": "日志目录不可写",
            },
            "diagnostics": {
                "failed": "诊断执行失败",
            },
        }
        safe_name = labels.get(name, name or "检查项")
        safe_message = messages.get(name, {}).get(str(status), self._display_status(str(status)))
        return {"name": safe_name, "status": status, "message": safe_message}

    def _start_runtime_process(self) -> None:
        process = self.qt["QProcess"](self.window)
        if getattr(sys, "frozen", False):
            program_path = Path(self.qt["QCoreApplication"].applicationFilePath()).resolve()
            working_directory = program_path.parent
            process.setProgram(str(program_path))
            arguments = []
        else:
            project_root = Path(__file__).resolve().parent.parent
            program_path = _runtime_program_path()
            working_directory = project_root
            process.setProgram(str(program_path))
            arguments = ["-m", "workstation.cli"]
            environment = process.processEnvironment()
            existing_pythonpath = environment.value("PYTHONPATH")
            environment.insert(
                "PYTHONPATH",
                f"{project_root}:{existing_pythonpath}" if existing_pythonpath else str(project_root),
            )
            process.setProcessEnvironment(environment)
        arguments.extend(
            [
                "--server",
                str(self.config_path),
                "--state-db",
                str(self.state_db),
                "--log-file",
                str(self.log_file),
                "run",
            ]
        )
        process.setWorkingDirectory(str(working_directory))
        process.setArguments(arguments)
        process.started.connect(self._set_runtime_running)
        process.errorOccurred.connect(self._runtime_process_error)
        process.finished.connect(self._runtime_finished)
        self.process = process
        self._runtime_starting = True
        self._runtime_stopping = False
        logger.info(
            "starting runtime process program=%s exists=%s working_directory=%s arguments=%s",
            process.program(),
            program_path.exists(),
            process.workingDirectory(),
            arguments,
        )
        if not program_path.exists():
            self.process = None
            self._runtime_starting = False
            raise RuntimeError(f"后台监听程序不存在：{program_path}")
        process.start()
        self.refresh_status()

    def start_runtime(self, *, show_message: bool = True) -> None:
        if self.process is not None:
            if show_message:
                self._show_info("监听已经在运行")
            return
        self._start_runtime_process()

    def _set_runtime_running(self) -> None:
        self._runtime_starting = False
        self.refresh_status()

    def stop_runtime(self) -> None:
        if self.process is None:
            self.refresh_status()
            return
        self._runtime_stopping = True
        self._runtime_starting = False
        process = self.process
        process.terminate()
        self.qt["QTimer"].singleShot(3000, lambda: self._force_stop_runtime(process))
        self.refresh_status()

    def _force_stop_runtime(self, process) -> None:
        if self.process is not process:
            return
        if process.state() != self.qt["PROCESS_NOT_RUNNING"]:
            logger.warning("runtime process did not stop in time, killing it")
            process.kill()

    def exit_application(self) -> None:
        self.stop_runtime()
        self._allow_close = True
        if self.tray_icon is not None:
            self.tray_icon.hide()
        self.qt["QApplication"].quit()

    def _runtime_process_error(self, error) -> None:
        if self.process is None:
            return
        error_message = self.process.errorString() or "未知错误"
        logger.error("runtime process error=%s", error_message)
        if error == self.qt["PROCESS_FAILED_TO_START"]:
            self._runtime_starting = False
            self._runtime_stopping = False
            self.process = None
            self.refresh_status()
            self._show_error(RuntimeError(f"后台监听进程启动失败：{error_message}"))

    def _runtime_finished(self, exit_code: int, _exit_status) -> None:
        was_stopping = self._runtime_stopping
        self._runtime_starting = False
        self._runtime_stopping = False
        self.process = None
        if exit_code != 0 and not was_stopping:
            logger.error("runtime process exited unexpectedly exit_code=%s", exit_code)
            self._show_error(RuntimeError(f"后台监听进程异常退出，退出码：{exit_code}。请查看日志。"))
        self.refresh_status()

    def open_log_folder(self) -> None:
        self.log_file.parent.mkdir(parents=True, exist_ok=True)
        opened = self.qt["QDesktopServices"].openUrl(self.qt["QUrl"].fromLocalFile(str(self.log_file.parent)))
        if opened:
            return
        process = self.qt["QProcess"](self.window)
        if sys.platform == "darwin":
            process.startDetached("open", [str(self.log_file.parent)])
            return
        if sys.platform.startswith("win"):
            process.startDetached("explorer", [str(self.log_file.parent)])
            return
        process.startDetached("xdg-open", [str(self.log_file.parent)])

    def _run_action(self, fn, message: str) -> None:
        try:
            fn()
        except Exception as exc:
            logger.exception("GUI action failed")
            self._show_error(exc)
            return
        self.refresh_all()
        self._show_info(message)

    def _show_error(self, exc: Exception) -> None:
        self.qt["QMessageBox"].critical(self.window, "错误", str(exc))

    def _show_info(self, message: str) -> None:
        self.qt["QMessageBox"].information(self.window, "NetStar", message)

    def _show_parse_notice(self, title: str, message: str, *, is_error: bool) -> None:
        box = self.qt["QWidget"](None, self.qt["Qt"].Tool | self.qt["Qt"].FramelessWindowHint | self.qt["Qt"].WindowStaysOnTopHint)
        box.setAttribute(self.qt["WA_DELETE_ON_CLOSE"], True)
        box.setAttribute(self.qt["WA_SHOW_WITHOUT_ACTIVATING"], True)
        box.setStyleSheet(
            f"""
            QWidget {{
                background: {"#fff4f4" if is_error else "#f3fbf8"};
                border: 1px solid {"#efc2c2" if is_error else "#b8dfcf"};
                border-radius: 14px;
            }}
            QLabel#noticeTitle {{
                color: {"#9f2f2f" if is_error else "#176748"};
                font-size: 13px;
                font-weight: 800;
                background: transparent;
            }}
            QLabel#noticeMessage {{
                color: #30485d;
                font-size: 12px;
                background: transparent;
            }}
            """
        )

        layout = self.qt["QVBoxLayout"](box)
        layout.setContentsMargins(14, 12, 14, 12)
        layout.setSpacing(6)
        title_label = self.qt["QLabel"](title)
        title_label.setObjectName("noticeTitle")
        message_label = self.qt["QLabel"](message)
        message_label.setObjectName("noticeMessage")
        message_label.setWordWrap(True)
        layout.addWidget(title_label)
        layout.addWidget(message_label)
        box.resize(320, box.sizeHint().height())
        self._position_notice_box(box)
        box.show()
        self._parse_notice_boxes.append(box)

        def close_box() -> None:
            if box in self._parse_notice_boxes:
                self._parse_notice_boxes.remove(box)
            box.close()

        box.destroyed.connect(lambda *_: self._reposition_notice_boxes())
        self.qt["QTimer"].singleShot(3500, close_box)

    def _position_notice_box(self, box) -> None:
        screen = self.window.screen() or self.qt["QApplication"].primaryScreen()
        if screen is None:
            return
        available = screen.availableGeometry()
        margin = 20
        spacing = 12
        width = box.width()
        height = box.height()
        stack_index = len(self._parse_notice_boxes)
        x = available.x() + available.width() - width - margin
        y = available.y() + available.height() - height - margin - stack_index * (height + spacing)
        box.move(max(available.x() + margin, x), max(available.y() + margin, y))

    def _reposition_notice_boxes(self) -> None:
        active_boxes = [box for box in self._parse_notice_boxes if box is not None and box.isVisible()]
        self._parse_notice_boxes = active_boxes
        for index, box in enumerate(active_boxes):
            screen = self.window.screen() or self.qt["QApplication"].primaryScreen()
            if screen is None:
                return
            available = screen.availableGeometry()
            margin = 20
            spacing = 12
            x = available.x() + available.width() - box.width() - margin
            y = available.y() + available.height() - box.height() - margin - index * (box.height() + spacing)
            box.move(max(available.x() + margin, x), max(available.y() + margin, y))

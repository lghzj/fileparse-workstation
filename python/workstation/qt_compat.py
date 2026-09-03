from __future__ import annotations


def load_qt() -> dict:
    try:
        from PySide6.QtCore import QCoreApplication, QFileSystemWatcher, QProcess, Qt, QTimer, QUrl
        from PySide6.QtGui import QAction, QColor, QDesktopServices, QIcon, QKeySequence
        from PySide6.QtNetwork import QLocalServer, QLocalSocket
        from PySide6.QtWidgets import (
            QApplication,
            QComboBox,
            QAbstractItemView,
            QFileDialog,
            QFormLayout,
            QFrame,
            QGridLayout,
            QGroupBox,
            QHBoxLayout,
            QHeaderView,
            QInputDialog,
            QLabel,
            QLineEdit,
            QMainWindow,
            QMenu,
            QMessageBox,
            QPlainTextEdit,
            QPushButton,
            QScrollArea,
            QSizePolicy,
            QStyle,
            QSystemTrayIcon,
            QTabWidget,
            QTableWidget,
            QTableWidgetItem,
            QVBoxLayout,
            QWidget,
        )

        binding = "PySide6"
        constants = {
            "HEADER_STRETCH": QHeaderView.ResizeMode.Stretch,
            "HEADER_INTERACTIVE": QHeaderView.ResizeMode.Interactive,
            "HEADER_RESIZE_TO_CONTENTS": QHeaderView.ResizeMode.ResizeToContents,
            "SIZE_EXPANDING": QSizePolicy.Policy.Expanding,
            "SIZE_FIXED": QSizePolicy.Policy.Fixed,
            "NO_LINE_WRAP": QPlainTextEdit.LineWrapMode.NoWrap,
            "TRAY_DOUBLE_CLICK": QSystemTrayIcon.ActivationReason.DoubleClick,
            "TRAY_INFO": QSystemTrayIcon.MessageIcon.Information,
            "TRAY_WARNING": QSystemTrayIcon.MessageIcon.Warning,
            "PASSWORD_ECHO": QLineEdit.EchoMode.Password,
            "WA_DELETE_ON_CLOSE": Qt.WidgetAttribute.WA_DeleteOnClose,
            "WA_SHOW_WITHOUT_ACTIVATING": Qt.WidgetAttribute.WA_ShowWithoutActivating,
            "FORM_GROW_ALL_NON_FIXED": QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow,
            "FORM_DONT_WRAP_ROWS": QFormLayout.RowWrapPolicy.DontWrapRows,
            "FRAME_NO_FRAME": QFrame.Shape.NoFrame,
            "STYLE_COMPUTER_ICON": QStyle.StandardPixmap.SP_ComputerIcon,
            "SELECT_ROWS": QAbstractItemView.SelectionBehavior.SelectRows,
            "SINGLE_SELECTION": QAbstractItemView.SelectionMode.SingleSelection,
            "PROCESS_NOT_RUNNING": QProcess.ProcessState.NotRunning,
            "PROCESS_FAILED_TO_START": QProcess.ProcessError.FailedToStart,
        }
    except ImportError:
        try:
            from PySide2.QtCore import QCoreApplication, QFileSystemWatcher, QProcess, Qt, QTimer, QUrl
            from PySide2.QtGui import QColor, QDesktopServices, QIcon, QKeySequence
            from PySide2.QtNetwork import QLocalServer, QLocalSocket
            from PySide2.QtWidgets import (
                QAction,
                QAbstractItemView,
                QApplication,
                QComboBox,
                QFileDialog,
                QFormLayout,
                QFrame,
                QGridLayout,
                QGroupBox,
                QHBoxLayout,
                QHeaderView,
                QInputDialog,
                QLabel,
                QLineEdit,
                QMainWindow,
                QMenu,
                QMessageBox,
                QPlainTextEdit,
                QPushButton,
                QScrollArea,
                QSizePolicy,
                QStyle,
                QSystemTrayIcon,
                QTabWidget,
                QTableWidget,
                QTableWidgetItem,
                QVBoxLayout,
                QWidget,
            )

            binding = "PySide2"
            constants = {
                "HEADER_STRETCH": QHeaderView.Stretch,
                "HEADER_INTERACTIVE": QHeaderView.Interactive,
                "HEADER_RESIZE_TO_CONTENTS": QHeaderView.ResizeToContents,
                "SIZE_EXPANDING": QSizePolicy.Expanding,
                "SIZE_FIXED": QSizePolicy.Fixed,
                "NO_LINE_WRAP": QPlainTextEdit.NoWrap,
                "TRAY_DOUBLE_CLICK": QSystemTrayIcon.DoubleClick,
                "TRAY_INFO": QSystemTrayIcon.Information,
                "TRAY_WARNING": QSystemTrayIcon.Warning,
                "PASSWORD_ECHO": QLineEdit.Password,
                "WA_DELETE_ON_CLOSE": Qt.WA_DeleteOnClose,
                "WA_SHOW_WITHOUT_ACTIVATING": Qt.WA_ShowWithoutActivating,
                "FORM_GROW_ALL_NON_FIXED": QFormLayout.AllNonFixedFieldsGrow,
                "FORM_DONT_WRAP_ROWS": QFormLayout.DontWrapRows,
                "FRAME_NO_FRAME": QFrame.NoFrame,
                "STYLE_COMPUTER_ICON": QStyle.SP_ComputerIcon,
                "SELECT_ROWS": QAbstractItemView.SelectRows,
                "SINGLE_SELECTION": QAbstractItemView.SingleSelection,
                "PROCESS_NOT_RUNNING": QProcess.NotRunning,
                "PROCESS_FAILED_TO_START": QProcess.FailedToStart,
            }
        except ImportError as exc:
            raise RuntimeError("PySide6 or PySide2 is required to run the workstation GUI") from exc

    api = {
        "binding": binding,
        "QAction": QAction,
        "QColor": QColor,
        "QCoreApplication": QCoreApplication,
        "QApplication": QApplication,
        "QComboBox": QComboBox,
        "QDesktopServices": QDesktopServices,
        "QFileSystemWatcher": QFileSystemWatcher,
        "QFileDialog": QFileDialog,
        "QFormLayout": QFormLayout,
        "QGridLayout": QGridLayout,
        "QGroupBox": QGroupBox,
        "QHBoxLayout": QHBoxLayout,
        "QHeaderView": QHeaderView,
        "QIcon": QIcon,
        "QInputDialog": QInputDialog,
        "QKeySequence": QKeySequence,
        "QLabel": QLabel,
        "QLineEdit": QLineEdit,
        "QLocalServer": QLocalServer,
        "QLocalSocket": QLocalSocket,
        "QMainWindow": QMainWindow,
        "QMenu": QMenu,
        "QMessageBox": QMessageBox,
        "QPlainTextEdit": QPlainTextEdit,
        "QProcess": QProcess,
        "QPushButton": QPushButton,
        "QScrollArea": QScrollArea,
        "QSizePolicy": QSizePolicy,
        "QSystemTrayIcon": QSystemTrayIcon,
        "QTabWidget": QTabWidget,
        "QTableWidget": QTableWidget,
        "QTableWidgetItem": QTableWidgetItem,
        "QTimer": QTimer,
        "QUrl": QUrl,
        "QVBoxLayout": QVBoxLayout,
        "QWidget": QWidget,
        "Qt": Qt,
    }
    api.update(constants)
    return api


def exec_application(app) -> int:
    exec_method = getattr(app, "exec", None)
    if exec_method is not None:
        return exec_method()
    return app.exec_()

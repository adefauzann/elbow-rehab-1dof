"""
core/styles.py
Semua stylesheet QSS (Qt Style Sheets) untuk dark theme dashboard.

Referensi dokumen:
  - docs/01_FEATURE_HEADER.md        (HEADER_STYLE)
  - docs/02_FEATURE_CARDS_REALTIME.md (CARD_STYLE)
  - docs/03_FEATURE_CARDS_SAFETY.md   (SAFETY_CARD_STYLE)
  - docs/04_FEATURE_CHARTS.md         (CHART_BUTTON_STYLE)
  - docs/05_FEATURE_SESSION_HISTORY.md (TABLE_STYLE, TOOLBAR_STYLE)

Palet warna utama:
  Background utama  : #0d1117
  Background card   : #16213e
  Accent kritis     : #e94560
  Teks utama        : #c9d1d9
  Teks sekunder     : #8b949e
  Border            : #30363d
"""

# ---------------------------------------------------------------------------
# Main Window
# ---------------------------------------------------------------------------
MAIN_WINDOW_STYLE = """
    QMainWindow {
        background-color: #0d1117;
    }
    QWidget {
        background-color: #0d1117;
        color: #c9d1d9;
        font-family: 'Segoe UI', sans-serif;
    }
    QScrollArea {
        border: none;
    }
    QLabel#SectionTitle {
        color: #8b949e;
        font-size: 10px;
        font-weight: bold;
        letter-spacing: 1.5px;
    }
    QSplitter::handle {
        background-color: #21262d;
    }
"""

# ---------------------------------------------------------------------------
# Header (Fitur 01)
# ---------------------------------------------------------------------------
HEADER_STYLE = """
    QWidget#HeaderWidget {
        background-color: #161b22;
        border-bottom: 2px solid #e94560;
    }
    QLabel#TitleLabel {
        color: #e6edf3;
        font-size: 17px;
        font-weight: bold;
        letter-spacing: 0.5px;
    }
    QLabel#SubtitleLabel {
        color: #8b949e;
        font-size: 10px;
    }
    QLabel#ClockLabel {
        color: #e6edf3;
        font-size: 20px;
        font-weight: bold;
        font-family: 'Consolas', monospace;
    }
    QLabel#DateLabel {
        color: #8b949e;
        font-size: 10px;
    }
    QLabel#StatusLabel {
        color: #ef5350;
        font-size: 10px;
        font-weight: bold;
    }
    QLabel#StatusLabelConnected {
        color: #66bb6a;
        font-size: 10px;
        font-weight: bold;
    }
    QComboBox#PortCombo, QComboBox#BaudCombo {
        background-color: #21262d;
        color: #c9d1d9;
        border: 1px solid #30363d;
        border-radius: 4px;
        padding: 4px 8px;
    }
    QComboBox#PortCombo:hover, QComboBox#BaudCombo:hover {
        border-color: #58a6ff;
    }
    QComboBox QAbstractItemView {
        background-color: #21262d;
        color: #c9d1d9;
        selection-background-color: #1f6feb;
    }
    QPushButton#ConnectButton {
        background-color: #e94560;
        color: white;
        border: none;
        border-radius: 4px;
        padding: 6px 16px;
        font-weight: bold;
        font-size: 12px;
    }
    QPushButton#ConnectButton:hover {
        background-color: #c73652;
    }
    QPushButton#RefreshButton {
        background-color: #21262d;
        color: #c9d1d9;
        border: 1px solid #30363d;
        border-radius: 4px;
        font-size: 14px;
    }
    QPushButton#RefreshButton:hover {
        background-color: #30363d;
    }
"""

# ---------------------------------------------------------------------------
# Metric Cards (Fitur 02)
# ---------------------------------------------------------------------------
CARD_STYLE = """
    QFrame#MetricCard {
        background-color: #161b22;
        border: 1px solid #21262d;
        border-radius: 8px;
    }
    QLabel#CardTitle {
        color: #8b949e;
        font-size: 9px;
        font-weight: bold;
        letter-spacing: 1.5px;
    }
    QLabel#CardIcon {
        font-size: 18px;
    }
    QLabel#CardValue {
        font-size: 38px;
        font-weight: bold;
        font-family: 'Consolas', monospace;
    }
    QLabel#CardDescription {
        color: #484f58;
        font-size: 9px;
    }
    QProgressBar#CardProgress {
        background-color: #21262d;
        border: none;
        border-radius: 3px;
    }
    QProgressBar#CardProgress::chunk {
        background-color: #1f6feb;
        border-radius: 3px;
    }
"""

# ---------------------------------------------------------------------------
# Safety Cards (Fitur 03)
# ---------------------------------------------------------------------------
SAFETY_CARD_STYLE = """
    QFrame#SafetyCard {
        background-color: #161b22;
        border: 1px solid #21262d;
        border-radius: 8px;
    }
    QLabel#SafetyTitle {
        color: #e94560;
        font-size: 9px;
        font-weight: bold;
        letter-spacing: 1.5px;
    }
    QLabel#IndicatorLabel {
        color: #8b949e;
        font-size: 8px;
        letter-spacing: 0.5px;
    }
    QLabel#IndicatorValue {
        font-size: 11px;
        font-weight: bold;
    }
    QLabel#ModeBadge {
        font-size: 14px;
        font-weight: bold;
    }
"""

# ---------------------------------------------------------------------------
# Chart Buttons (Fitur 04)
# ---------------------------------------------------------------------------
CHART_BUTTON_STYLE = """
    QPushButton#ChartButton {
        background-color: #21262d;
        color: #c9d1d9;
        border: 1px solid #30363d;
        border-radius: 5px;
        padding: 4px 12px;
        font-size: 11px;
    }
    QPushButton#ChartButton:hover {
        background-color: #30363d;
        border-color: #8b949e;
    }
"""

# ---------------------------------------------------------------------------
# Session History Table (Fitur 05)
# ---------------------------------------------------------------------------
TABLE_STYLE = """
    QTableWidget {
        background-color: #0d1117;
        alternate-background-color: #161b22;
        color: #c9d1d9;
        gridline-color: #21262d;
        border: 1px solid #30363d;
        border-radius: 6px;
        font-family: 'Segoe UI', sans-serif;
        font-size: 12px;
    }
    QTableWidget::item {
        padding: 4px 8px;
    }
    QTableWidget::item:selected {
        background-color: #1f6feb;
        color: white;
    }
    QHeaderView::section {
        background-color: #161b22;
        color: #8b949e;
        border: none;
        border-right: 1px solid #21262d;
        border-bottom: 1px solid #21262d;
        padding: 6px 8px;
        font-weight: bold;
        font-size: 10px;
        letter-spacing: 0.5px;
    }
    QHeaderView::section:hover {
        background-color: #1c2128;
        color: #c9d1d9;
    }
    QScrollBar:vertical {
        background-color: #0d1117;
        width: 8px;
    }
    QScrollBar::handle:vertical {
        background-color: #30363d;
        border-radius: 4px;
    }
"""

TOOLBAR_STYLE = """
    QPushButton {
        background-color: #21262d;
        color: #c9d1d9;
        border: 1px solid #30363d;
        border-radius: 5px;
        padding: 5px 12px;
        font-size: 11px;
    }
    QPushButton:hover {
        background-color: #30363d;
        border-color: #8b949e;
    }
    QPushButton#ExportButton {
        background-color: #238636;
        color: white;
        border-color: #2ea043;
        font-weight: bold;
    }
    QPushButton#ExportButton:hover {
        background-color: #2ea043;
    }
    QLineEdit {
        background-color: #21262d;
        color: #c9d1d9;
        border: 1px solid #30363d;
        border-radius: 5px;
        padding: 5px 10px;
        font-size: 11px;
    }
    QLineEdit:focus {
        border-color: #58a6ff;
    }
"""

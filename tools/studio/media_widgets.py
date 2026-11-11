"""Reusable Media Manager widget primitives and visual assets."""
import sys
from functools import lru_cache
from pathlib import Path

from PySide6.QtCore import QEvent, QPointF, Qt
from PySide6.QtGui import QColor, QIcon, QImage, QPainter, QPainterPath, QPalette, QPen, QPixmap
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtWidgets import (
    QBoxLayout,
    QCheckBox,
    QComboBox,
    QHBoxLayout,
    QLabel,
    QListView,
    QMenu,
    QPushButton,
    QSizePolicy,
    QStyle,
    QStyleOptionButton,
    QVBoxLayout,
    QWidget,
)


UI_FONT = 'Century Gothic'

ICONS = {
    'music': '<path d="M9 18V5l12-2v13M9 9l12-2"/><ellipse cx="6" cy="18" rx="3" ry="3"/><ellipse cx="18" cy="16" rx="3" ry="3"/>',
    'photos': '<rect x="3" y="3" width="18" height="18" rx="3"/><circle cx="8" cy="8" r="1.5"/><path d="m3 17 5-5 4 4 4-6 5 7"/>',
    'videos': '<rect x="3" y="3" width="18" height="18" rx="3"/><path d="m10 8 6 4-6 4Z"/>',
    'notes': '<path d="M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9Z M14 3v6h6M8 13h8M8 17h5"/>',
    'prepare': '<path d="M12 3v12m-4-4 4 4 4-4M4 16v4h16v-4"/>',
    'setup': '<rect x="6" y="2" width="12" height="20" rx="3"/><rect x="8.5" y="5" width="7" height="6" rx="1"/><circle cx="12" cy="16" r="2.5"/>',
    'trash': '<path d="M3 6h18M9 6V3h6v3M5 6l1 15h12l1-15M10 10v7M14 10v7"/>',
    'settings': '<path d="M4 6h16M4 12h16M4 18h16"/><circle cx="8" cy="6" r="2"/><circle cx="16" cy="12" r="2"/><circle cx="10" cy="18" r="2"/>',
    'search': '<circle cx="10" cy="10" r="6"/><path d="m15 15 6 6"/>',
    'refresh': '<path d="M20 10a8 8 0 1 0-2 8M20 4v6h-6"/>',
    'plus': '<path d="M12 5v14M5 12h14"/>',
    'folder': '<path d="M3 7V5h6l2 2h10v13H3Z"/>',
    'back': '<path d="m14 5-7 7 7 7"/>',
    'eject': '<path d="m5 14 7-10 7 10ZM5 19h14"/>',
    'check': '<path d="m5 12 4 4L19 6"/>',
    'camera': '<path d="M3 7h4l2-3h6l2 3h4v14H3Z"/><circle cx="12" cy="13" r="4"/>',
}

@lru_cache(maxsize=1)
def _boot_artwork_source():
    return QPixmap(str(Path(__file__).resolve().parents[2] / 'assets' / 'boot-screen.png'))

def boot_artwork(rect, width, height):
    artwork = _boot_artwork_source()
    image = artwork.copy(*rect).scaled(width * 2, height * 2, Qt.KeepAspectRatio,
                                      Qt.SmoothTransformation).toImage().convertToFormat(QImage.Format_ARGB32)
    # The boot artwork has a near-black matte. Render it as transparency while
    # preserving the silver and cyan lettering and antialiased edges.
    pixels = image.bits()
    stride = image.bytesPerLine()
    red, green, blue, alpha_byte = ((2, 1, 0, 3) if sys.byteorder == 'little' else
                                    (1, 2, 3, 0))
    for y in range(image.height()):
        for x in range(image.width()):
            offset = y * stride + x * 4
            peak = max(pixels[offset + red], pixels[offset + green], pixels[offset + blue])
            alpha = max(0, (peak - 24) * 255 // 231)
            if alpha:
                for channel in (red, green, blue):
                    index = offset + channel
                    pixels[index] = min(255, pixels[index] * 255 // peak)
                pixels[offset + alpha_byte] = alpha
            else:
                pixels[offset:offset + 4] = b'\0\0\0\0'
    pixmap = QPixmap.fromImage(image)
    pixmap.setDevicePixelRatio(2)
    return pixmap

@lru_cache(maxsize=128)
def icon(name, color='#63757b', size=20):
    data = f'<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="{color}" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">{ICONS[name]}</svg>'
    pixmap = QPixmap(size * 2, size * 2)
    pixmap.fill(Qt.transparent)
    painter = QPainter(pixmap)
    QSvgRenderer(data.encode()).render(painter)
    painter.end()
    pixmap.setDevicePixelRatio(2)
    return QIcon(pixmap)

def label(text='', name=None, wrap=False):
    item = QLabel(text)
    item.setTextFormat(Qt.PlainText)
    item.setWordWrap(wrap)
    if name:
        item.setObjectName(name)
    return item

def button(text, callback, name=None, glyph=None):
    item = QPushButton(text)
    item.setCursor(Qt.PointingHandCursor)
    if name:
        item.setObjectName(name)
    if glyph:
        item.setIcon(icon(glyph, '#102b32' if name == 'primary' else '#63757b'))
    item.clicked.connect(callback)
    return item

def box(parent=None, horizontal=False, margins=(0, 0, 0, 0), spacing=12):
    layout = QHBoxLayout(parent) if horizontal else QVBoxLayout(parent)
    layout.setContentsMargins(*margins)
    layout.setSpacing(spacing)
    return layout

def action_menu(title, entries, *, minimum_width=0):
    """Group related actions while reflecting their availability in the chooser."""
    chooser = button(title, lambda: None, 'actionChooser')
    chooser.setMinimumWidth(minimum_width)
    menu = QMenu(chooser)
    menu.setObjectName('actionMenu')
    actions = []
    for text, callback in entries:
        if text is None:
            menu.addSeparator()
            continue
        action = menu.addAction(text)
        action.triggered.connect(callback)
        actions.append(action)
    chooser.setMenu(menu)

    def update_availability():
        visible = [action for action in actions if action.isVisible()]
        # Layouts will show new buttons after giving them a parent widget.
        if chooser.parentWidget() is not None or not visible:
            chooser.setVisible(bool(visible))
        chooser.setEnabled(any(action.isEnabled() for action in visible))

    for action in actions:
        action.changed.connect(update_availability)
    update_availability()
    return chooser, actions

class ControlRow(QWidget):
    """Place related text beside its control, stacking on smaller windows."""

    def __init__(self, text, control, *, breakpoint=620):
        super().__init__()
        self.text = text
        self.control = control
        self.breakpoint = breakpoint
        self.row = box(self, spacing=16)
        self.row.addWidget(text, 1)
        self.row.addWidget(control, 0, Qt.AlignLeft)
        self.stacked = True

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.update_direction()

    def event(self, event):
        handled = super().event(event)
        if event.type() == QEvent.LayoutRequest:
            self.update_direction()
        return handled

    def update_direction(self):
        if self.breakpoint is None:
            visible = [item for item in (self.text, self.control) if not item.isHidden()]
            required = sum(max(item.minimumSizeHint().width(), item.minimumWidth()) for item in visible)
            required += self.row.spacing() * max(0, len(visible) - 1)
            margins = self.row.contentsMargins()
            stacked = required > self.width() - margins.left() - margins.right()
        else:
            stacked = self.width() < self.breakpoint
        if stacked != self.stacked:
            self.row.setDirection(QBoxLayout.TopToBottom if stacked else QBoxLayout.LeftToRight)
            self.row.setAlignment(self.control, Qt.AlignLeft if stacked else Qt.AlignRight | Qt.AlignVCenter)
            self.stacked = stacked
            self.updateGeometry()

class StatusLabel(QLabel):
    def __init__(self, text):
        super().__init__()
        self.setTextFormat(Qt.PlainText)
        self.setObjectName('muted'); self.setMinimumWidth(0)
        self.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
        self.message = text

    def set_message(self, text, tooltip=None):
        self.message = text
        self.setToolTip(tooltip or text)
        self.render_message()

    def render_message(self):
        self.setText(self.message if self.wordWrap() else
                     self.fontMetrics().elidedText(self.message, Qt.ElideRight, max(0, self.contentsRect().width())))

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.render_message()

class ComboBox(QComboBox):
    def __init__(self, parent=None):
        super().__init__(parent)
        view = QListView(self)
        view.setUniformItemSizes(True)
        self.setView(view)

    def showPopup(self):
        # Qt's popup container and scroll gutters otherwise inherit the OS dark
        # palette, even when the list itself has a light stylesheet.
        popup = self.view().window()
        palette = popup.palette()
        for role in (QPalette.Window, QPalette.Base, QPalette.Button):
            palette.setColor(role, QColor('white'))
        palette.setColor(QPalette.WindowText, QColor('#202629'))
        popup.setPalette(palette)
        popup.setAutoFillBackground(True)
        super().showPopup()

    def paintEvent(self, event):
        super().paintEvent(event)
        painter = QPainter(self); painter.setRenderHint(QPainter.Antialiasing)
        painter.setPen(QPen(QColor('#63757b'), 1.5))
        x, y = self.width() - 18, self.height() / 2
        painter.drawLine(QPointF(x - 3, y - 2), QPointF(x, y + 1))
        painter.drawLine(QPointF(x, y + 1), QPointF(x + 3, y - 2))
        painter.end()

class CheckBox(QCheckBox):
    def paintEvent(self, event):
        super().paintEvent(event)
        if self.isChecked():
            option = QStyleOptionButton(); self.initStyleOption(option)
            rect = self.style().subElementRect(QStyle.SE_CheckBoxIndicator, option, self)
            painter = QPainter(self); painter.setRenderHint(QPainter.Antialiasing)
            painter.setPen(QPen(QColor('white'), 1.7, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
            path = QPainterPath(QPointF(rect.x() + rect.width() * .27, rect.y() + rect.height() * .5))
            path.lineTo(rect.x() + rect.width() * .45, rect.y() + rect.height() * .68)
            path.lineTo(rect.x() + rect.width() * .76, rect.y() + rect.height() * .34)
            painter.drawPath(path); painter.end()

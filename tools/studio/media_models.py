"""Library table models and artwork delegates."""
from PySide6.QtCore import QAbstractTableModel, QModelIndex, QRectF, QSize, Qt, Slot
from PySide6.QtGui import QColor, QFont, QPainter
from PySide6.QtWidgets import QStyle, QStyledItemDelegate, QTableView, QWidget

from tools.content.media_manager import format_size as size

from .media_artwork import paint_cover
from .media_widgets import UI_FONT


def music_group_key(row, field):
    title = row.get(field) or f'Unknown {field}'
    return (title, row.get('album_artist') or row.get('artist', '')) if field == 'album' else title

def library_search_text(row):
    return ' '.join(str(row.get(field, '')) for field in ('title', 'artist', 'album', 'genre', 'path')).casefold()

class MediaModel(QAbstractTableModel):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.rows, self.columns = [], [('title', 'TITLE')]

    def reset(self, rows, columns=None):
        self.beginResetModel()
        self.rows = rows
        if columns is not None:
            self.columns = columns
        self.endResetModel()

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.rows)

    def columnCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.columns)

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or not 0 <= index.row() < len(self.rows) or not 0 <= index.column() < len(self.columns):
            return None
        row = self.rows[index.row()]
        if role == Qt.UserRole:
            return row
        if role == Qt.ToolTipRole:
            return str(row.get(self.columns[index.column()][0], ''))
        if role == Qt.DisplayRole:
            key = self.columns[index.column()][0]
            value = row.get(key, '')
            if key == 'bytes':
                return size(value or 0)
            if key == 'duration':
                return f'{int(value) // 60}:{int(value) % 60:02}' if value else '—'
            return str(value or '—')
        if role == Qt.ForegroundRole and index.column():
            return QColor('#63757b')

    def headerData(self, section, orientation, role=Qt.DisplayRole):
        if orientation == Qt.Horizontal and role == Qt.DisplayRole and 0 <= section < len(self.columns):
            return self.columns[section][1]

    def sort(self, column, order=Qt.AscendingOrder):
        if not 0 <= column < len(self.columns):
            return
        key = self.columns[column][0]
        self.layoutAboutToBeChanged.emit()
        previous = self.persistentIndexList()
        if key == 'status' and all(row.get('kind') == 'Trash' for row in self.rows):
            key = 'started'
        if key in ('bytes', 'duration', 'started'):
            sort_key = lambda row: row.get(key, 0) or 0
        else:
            sort_key = lambda row: str(row.get(key, '')).casefold()
        if previous:
            positions = sorted(range(len(self.rows)), key=lambda at: sort_key(self.rows[at]), reverse=order == Qt.DescendingOrder)
            self.rows[:] = [self.rows[at] for at in positions]
            relocated = {old: new for new, old in enumerate(positions)}
            self.changePersistentIndexList(previous, [self.index(relocated[index.row()], index.column()) for index in previous])
        else:
            self.rows.sort(key=sort_key, reverse=order == Qt.DescendingOrder)
        self.layoutChanged.emit()


class LibraryTable(QTableView):
    """Keep track titles readable when secondary metadata cannot fit."""

    def fit_columns(self):
        model = self.model()
        if model is None:
            return
        compact = self.viewport().width() < 700
        for column, (key, _) in enumerate(model.columns):
            hidden = compact and key in ('album', 'genre')
            if self.isColumnHidden(column) != hidden:
                self.setColumnHidden(column, hidden)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.fit_columns()


class Artwork(QWidget):
    def __init__(self, row, covers, parent=None):
        super().__init__(parent)
        self.row, self.covers = row, covers
        self.setFixedSize(180, 180)
        covers.events.artwork.connect(self.art_ready)

    @Slot(int, str, object)
    def art_ready(self, generation, path, _image):
        artwork = self.row.get('artwork')
        if generation == self.covers.generation and (path == artwork or artwork not in self.covers.cache):
            self.update()

    def paintEvent(self, event):
        painter = QPainter(self); painter.setRenderHint(QPainter.Antialiasing)
        paint_cover(painter, QRectF(self.rect()), self.row, self.covers)
        painter.end()

class TrackDelegate(QStyledItemDelegate):
    def __init__(self, covers, parent=None):
        super().__init__(parent)
        self.covers = covers

    def paint(self, painter, option, index):
        if index.column():
            return super().paint(painter, option, index)
        row = index.data(Qt.UserRole)
        painter.save()
        painter.setRenderHint(QPainter.Antialiasing)
        if option.state & QStyle.State_Selected:
            painter.fillRect(option.rect, QColor('#ddf4f8'))
        elif option.state & QStyle.State_MouseOver:
            painter.fillRect(option.rect, QColor('#edf7f9'))
        painter.setPen(QColor('#e9eef0'))
        painter.drawLine(option.rect.bottomLeft(), option.rect.bottomRight())
        x, y = option.rect.x(), option.rect.y()
        paint_cover(painter, QRectF(x + 12, y + 9, 38, 38), row, self.covers, 5)
        painter.setPen(QColor('#202c31'))
        painter.setFont(QFont(UI_FONT, 10, QFont.DemiBold))
        width = option.rect.width() - 76
        painter.drawText(x + 64, y + 25, painter.fontMetrics().elidedText(row['title'], Qt.ElideRight, width))
        painter.setPen(QColor('#617176'))
        painter.setFont(QFont(UI_FONT, 9))
        subtitle = row.get('artist') or row.get('status') or row.get('subtitle', '')
        painter.drawText(x + 64, y + 43, painter.fontMetrics().elidedText(subtitle, Qt.ElideRight, width))
        painter.restore()

class CardDelegate(QStyledItemDelegate):
    def __init__(self, covers, parent=None):
        super().__init__(parent)
        self.covers = covers

    def sizeHint(self, option, index):
        return QSize(186, 234)

    def paint(self, painter, option, index):
        row = index.data(Qt.UserRole)
        painter.save()
        painter.setRenderHint(QPainter.Antialiasing)
        rect = QRectF(option.rect).adjusted(4, 4, -8, -5)
        if option.state & (QStyle.State_Selected | QStyle.State_MouseOver):
            painter.setPen(Qt.NoPen)
            painter.setBrush(QColor('#d9f2f7' if option.state & QStyle.State_Selected else '#ecf4f6'))
            painter.drawRoundedRect(rect, 13, 13)
        art = QRectF(rect.x() + 7, rect.y() + 7, rect.width() - 14, rect.width() - 14)
        paint_cover(painter, art, row, self.covers, art.width() / 2 if row.get('group_kind') == 'Artists' else 9)
        painter.setPen(QColor('#202c31'))
        painter.setFont(QFont(UI_FONT, 10, QFont.DemiBold))
        painter.drawText(int(art.x()), int(art.bottom() + 24), painter.fontMetrics().elidedText(row['title'], Qt.ElideRight, int(art.width())))
        painter.setFont(QFont(UI_FONT, 9))
        painter.setPen(QColor('#617176'))
        subtitle = row.get('subtitle') or row.get('status', '')
        painter.drawText(int(art.x()), int(art.bottom() + 44), painter.fontMetrics().elidedText(subtitle, Qt.ElideRight, int(art.width())))
        painter.restore()

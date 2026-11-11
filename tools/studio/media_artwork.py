"""Bounded asynchronous artwork loading and cover painting."""
import hashlib
import threading
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PySide6.QtCore import QBuffer, QIODevice, QObject, QPointF, QRectF, QTimer, Qt, Slot
from PySide6.QtGui import (
    QColor,
    QImage,
    QImageReader,
    QLinearGradient,
    QPainter,
    QPainterPath,
    QPixmap,
)

from tools.content.music import COVER_NAMES, EXTENSIONS as MUSIC_EXTENSIONS

from .media_jobs import Events
from .media_widgets import ICONS, icon


class Covers(QObject):
    """Load only visible artwork; keep device IO off the GUI thread."""
    MAX_CACHE_BYTES = 64 * 1024 * 1024

    def __init__(self, parent):
        super().__init__(parent)
        self.cache, self.pending = OrderedDict(), {}
        self.cache_costs, self.cache_bytes = {}, 0
        self.sidecars = OrderedDict()
        self.sidecar_lock = threading.Lock()
        self.generation = 0
        self.closed = False
        self.pool = ThreadPoolExecutor(max_workers=3, thread_name_prefix='linnet-art')
        self.events = Events(self)
        self.events.artwork.connect(self.receive)
        self.views = []
        self.repaint_queued = False

    def get(self, path, source=None):
        if not path or self.closed:
            return None
        if path in self.cache:
            self.cache.move_to_end(path)
            return self.cache[path]
        # Completions repaint the views, which request any still-visible covers
        # again. Keep rapid scrolling from queuing an entire library of reads.
        if path not in self.pending and len(self.pending) < 6:
            self.pending[path] = self.pool.submit(self.read, path, source, self.generation)
        return None

    def invalidate(self, paths=None):
        with self.sidecar_lock:
            self.generation += 1
            self.sidecars.clear()
        for future in self.pending.values():
            future.cancel()
        self.pending.clear()
        if paths is None:
            self.cache.clear()
            self.cache_costs.clear()
            self.cache_bytes = 0
        else:
            for path in paths:
                self.cache.pop(path, None)
                self.cache_bytes -= self.cache_costs.pop(path, 0)

    def read(self, path, source_path, generation):
        if self.closed or generation != self.generation:
            return
        image = QImage()
        try:
            source = Path(path)
            if source.suffix.lower() == '.rgb' or source.suffix.lower() in MUSIC_EXTENSIONS:
                source_path = source_path or (path if source.suffix.lower() in MUSIC_EXTENSIONS else None)
                if source_path:
                    folder = Path(source_path).parent
                    original = self.sidecar(folder, generation)
                    if original and original.stat().st_size <= 32 * 1024 * 1024:
                        image = self.read_image(str(original))
                    if image.isNull():
                        from tools.content.music import _embedded_cover
                        embedded = _embedded_cover(Path(source_path))
                        if embedded:
                            buffer = QBuffer(); buffer.setData(embedded); buffer.open(QIODevice.ReadOnly)
                            image = self.read_image(buffer)
                if image.isNull() and source.suffix.lower() == '.rgb':
                    with source.open('rb') as reader:
                        pixels = reader.read(108041)
                    # RGB565 covers may append exact Now Playing renditions.
                    # Preview their leading square; reject unknown file sizes.
                    side = {8192: 64, 32768: 128, 51200: 160, 76288: 160, 108040: 160}.get(len(pixels))
                    if side:
                        image = QImage(pixels[:side * side * 2], side, side, side * 2, QImage.Format_RGB16).copy()
            else:
                image = self.read_image(path)
        except (OSError, ValueError):
            pass
        if not self.closed:
            self.events.artwork.emit(generation, path, image)

    def sidecar(self, folder, generation):
        """Share folder lookups within one artwork generation."""
        with self.sidecar_lock:
            if self.closed or generation != self.generation:
                return None
            if folder in self.sidecars:
                original = self.sidecars.pop(folder)
                if original is None or original.is_file():
                    self.sidecars[folder] = original
                    return original
            original = next((folder / name for name in COVER_NAMES if (folder / name).is_file()), None)
            self.sidecars[folder] = original
            if len(self.sidecars) > 256:
                self.sidecars.popitem(last=False)
            return original

    @staticmethod
    def read_image(source):
        reader = QImageReader(source); reader.setAutoTransform(True)
        dimensions = reader.size()
        if dimensions.isValid() and dimensions.width() * dimensions.height() <= 32_000_000:
            if dimensions.width() > 320 or dimensions.height() > 320:
                reader.setScaledSize(dimensions.scaled(320, 320, Qt.KeepAspectRatio))
            return reader.read()
        return QImage()

    @Slot(int, str, object)
    def receive(self, generation, path, image):
        if generation != self.generation:
            return
        self.pending.pop(path, None)
        self.cache_bytes -= self.cache_costs.pop(path, 0)
        picture = QPixmap.fromImage(image) if not image.isNull() else None
        cost = picture.width() * picture.height() * 4 if picture else 0
        self.cache[path] = picture
        self.cache_costs[path] = cost
        self.cache_bytes += cost
        self.cache.move_to_end(path)
        while len(self.cache) > 600 or self.cache_bytes > self.MAX_CACHE_BYTES:
            oldest, _ = self.cache.popitem(last=False)
            self.cache_bytes -= self.cache_costs.pop(oldest)
        if not self.repaint_queued:
            self.repaint_queued = True
            QTimer.singleShot(0, self.repaint_views)

    def repaint_views(self):
        self.repaint_queued = False
        for view in self.views:
            if view.isVisible():
                view.viewport().update()

def paint_cover(painter, rect, row, covers, rounded=9):
    if rect.isEmpty():
        return
    painter.save()
    clip = QPainterPath()
    clip.addRoundedRect(QRectF(rect), rounded, rounded)
    painter.setClipPath(clip)
    picture = covers.get(row.get('artwork', ''), row.get('cover_source') or row.get('path'))
    if picture:
        scale = max(rect.width() / picture.width(), rect.height() / picture.height())
        source = QRectF(0, 0, rect.width() / scale, rect.height() / scale)
        source.moveCenter(QPointF(picture.width() / 2, picture.height() / 2))
        painter.setRenderHint(QPainter.SmoothPixmapTransform)
        painter.drawPixmap(QRectF(rect), picture, source)
    else:
        colors = [('#9c8cce', '#615285'), ('#8fbdba', '#457676'), ('#d4ac9a', '#926769'), ('#a1afce', '#546884')]
        a, b = colors[hashlib.sha256(row['title'].encode()).digest()[0] % len(colors)]
        gradient = QLinearGradient(rect.topLeft(), rect.bottomRight())
        gradient.setColorAt(0, QColor(a)); gradient.setColorAt(1, QColor(b))
        painter.fillRect(rect, gradient)
        glyph = row.get('kind', 'Music').lower()
        if glyph not in ICONS:
            glyph = 'music'
        extent = min(42, int(rect.width() * .34))
        icon(glyph, '#ffffff', extent).paint(painter, int(rect.center().x() - extent / 2), int(rect.center().y() - extent / 2), extent, extent)
    painter.restore()

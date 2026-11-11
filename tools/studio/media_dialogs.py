"""Screenshot preview and live setup instruction dialogs."""
from datetime import datetime
from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtGui import QImage, QKeySequence, QPixmap, QShortcut
from PySide6.QtWidgets import QApplication, QDialog, QFileDialog, QLabel, QSizePolicy

from .media_widgets import box, button, label


class ScreenshotPreview(QLabel):
    def __init__(self, image):
        super().__init__()
        self.original = QPixmap.fromImage(image)
        self.setAlignment(Qt.AlignCenter); self.setMinimumSize(160,128)
        self.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Ignored)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.setPixmap(self.original.scaled(self.contentsRect().size(), Qt.KeepAspectRatio, Qt.FastTransformation))

class ScreenshotDialog(QDialog):
    """Keep a captured image available even if the iPod disconnects."""
    def __init__(self, result, parent):
        super().__init__(parent)
        self.setWindowTitle('iPod screenshot')
        self.resize(720, 620); self.setMinimumSize(400, 360)
        self.image = QImage.fromData(result['png'], 'PNG')
        if self.image.isNull():
            raise ValueError('The screenshot could not be opened.')
        self.settings = parent.settings
        self.filename = 'linnet-screenshot-' + datetime.now().strftime('%Y%m%d-%H%M%S') + '.png'
        layout = box(self, margins=(24,24,24,24), spacing=14)
        layout.addWidget(label('iPod screenshot', 'section'))
        layout.addWidget(label(f'{self.image.width()} × {self.image.height()} pixels · PNG', 'muted'))
        layout.addWidget(ScreenshotPreview(self.image), 1)
        self.message = label('', 'muted', True); layout.addWidget(self.message)
        actions = box(horizontal=True)
        actions.addWidget(button('Copy image', self.copy_image))
        actions.addWidget(button('Save PNG…', self.save_image, 'primary'))
        actions.addStretch(); actions.addWidget(button('Close', self.accept)); layout.addLayout(actions)
        QShortcut(QKeySequence.Copy, self, activated=self.copy_image)
        QShortcut(QKeySequence.Save, self, activated=self.save_image)

    def copy_image(self):
        QApplication.clipboard().setImage(self.image)
        self.message.setText('Screenshot copied to the clipboard.')

    def save_image(self):
        folder = Path(str(self.settings.value('screenshots/folder', str(Path.home() / 'Pictures'))))
        if not folder.is_dir():folder = Path.home()
        picker = QFileDialog(self, 'Save screenshot', str(folder / self.filename), 'PNG image (*.png)')
        picker.setAcceptMode(QFileDialog.AcceptSave); picker.setDefaultSuffix('png')
        if picker.exec() != QDialog.Accepted:return
        path = Path(picker.selectedFiles()[0])
        if not self.image.save(str(path), 'PNG'):
            self.message.setText('Could not save the screenshot. Choose another folder and try again.')
            return
        self.settings.setValue('screenshots/folder', str(path.parent))
        self.message.setText(f'Saved {path.name}.')

class SetupInstructionDialog(QDialog):
    """Keep the live button instructions visible until setup advances."""
    def reject(self):
        # Escape and the window close shortcut must not hide the release cue.
        # The explicit Stop button requests cancellation through the worker.
        pass

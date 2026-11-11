"""Settings rows and contextual actions for the desktop Media Manager."""
import sys

from PySide6.QtCore import Qt, QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import QFrame, QSizePolicy, QToolButton, QWidget

from tools.content import apple_font, host_cache

from .media_widgets import CheckBox, ComboBox, ControlRow as SettingRow, action_menu as _action_menu, box, button, label


def _card(layout, title):
    card = QFrame()
    card.setObjectName('card')
    content = box(card, margins=(20, 18, 20, 18), spacing=16)
    content.addWidget(label(title, 'cardTitle', True))
    layout.addWidget(card)
    return content


def _muted(text=''):
    item = label(text, 'muted', True)
    item.setMinimumWidth(0)
    item.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
    return item


def _row(layout, title, description, control):
    if layout.count() > 1:
        divider = QFrame()
        divider.setObjectName('settingsDivider')
        divider.setFixedHeight(1)
        layout.addWidget(divider)
    text = QWidget()
    details = box(text, spacing=5)
    details.addWidget(label(title, 'settingsLabel', True))
    if description is not None:
        details.addWidget(description)
    layout.addWidget(SettingRow(text, control))
    return details


def _advanced(layout):
    frame = QFrame()
    frame.setObjectName('card')
    options = box(frame, margins=(20, 15, 20, 15), spacing=12)
    toggle = QToolButton()
    toggle.setObjectName('settingsDisclosure')
    toggle.setText('Advanced preparation')
    toggle.setCheckable(True)
    toggle.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
    toggle.setArrowType(Qt.RightArrow)
    toggle.setFocusPolicy(Qt.StrongFocus)
    toggle.setCursor(Qt.PointingHandCursor)
    content = QWidget()
    content.hide()
    toggle.toggled.connect(content.setVisible)
    toggle.toggled.connect(lambda expanded: toggle.setArrowType(Qt.DownArrow if expanded else Qt.RightArrow))
    options.addWidget(toggle, 0, Qt.AlignLeft)
    options.addWidget(content)
    layout.addWidget(frame)
    return toggle, box(content, spacing=10)


def build_settings_page(window):
    page, outer = window.scroll_page()
    content = QWidget()
    content.setMaximumWidth(960)
    layout = box(content, spacing=16)
    outer.addWidget(content)
    outer.addStretch()
    heading = box(spacing=5)
    heading.addWidget(label('Settings', 'title'))
    heading.addWidget(label('Media preparation, iPod appearance and computer storage.', 'subtitle', True))
    layout.addLayout(heading)
    layout.addSpacing(4)

    prepare = _card(layout, 'Media preparation')
    window.photo_detail = ComboBox()
    window.photo_detail.setAccessibleName('Photo detail')
    window.photo_detail.addItems(['Standard · 640 px', 'High detail · 2048 px'])
    window.photo_detail.setCurrentIndex(int(window.settings.value('photo_detail', 0)))
    window.photo_detail.setFixedWidth(240)
    window.photo_detail.currentIndexChanged.connect(lambda value: window.settings.setValue('photo_detail', value))
    _row(prepare, 'Photo detail', _muted('Higher detail uses more iPod storage and takes longer to prepare.'), window.photo_detail)
    window.convert_audio = CheckBox('Convert to WAV')
    window.convert_audio.setChecked(str(window.settings.value('convert_audio', True)).lower() in ('true', '1'))
    window.convert_audio.toggled.connect(lambda checked: window.settings.setValue('convert_audio', checked))
    _row(prepare, 'Music playback', _muted('Decode compressed music on this computer for faster iPod playback and navigation. WAV files use more iPod storage.'), window.convert_audio)
    window.online_artwork = CheckBox('Find online')
    window.online_artwork.setChecked(window.settings.value('onlineArtwork', True, type=bool))
    window.online_artwork.toggled.connect(lambda enabled: window.settings.setValue('onlineArtwork', enabled))
    _row(prepare, 'Album artwork', _muted('Find missing iTunes album covers using MusicBrainz and Cover Art Archive.'), window.online_artwork)
    appearance = _card(layout, 'iPod appearance')
    window.apple_font_status = _muted('Connect a color iPod in Disk Mode to choose its font.')
    window.font_chooser, font_actions = _action_menu('Choose font', (
        ('Use included font', window.remove_apple_font),
        ('Download and use Apple font (3.8 MB)', window.install_apple_font),
    ), minimum_width=170)
    window.apple_font_remove, window.apple_font_install = font_actions
    font = _row(appearance, 'Theme font', window.apple_font_status, window.font_chooser)
    terms = _muted()
    terms.setTextFormat(Qt.RichText)
    terms.setText('Apple Podium Sans is downloaded from Apple. '
                  f'<a href="{apple_font.LICENSE_URL}" style="color:#63757b">Apple software license</a>.')
    terms.setTextInteractionFlags(Qt.LinksAccessibleByMouse | Qt.LinksAccessibleByKeyboard)
    terms.linkActivated.connect(lambda url: QDesktopServices.openUrl(QUrl(url)))
    font.addWidget(terms)

    storage = _card(layout, 'Computer storage')
    window.cache_chooser, cache_actions = _action_menu('Manage cache', (
        ('Open cache folder', window.open_computer_folder),
        ('Refresh size', window.refresh_cache),
        (None, None),
        ('Change location…', window.change_cache_location),
        ('Clear cache…', window.clear_computer_cache),
    ), minimum_width=170)
    window.cache_open, window.cache_refresh, window.cache_change, window.cache_clear = cache_actions
    window.cache_buttons = tuple(cache_actions)
    window.cache_size = _muted('Cache size has not been checked.')
    cache = _row(storage, 'Media cache', window.cache_size, window.cache_chooser)
    window.cache_location = _muted()
    window.cache_location.setTextInteractionFlags(Qt.TextSelectableByMouse)
    window.cache_location.setAccessibleName('Cache location')
    cache.addWidget(window.cache_location)
    cache.addWidget(_muted('Reusable media files speed up preparation.'))
    window.backups_chooser, backup_actions = _action_menu('Open folder', (
        ('Startup backups', lambda: window.open_computer_folder(host_cache.storage_root() / 'Installer')),
        ('Recovery and saved state', lambda: window.open_computer_folder(host_cache.media_state_root())),
    ), minimum_width=170)
    window.backups_open, window.recovery_open = backup_actions
    window.cache_protected = _muted('Startup backups, recovery copies and saved selections are kept when you clear or move the cache.')
    _row(storage, 'Backups and recovery', window.cache_protected, window.backups_chooser)

    window.advanced_toggle, details = _advanced(layout)
    window.tools_label = None
    if not getattr(sys, 'frozen', False):
        window.tools_label = _muted('Checking FFmpeg and FFprobe…')
        tools = button('Choose folder…', window.locate_tools, None, 'folder')
        tools.setMinimumWidth(170)
        tools.setToolTip('Choose FFmpeg and FFprobe when running Media Manager from source.')
        _row(details, 'FFmpeg (source builds)', window.tools_label, tools)
    details.addWidget(_muted('These options stay active until you change them or close Media Manager.'))
    window.verify_audio = CheckBox('Verify complete audio files')
    details.addWidget(window.verify_audio, 0, Qt.AlignLeft)
    details.addWidget(_muted('Read entire audio files for a thorough check. Takes longer.'))
    window.rebuild = CheckBox('Rebuild prepared media')
    details.addWidget(window.rebuild, 0, Qt.AlignLeft)
    details.addWidget(_muted('Prepare files again even when they are already up to date.'))
    layout.addWidget(button('Help and About', window.show_help, 'quiet'), 0, Qt.AlignLeft)
    window.update_tools()
    return page

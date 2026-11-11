"""Linnet's native media library and device workspace."""
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PySide6.QtCore import QLoggingCategory, QSettings, QSignalBlocker, QSize, QTimer, Qt, Slot
from PySide6.QtGui import QFont, QIcon, QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QBoxLayout,
    QDialog,
    QFrame,
    QHeaderView,
    QMainWindow,
    QProgressBar,
    QScrollArea,
    QStackedWidget,
    QTableView,
    QWidget,
)

from tools.content import host_cache, media_manager as media, media_trash
from tools.content.media_manager import format_size as size
from tools.studio import installer, screen_settings
from tools.studio.media_identity import ICON_PATH, activate_window, configure_taskbar

from .media_artwork import Covers, paint_cover as paint_cover
from .media_devices import DeviceWorkspace
from .media_dialogs import (
    ScreenshotDialog as ScreenshotDialog,
    ScreenshotPreview as ScreenshotPreview,
    SetupInstructionDialog,
)
from .media_drafts import DraftWorkspace
from .media_editors import EditorWorkspace, GamesController, ScreensController
from .media_jobs import Events, OperationContext, OperationOutcome, operation_status
from .media_library import LibraryWorkspace
from .media_models import (
    Artwork as Artwork,
    CardDelegate as CardDelegate,
    MediaModel,
    TrackDelegate as TrackDelegate,
    library_search_text as library_search_text,
    music_group_key as music_group_key,
)
from .media_pages import (
    build_games_page,
    build_library_page,
    build_preparation_page,
    build_screens_page,
    build_setup_page,
)
from .media_preparation import PreparationWorkspace
from .media_settings import SettingsWorkspace, linnet_version as linnet_version
from .media_settings_page import build_settings_page
from .media_setup import SetupWorkspace
from .media_widgets import (
    CheckBox as CheckBox,
    ComboBox as ComboBox,
    ICONS as ICONS,
    StatusLabel,
    UI_FONT,
    _boot_artwork_source as _boot_artwork_source,
    boot_artwork,
    box,
    button,
    icon,
    label,
)


class MediaWindow(
    LibraryWorkspace, PreparationWorkspace, DeviceWorkspace, DraftWorkspace,
    SetupWorkspace, SettingsWorkspace, EditorWorkspace, QMainWindow,
):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('Linnet · Media Manager')
        self.resize(1280, 860)
        self.setMinimumSize(780, 540)
        self.setWindowIcon(QIcon(str(ICON_PATH)))
        self.settings = QSettings('Linnet', 'MediaManager')
        self.identity, self.inventory, self.review = None, None, None
        self.last_preparation = None
        self.rows, self.readiness, self.pending, self.trash_rows = [], {}, [], []
        self.trash_summary = None
        self.pending_keys = set()
        self.queue_outcomes = {}
        self.draft_issues = {}
        self.stale_videos = 0
        self.queue_context = None
        self.discovery_errors = []
        self.hardware_report = None
        self.device_connected = False
        self.publication_recovery_pending = False
        self.publication_recovery_groups = []
        self._recent_private_identifiers = set()
        self.screen_saved = None
        self.screen_dirty = False
        self.game_saved = None
        self.game_dirty = False
        self.game_read_failed = False
        self.apple_info = None
        self.apple_font_result = None
        self._library_source, self._library_cache, self._library_stats = None, {}, {}
        self._library_search = None
        self.video_selection, self.video_mode, self.stop_event = set(), 'all', threading.Event()
        self.page, self.view, self.group = 'Music', self.settings.value('music_view', 'Albums'), None
        if self.view not in ('Songs', 'Albums', 'Artists', 'Genres'):
            self.view = 'Albums'
        self.busy = False
        self.setup_instruction_dialog = None
        self.link_connection = None
        self.closed = False
        self.cache_info = None
        self.cache_library_dirty = False
        self.operation_id = 0
        self.active_operation = None
        self.pool = ThreadPoolExecutor(max_workers=1, thread_name_prefix='linnet-media')
        self.tool_pool = ThreadPoolExecutor(max_workers=1, thread_name_prefix='linnet-tools')
        self.tool_check_id = 0
        self.verified_ffmpeg = None
        self.verified_ffprobe = None
        self.events = Events(self)
        self.events.done.connect(self.completed)
        self.events.progress.connect(self.show_progress)
        self.events.stage.connect(self.show_operation_stage)
        self.events.conversion.connect(self.show_conversion_progress)
        self.events.tools_checked.connect(self.tools_checked)
        self.events.setup_instruction.connect(self.show_setup_instruction)
        self.covers = Covers(self)
        self.ffmpeg = self.settings.value('ffmpeg', '')
        self.ffprobe = self.settings.value('ffprobe', '')
        self.screens_controller = ScreensController(self)
        self.games_controller = GamesController(self)
        self.build()
        self.setStyleSheet(Path(__file__).with_suffix('.qss').read_text(encoding='utf-8'))
        self.update_preparation()
        self.navigate('Music')
        QTimer.singleShot(100, self.discover)

    def build(self):
        shell = QWidget(); self.setCentralWidget(shell)
        main = box(shell, horizontal=True, spacing=0)
        sidebar = QWidget(); sidebar.setObjectName('sidebar'); sidebar.setMinimumWidth(232)
        side = box(sidebar, margins=(18, 20, 18, 16), spacing=6)
        brand = box(horizontal=True, spacing=11)
        mark = label(); mark.setPixmap(boot_artwork((594, 180, 310, 445), 38, 56))
        wordmark = label(); wordmark.setPixmap(boot_artwork((548, 648, 442, 64), 132, 20))
        brand.addWidget(mark); brand.addWidget(wordmark); brand.addStretch()
        side.addLayout(brand); side.addSpacing(18)
        side.addWidget(label('LIBRARY', 'eyebrow')); side.addSpacing(7)
        self.navigation = {}
        for name, glyph in [('Music', 'music'), ('Photos', 'photos'), ('Videos', 'videos'), ('Notes', 'notes'),
                            ('iPod setup', 'setup'), ('Preparation', 'prepare'), ('Games', 'setup'), ('Screens', 'settings'), ('Trash', 'trash'), ('Settings', 'settings')]:
            if name == 'iPod setup':
                side.addSpacing(18); side.addWidget(label('DEVICE', 'eyebrow')); side.addSpacing(7)
            item = button(name, lambda _=False, n=name: self.navigate(n), 'nav')
            item.setIcon(icon(glyph, '#a8bbc1')); item.setIconSize(QSize(19, 19)); item.setCheckable(True)
            item.toggled.connect(lambda checked, glyph=glyph, item=item:
                                 item.setIcon(icon(glyph, '#102b32' if checked else '#a8bbc1')))
            self.navigation[name] = item; side.addWidget(item)
            if name == 'Trash':
                self.trash_badge = label('', 'trashBadge')
                self.trash_badge.setAlignment(Qt.AlignCenter)
                self.trash_badge.setAttribute(Qt.WA_TransparentForMouseEvents)
                self.trash_badge.setFixedHeight(20); self.trash_badge.setMinimumWidth(22)
                badge_layout = box(item, horizontal=True, margins=(0, 0, 10, 0), spacing=0)
                badge_layout.addStretch(); badge_layout.addWidget(self.trash_badge)
                self.trash_badge.hide()
        side.addStretch()
        device = QFrame(); device.setObjectName('deviceCard')
        layout = box(device, margins=(13, 17, 13, 15), spacing=9)
        self.device_name = label('Connect your iPod', 'deviceName', True)
        self.device_info = label('USB · Disk Mode', 'deviceInfo', True)
        self.capacity = QProgressBar(); self.capacity.setTextVisible(False); self.capacity.setRange(0, 100); self.capacity.setValue(0)
        layout.addWidget(self.device_name); layout.addWidget(self.device_info); layout.addWidget(self.capacity)
        self.device_details_button = button('Device details', self.device_details, 'sidebarAction', 'setup')
        layout.addWidget(self.device_details_button)
        self.screenshot_button = button('Screenshot…', self.capture_screenshot, 'sidebarAction', 'camera')
        self.screenshot_button.setEnabled(False)
        layout.addWidget(self.screenshot_button)
        self.choose_device_button = button('Find or switch iPod', self.discover, 'sidebarAction', 'refresh')
        layout.addWidget(self.choose_device_button)
        self.eject_button = button('Eject safely', self.eject, 'sidebarAction', 'eject'); layout.addWidget(self.eject_button)
        self.publication_recovery_button = button('Recover media indexes', self.recover_publication, 'sidebarAction')
        layout.addWidget(self.publication_recovery_button)
        side.addWidget(device)
        sidebar_scroll = QScrollArea(); sidebar_scroll.setObjectName('sidebarScroll')
        sidebar_scroll.setWidgetResizable(True); sidebar_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        sidebar_scroll.setFixedWidth(240); sidebar_scroll.setWidget(sidebar)
        main.addWidget(sidebar_scroll)
        right = QWidget(); right.setObjectName('workspace'); body = box(right, spacing=0)
        self.stack = QStackedWidget(); body.addWidget(self.stack, 1)
        self.library = build_library_page(self); self.stack.addWidget(self.library)
        self.preparation = build_preparation_page(self); self.stack.addWidget(self.preparation)
        self.setup = build_setup_page(self); self.stack.addWidget(self.setup)
        self.screens = build_screens_page(self); self.stack.addWidget(self.screens)
        self.games_page = build_games_page(self); self.stack.addWidget(self.games_page)
        self.preferences = build_settings_page(self); self.stack.addWidget(self.preferences)
        activity = QFrame(); activity.setObjectName('activity'); foot = box(activity, margins=(28, 10, 28, 12), spacing=7)
        self.progress = QProgressBar(); self.progress.setTextVisible(False); self.progress.hide(); foot.addWidget(self.progress)
        self.status_row = box(horizontal=True, spacing=12)
        self._activity_message = 'Looking for your iPod…'
        self.status = StatusLabel(self._activity_message); self.status_row.addWidget(self.status, 1)
        self.status_actions = QWidget(); actions = box(self.status_actions, horizontal=True, spacing=12)
        self.cancel_button = button('Stop', self.stop, 'quiet'); self.cancel_button.setToolTip('Stop after the current step finishes'); self.cancel_button.hide(); actions.addWidget(self.cancel_button)
        self.refresh_button = button('Refresh', self.refresh, 'quiet', 'refresh'); actions.addWidget(self.refresh_button)
        self.diagnostics_button = button('Diagnostics', self.show_diagnostics, 'quiet'); actions.addWidget(self.diagnostics_button)
        self.status_row.addWidget(self.status_actions)
        foot.addLayout(self.status_row); body.addWidget(activity)
        main.addWidget(right, 1)
        QShortcut(QKeySequence.Find, self, activated=self.focus_search)
        QShortcut(QKeySequence.Refresh, self, activated=self.refresh)
        for view in (self.table, self.grid):
            shortcut = QShortcut(QKeySequence('Delete'), view, activated=self.remove_selected)
            shortcut.setContext(Qt.WidgetWithChildrenShortcut)

    def heading(self, layout, title, subtitle):
        layout.addWidget(label(title, 'title'))
        layout.addWidget(label(subtitle, 'subtitle', True))
        layout.addSpacing(16)

    def scroll_page(self):
        scroll = QScrollArea(); scroll.setWidgetResizable(True)
        content = QWidget(); content.setObjectName('pageContent')
        layout = box(content, margins=(34, 32, 34, 28), spacing=14)
        scroll.setWidget(content)
        return scroll, layout

    def navigate(self, page):
        changed = self.page != page
        self.page = page
        for name, item in self.navigation.items():
            item.setChecked(name == page)
        if page in media.KINDS or page == 'Trash':
            if changed:
                self.group = None; self.clear_filters(render=False)
            self.stack.setCurrentWidget(self.library)
            self.render_library()
            if page == 'Trash' and self.identity and self.device_connected and not self.busy:
                identity = self.identity
                self.work('trash', lambda: media_trash.inventory(identity), 'Reading Trash…')
        elif page == 'Preparation':
            self.stack.setCurrentWidget(self.preparation)
        elif page == 'Screens':
            self.stack.setCurrentWidget(self.screens); self.screens_controller.load_screens()
        elif page == 'Games':
            self.stack.setCurrentWidget(self.games_page); self.games_controller.load_games()
        elif page == 'iPod setup':
            self.stack.setCurrentWidget(self.setup)
            if self.identity and self.review is not None:
                self.render_setup()
                if self.review is not None and not self.busy:
                    self.show_progress('Device setup is ready.')
            elif self.identity and self.device_connected and not self.busy:
                identity = self.identity
                self.work('setup', lambda: installer.assess(identity), 'Checking installed software…')
        else:
            self.stack.setCurrentWidget(self.preferences)
            self.refresh_cache()
        self.update_controls()
        self.render_activity_status()
        # Link setup leaves the collection unread. Setup and Trash have their
        # own readers; returning to removal must not wait for a media scan.
        if (self.cache_library_dirty and page not in ('Settings', 'iPod setup', 'Trash') and self.identity and
                self.device_connected and not self.busy):
            self.refresh()

    def dialog(self, title, message, action='Continue', danger=False, cancel=True, secondary=None):
        dialog = QDialog(self); dialog.setWindowTitle(title); dialog.setMinimumWidth(460)
        layout = box(dialog, margins=(28, 28, 28, 24), spacing=18)
        layout.addWidget(label(title, 'section')); layout.addWidget(label(message, None, True))
        actions = box(horizontal=True); actions.addStretch()
        if secondary:
            actions.addWidget(button(secondary[0], lambda: dialog.done(2)))
        if cancel:
            actions.addWidget(button('Cancel', dialog.reject))
        actions.addWidget(button(action, dialog.accept, 'danger' if danger else 'primary')); layout.addLayout(actions)
        result = dialog.exec()
        if result == 2 and secondary:
            secondary[1]()
        return result == QDialog.Accepted

    def confirm_trash(self, rows):
        dialog = QDialog(self); dialog.setObjectName('trashDialog')
        dialog.setWindowTitle('Move to iPod Trash'); dialog.resize(720, min(550, 340 + len(rows) * 42))
        dialog.setMinimumSize(580, 380)
        layout = box(dialog, margins=(28, 26, 28, 24), spacing=16)
        heading = box(horizontal=True, spacing=14)
        symbol = label(); symbol.setPixmap(icon('trash', '#167489', 30).pixmap(30, 30))
        heading.addWidget(symbol); heading.addWidget(label('Move to iPod Trash?', 'dialogTitle')); heading.addStretch()
        layout.addLayout(heading)
        noun = 'file' if len(rows) == 1 else 'files'
        layout.addWidget(label(f'{len(rows):,} {noun} selected · {size(sum(row["bytes"] for row in rows))}', 'trashSummary'))
        preview = QTableView(); preview.setObjectName('trashPreview')
        model = MediaModel(preview)
        model.reset([dict(row, filename=Path(row['path']).name) for row in rows],
                    [('title', 'TITLE'), ('filename', 'FILE'), ('bytes', 'SIZE')])
        preview.setModel(model); preview.setSortingEnabled(True)
        preview.setEditTriggers(QAbstractItemView.NoEditTriggers)
        preview.setSelectionBehavior(QAbstractItemView.SelectRows)
        preview.setShowGrid(False); preview.setWordWrap(False)
        preview.verticalHeader().hide(); preview.verticalHeader().setDefaultSectionSize(42)
        preview.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        preview.horizontalHeader().setSectionResizeMode(1, QHeaderView.Stretch)
        preview.setColumnWidth(2, 95); preview.sortByColumn(0, Qt.AscendingOrder)
        layout.addWidget(preview, 1)
        note = QFrame(); note.setObjectName('recoveryNote')
        note_layout = box(note, margins=(16, 13, 16, 13), spacing=5)
        note_layout.addWidget(label('You can restore these files', 'cardTitle'))
        note_layout.addWidget(label('Find them in Trash on this iPod. Playback libraries update automatically. Storage is freed when you empty Trash.', None, True))
        layout.addWidget(note)
        actions = box(horizontal=True, spacing=10); actions.addStretch()
        cancel = button('Keep files', dialog.reject); cancel.setDefault(True)
        move = button('Move to Trash', dialog.accept, 'danger', 'trash'); move.setAutoDefault(False)
        actions.addWidget(cancel); actions.addWidget(move); layout.addLayout(actions)
        cancel.setFocus()
        return dialog.exec() == QDialog.Accepted

    def work(self, tag, operation, message):
        if self.busy or self.closed:
            return
        self.operation_id += 1
        context = OperationContext(self.operation_id, tag,
            dict(self.identity) if self.identity and tag not in ('devices','hardware_report') and not tag.startswith('cache_') else None,
            self.stop_event if tag in ('prepare', 'remove', 'restore', 'scan', 'plan', 'devices', 'hardware_report', 'apple_update', 'classic_install', 'apple_font_install') else None)
        self.active_operation = context
        self.busy = True
        self.stop_event.clear()
        self.progress.setRange(0, 0)
        self.progress.show()
        self.cancel_button.setEnabled(True)
        self.cancel_button.setText('Stop')
        self.cancel_button.setToolTip('Stop reading the iPod' if tag in ('scan', 'devices', 'hardware_report') else
                                      'Stop checking the preparation plan' if tag == 'plan' else
                                      'Stop after the current step finishes')
        self.cancel_button.setVisible(context.can_cancel)
        self.show_progress(message)
        self.update_controls()
        def run():
            try:
                with host_cache.activity():
                    value = operation()
                outcome = OperationOutcome(context, value, status=operation_status(value),
                    cancel_requested=bool(context.cancel_event and context.cancel_event.is_set()))
            except Exception as error:
                outcome = OperationOutcome(context,
                    status='cancelled' if type(error).__name__ == 'Cancelled' else 'failed',
                    error=str(error) or type(error).__name__,
                    error_type=getattr(error, 'error_type', type(error).__name__),
                    cancel_requested=bool(context.cancel_event and context.cancel_event.is_set()))
            self.events.done.emit(outcome)
        self.pool.submit(run)

    @Slot(str)
    def show_progress(self, text):
        self._activity_message = text
        self.render_activity_status()

    def render_activity_status(self):
        expanded = bool(self.page == 'iPod setup' and self.busy and self.active_operation
                        and self.active_operation.tag == 'scan')
        self.status_row.setDirection(QBoxLayout.TopToBottom if expanded else QBoxLayout.LeftToRight)
        self.status_row.setAlignment(self.status_actions, Qt.AlignRight | Qt.AlignVCenter)
        self.status.setWordWrap(expanded)
        self.status.set_message(self._activity_message +
            ('\nLarge collections take longer, especially on the first read.' if expanded else ''))

    @Slot(object)
    def show_operation_stage(self, event):
        if not self.busy:
            return
        fraction = min(1.0, max(0.0, event.get('fraction', 0)))
        value = min(999, round(1000 * (event['completed'] + fraction) / event['total']))
        self.progress.setRange(0, 1000)
        self.progress.setValue(value)
        context = self.active_operation
        can_stop = bool(context and context.cancel_event and event['can_stop']
                        and not context.cancel_event.is_set())
        self.cancel_button.setEnabled(can_stop)
        self.cancel_button.setText('Stop' if can_stop else 'Finishing current step')
        self.cancel_button.setToolTip('Stop after the current step finishes' if can_stop else
                                      'The current guarded write or cleanup must finish.')
        self.show_progress(event['name'])

    @Slot(object)
    def show_conversion_progress(self, event):
        def duration(seconds):
            minutes, seconds = divmod(max(0, round(seconds)), 60)
            return f'{minutes}:{seconds:02}'
        stage = 'Converting' if event['stage'] == 'convert' else 'Checking'
        parts = []
        if event.get('percent') is not None:
            parts.append(f"{event['percent']:.0f}%")
        eta = event.get('eta_seconds')
        parts.append(f'~{duration(eta)} left' if eta is not None else 'Estimating time')
        parts.append(f"{duration(event['elapsed_seconds'])} elapsed")
        parts.append(stage)
        if event.get('video_total', 1) > 1:
            parts.append(f"Video {event['video_index']}/{event['video_total']}")
        if event.get('name'):
            parts.append(event['name'])
        self.show_progress(' · '.join(parts))
        self.status.setToolTip(event['message'])

    @Slot(object)
    def completed(self, outcome):
        context = outcome.context
        if self.active_operation is None or context.id != self.active_operation.id:
            return
        self.active_operation = None
        tag, result, error = context.tag, outcome.value, outcome.error
        if tag in ('classic_install', 'uninstall'):
            self.close_setup_instruction()
        self.busy = False
        self.render_activity_status()
        self.progress.hide()
        self.cancel_button.hide()
        self.update_controls()
        cancel_requested = outcome.cancel_requested or bool(context.cancel_event and context.cancel_event.is_set())
        if cancel_requested and tag == 'hardware_report' and outcome.status != 'failed':
            self.show_progress('Stopped collecting the hardware report.')
            return
        if cancel_requested and tag in ('scan', 'plan', 'devices') and outcome.status != 'failed':
            if tag == 'plan':
                self.plan_context = None
            elif tag == 'scan' and isinstance(result, dict) and result.get('publication_recovery_pending'):
                self._complete_scan(result)
                self.show_progress('An interrupted media update needs review before this iPod can be used.')
                return
            elif tag == 'devices' and self.publication_recovery_pending:
                self.update_controls()
            else:
                self.transition_device(None)
            self.show_progress('Stopped reading the iPod. If iTunes is syncing, let it finish before refreshing.'
                               if tag in ('scan', 'devices') else
                               'Stopped checking the preparation plan. Refresh when ready.')
            return
        if outcome.status not in ('succeeded', 'partial', 'cancelled', 'failed'):
            self.show_error(f'Unknown background outcome: {outcome.status}')
            return
        if context.identity and not self.same_device(self.identity, context.identity):
            self.record_diagnostic(tag, 'DeviceChanged', 'The selected iPod changed during the operation.', 'failed')
            self.show_error('The selected iPod changed while this operation was running. Refresh and try again.')
            return
        if tag == 'apple_update' and outcome.status == 'cancelled':
            message = 'Apple library update stopped. Checking the current library…'
            self.prep_results.setPlainText(message); self.prep_results.show()
            self.show_progress(message); self.refresh()
            return
        if tag == 'classic_install' and outcome.status == 'cancelled':
            self.show_progress(error or 'Classic setup stopped before bootloader installation.')
            return
        if tag == 'apple_font_install' and outcome.status == 'cancelled':
            self.show_progress('Apple font download stopped. The iPod font was not changed.')
            return
        if error:
            self.record_diagnostic(tag, outcome.error_type or 'Error', error, outcome.status,
                                   cancel_requested)
        if tag == 'queue':
            self._complete_queue(result, error)
            return
        if error:
            self._complete_error(tag, error, outcome.error_type)
            return
        handlers = {
            'cache_size': self._complete_cache, 'cache_clear': self._complete_cache, 'cache_move': self._complete_cache,
            'screens': self._complete_screens, 'plan': self._complete_plan,
            'save_screens': self._complete_save_screens, 'games': self._complete_games,
            'save_games': self._complete_save_games, 'review_skipped': self._complete_review_skipped,
            'import_game': self._complete_installed_game, 'remove_game': self._complete_installed_game,
            'playlists': self._complete_playlists, 'save_playlists': self._complete_save_playlists,
            'apple_selection': self._complete_apple_selection, 'apple_update': self._complete_apple_update,
            'devices': self._complete_devices, 'scan': self._complete_scan,
            'hardware_report': self._complete_hardware_report,
            'video_recovery_options': self._complete_video_recovery_options,
            'recover_publication': self._complete_recover_publication,
            'trash': self._complete_trash, 'setup': self._complete_setup,
            'uninstall_trash': self._complete_uninstall_trash,
            'uninstall': self._complete_uninstall, 'uninstall_setup': self._complete_uninstall_setup,
            'link_setup': self._complete_link_setup,
            'prepare': self._complete_prepare, 'eject': self._complete_eject,
            'device_details': self._complete_device_details,
            'screenshot': self._complete_screenshot,
            'apple_font_install': self._complete_apple_font,
            'apple_font_remove': self._complete_apple_font,
        }
        for change in ('remove', 'restore', 'erase', 'sync', 'install', 'skipped',
                       'startup_default', 'boot_upgrade', 'restore_video_catalog', 'classic_install', 'link_disk'):
            handlers[change] = self._complete_change
        handler = handlers.get(tag)
        if handler is None:
            self.show_error(f'Unknown background operation: {tag}')
            return
        handler(result)
        if tag in ('cache_clear', 'cache_move'):
            self.covers.invalidate()
            self._library_source, self._library_cache, self._library_stats = None, {}, {}
            self._library_search = None
            self.cache_library_dirty = True
            if self.page != 'Settings' and self.identity and self.device_connected:
                QTimer.singleShot(0, self.refresh)
        elif not tag.startswith('cache_') and self.page == 'Settings' and self.cache_info is None:
            QTimer.singleShot(0, self.refresh_cache)
        if tag == 'scan':
            self.cache_library_dirty = False
        if tag not in ('screens', 'plan', 'save_screens', 'games', 'save_games', 'import_game', 'remove_game',
                       'review_skipped', 'playlists', 'save_playlists',
                       'apple_selection', 'apple_update'):
            self.update_controls()

    def _complete_error(self, tag, error, error_type=''):
        if tag == 'uninstall' and error_type == 'AppleStoragePending':
            # The saved identity belongs to the ejected Linnet connection. Do
            # not leave removal enabled against its old disk serial or volume.
            self.transition_device(None)
            self.device_name.setText('Startup restored · cleanup pending')
            self.device_info.setText('Connect the iPod’s USB disk, then choose Find iPod')
        if tag == 'classic_install':
            # The disk may have changed mode while setup was running. Keep the
            # host transaction reachable without offering stale install choices.
            self.review=None
            with QSignalBlocker(self.boot_choice),QSignalBlocker(self.method_choice):
                self.boot_choice.clear();self.method_choice.clear()
            self.update_controls()
        if tag == 'plan':
            self.plan_context = None
        if tag == 'screens':
            self.screen_state = screen_settings.defaults(self.identity)
            self.screen_saved = None
            self.screens_controller.render_screen_items()
            self.screen_status.setText('Could not read settings. Showing defaults for this iPod; saving will replace its screen settings.')
        if tag == 'games':
            self.game_read_failed = error_type=='GameCatalogueRecovery' or 'game selection is damaged' in error.casefold()
            self.games_status.setText('Game settings are damaged. Use Replace damaged game settings to choose a new selection.'
                                      if self.game_read_failed else 'Could not read game settings. Refresh or choose the iPod again.')
        if (tag in ('scan', 'devices', 'link_setup', 'link_disk', 'uninstall') and
                error_type != 'AppleStoragePending'):
            self.transition_device(None)
            self.device_name.setText('Connection needs checking')
            self.device_info.setText('Connection needs checking · choose Refresh or Choose iPod')
            self.update_controls()
        if tag in ('setup', 'uninstall_setup', 'devices', 'scan'):
            self.setup_progress.hide()
            self.setup_software.setText('Could not check iPod setup. Choose Refresh to try again.')
        self.show_error(error, alert=error_type == 'LinnetInstallationRequired')
        self.update_controls()
        if tag == 'save_playlists':
            self.edit_playlists(self.playlist_draft)
        return

    @Slot(object)
    def show_setup_instruction(self, event):
        if not self.busy or not self.active_operation or self.active_operation.tag not in ('classic_install', 'uninstall'):
            return
        removing = self.active_operation.tag == 'uninstall'
        if event.get('action') in ('hold','release','apple-disk'):
            if self.setup_instruction_dialog is None:
                dialog = SetupInstructionDialog(self)
                dialog.setWindowTitle('Finish removing Linnet' if removing else 'Connect your Classic for installation')
                dialog.setWindowFlag(Qt.WindowCloseButtonHint, False)
                dialog.setWindowModality(Qt.WindowModal)
                dialog.setMinimumWidth(480)
                dialog.setMaximumWidth(600)
                layout = box(dialog, margins=(28,28,28,28))
                dialog.heading = label('', 'section', True)
                dialog.message = label('', None, True)
                layout.addWidget(dialog.heading); layout.addWidget(dialog.message)
                layout.addWidget(label('Media Manager detects this automatically. Keep USB connected.', None, True))
                spinner = QProgressBar(); spinner.setRange(0,0); spinner.setTextVisible(False)
                layout.addWidget(spinner)
                dialog.stop_button = button('Stop installation', self.stop)
                layout.addWidget(dialog.stop_button)
                self.setup_instruction_dialog = dialog
            dialog = self.setup_instruction_dialog
            dialog.heading.setText('Waiting for the iPod’s Apple USB disk' if event['action']=='apple-disk' else
                                   'Release Menu + Select now' if event['action']=='release' else
                                   'Hold Menu + Select')
            dialog.message.setText(event['message'])
            dialog.stop_button.setVisible(bool(event['can_stop']))
            dialog.stop_button.setEnabled(bool(event['can_stop']) and not self.stop_event.is_set())
            dialog.show(); dialog.raise_()
            if not removing:
                self.install_detail.hide()
        else:
            self.close_setup_instruction()
            if not removing:
                self.install_detail.setText(event['message']); self.install_detail.show()
        if not removing:
            self.setup_progress.show()
        self.cancel_button.setVisible(bool(event['can_stop']))
        self.cancel_button.setEnabled(bool(event['can_stop']) and not self.stop_event.is_set())
        self.show_progress(event['message'])

    @Slot(int, object)
    def tools_checked(self, request, result):
        if self.closed or request != self.tool_check_id:
            return
        validated = result['validated']
        if result['candidate'] is not None:
            for name, setting in (('FFmpeg', 'ffmpeg'), ('FFprobe', 'ffprobe')):
                if name in validated:
                    setattr(self, setting, validated[name])
                    setattr(self, 'verified_' + setting, validated[name])
                    self.settings.setValue(setting, validated[name])
            if validated:
                if len(validated) != 2:
                    self.update_tools()
                    return
        else:
            for name, setting in (('FFmpeg', 'ffmpeg'), ('FFprobe', 'ffprobe')):
                value = validated.get(name)
                setattr(self, 'verified_' + setting, value)
                if value:
                    setattr(self, setting, value)
        if result['errors']:
            self.record_diagnostic('tools', 'ToolCheckError', '; '.join(result['errors']), 'failed')
            if self.tools_label is None:
                return
            available = (('FFprobe is ready. ' if self.verified_ffprobe else '') +
                         ('FFmpeg is ready. ' if self.verified_ffmpeg else ''))
            self.tools_label.setText(available + ' '.join(result['errors']) +
                ' Choose a folder containing the missing tool.')
            return
        if self.tools_label is not None:
            self.tools_label.setText('FFmpeg and FFprobe are ready.')

    def stop(self):
        context = self.active_operation
        if context and context.cancel_event:
            context.cancel_event.set()
            if self.setup_instruction_dialog is not None:
                self.setup_instruction_dialog.stop_button.setEnabled(False)
            self.cancel_button.setEnabled(False)
            self.show_progress('Stopping iPod reading…' if context.tag in ('scan', 'devices') else
                               'Stopping preparation checks…' if context.tag == 'plan' else
                               'Finishing the current step before stopping…')

    def closeEvent(self, event):
        if self.busy:
            self.show_progress('Wait for the current operation to finish before closing.'); event.ignore(); return
        self.save_draft()
        self.save_choices_draft()
        self.closed = True
        self.covers.closed = True
        self.covers.pool.shutdown(wait=False, cancel_futures=True)
        self.pool.shutdown(wait=False, cancel_futures=True)
        self.tool_pool.shutdown(wait=False, cancel_futures=True)
        event.accept()


def launch():
    configure_taskbar()
    # Qt still decodes photos with malformed ICC metadata; avoid repeating
    # profile diagnostics as their previews enter and leave the artwork cache.
    QLoggingCategory.setFilterRules('qt.gui.icc.warning=false')
    app = QApplication.instance() or QApplication(sys.argv[:1])
    app.setApplicationVersion(linnet_version())
    app.setWindowIcon(QIcon(str(ICON_PATH)))
    app.setStyle('Fusion'); app.setFont(QFont(UI_FONT, 10))
    window = MediaWindow(); window.show()
    activate_window(window)
    app.exec()

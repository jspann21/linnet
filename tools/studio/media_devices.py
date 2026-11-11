"""Device discovery, selection, scan results, and connection state."""
import shutil
from pathlib import Path

from PySide6.QtCore import QSignalBlocker, Qt
from PySide6.QtWidgets import QDialog, QFrame, QGridLayout, QProgressBar, QScrollArea, QWidget

from tools.content import apple_font, games, media_manager as media, media_trash, publish
from tools.content.media_manager import format_size as size
from tools.studio import media_reader, screen_settings

from .media_dialogs import ScreenshotDialog
from .media_models import library_search_text, music_group_key
from .media_widgets import ComboBox, box, button, label


class DeviceWorkspace:
    """Device discovery, selection, scan results, and connection state."""

    def _complete_devices(self, result):
        devices, errors = sorted(result['devices'], key=lambda item: item['drive']), result['errors']
        self.discovery_errors = errors
        self.hardware_report = result.get('hardware_report')
        self.link_connection = None
        if devices and result.get('links'):
            dialog = QDialog(self); dialog.setWindowTitle('Choose an iPod')
            layout = box(dialog, margins=(24,24,24,24)); picker = ComboBox()
            layout.addWidget(label('Choose an iPod', 'section'))
            for item in devices:picker.addItem(self.device_title(item),('disk',item))
            for item in result['links']:
                picker.addItem(item['target_name']+' · Linnet Link · '+item['serial'][-6:],('link',item))
            layout.addWidget(picker); layout.addWidget(button('Use this iPod',dialog.accept,'primary'))
            if dialog.exec() != QDialog.Accepted:return
            kind, selected = picker.currentData()
            devices = [selected] if kind=='disk' else []
            result = dict(result,links=[selected] if kind=='link' else [])
        if not devices and result.get('links'):
            links = result['links']
            selected = links[0]
            if len(links) > 1:
                dialog = QDialog(self); dialog.setWindowTitle('Choose an iPod')
                layout = box(dialog, margins=(24,24,24,24)); picker = ComboBox()
                for item in links:
                    picker.addItem(item['target_name'] + ' · ' + item['serial'][-6:], item)
                layout.addWidget(picker); layout.addWidget(button('Use this iPod',dialog.accept,'primary'))
                if dialog.exec() != QDialog.Accepted:return
                selected = picker.currentData()
            self.transition_device(None)
            self.link_connection = selected
            self.device_name.setText(selected['target_name'].replace('Linnet for ',''))
            self.device_info.setText('Connected through Linnet Link')
            self.update_preparation()
            self.show_progress('Your iPod is connected. Manage startup settings and Linnet builds in iPod setup.')
            self.navigate('iPod setup'); self.update_controls()
            if errors:
                self.record_diagnostic('devices', 'DiscoveryError', '; '.join(errors), 'partial')
                self.show_progress('A connected iPod needs attention. See the message in iPod setup.')
            return
        if not devices:
            self.transition_device(None)
            self.device_name.setText('No iPod connected'); self.device_info.setText('Connect in Disk Mode')
            if self.hardware_report:
                self.device_name.setText('Compatibility needs checking' if self.hardware_report['usb'] else 'No iPod connection verified')
                self.device_info.setText('See iPod setup for details')
                self.show_progress(self.hardware_report['summary'])
                if errors:self.record_diagnostic('devices', 'DiscoveryError', '; '.join(errors), 'failed')
            elif errors:
                self.record_diagnostic('devices', 'DiscoveryError', '; '.join(errors), 'failed')
                self.show_progress('The connected iPod’s storage could not be read. Follow the Disk Mode steps in iPod setup, then choose Refresh.')
            else:
                self.show_progress('Connect your iPod, then choose Refresh.')
            self.navigate('iPod setup')
            return
        if len(devices) > 1:
            dialog = QDialog(self); dialog.setWindowTitle('Choose an iPod'); layout = box(dialog, margins=(24, 24, 24, 24))
            layout.addWidget(label('Choose an iPod', 'section'))
            layout.addWidget(label('Select by name, drive, model, and serial suffix.', 'muted'))
            picker = ComboBox()
            for device in devices:
                picker.addItem(self.device_title(device), device)
                if self.same_device(self.identity, device):
                    picker.setCurrentIndex(picker.count() - 1)
            layout.addWidget(picker); layout.addWidget(button('Use this iPod', dialog.accept, 'primary'))
            if dialog.exec() != QDialog.Accepted:
                return
            identity = picker.currentData()
        else:
            identity = devices[0]
        if errors:
            self.record_diagnostic('devices', 'DiscoveryError', '; '.join(errors), 'partial')
            self.notice.setText('Other connected iPods could not be read: ' + '; '.join(errors))
            self.notice.show()
        self.transition_device(identity)
        self.device_connected = True
        self.refresh()

    @staticmethod
    def device_title(identity):
        name = (identity.get('volume_label') or 'Unnamed iPod').strip()
        model = identity['target_name'].replace('Linnet for ', '')
        serial = identity['disk_serial'][-6:]
        return f"{name} · {identity['drive']} · {model} · {serial}"

    def _complete_scan(self, result):
        if isinstance(result, dict) and result.get('publication_recovery_pending'):
            self.identity = result['identity']
            self.device_connected = False
            self.publication_recovery_pending = True
            self.publication_recovery_groups = result['groups']
            self.inventory = self.review = self.apple_info = None
            self.rows = []; self.readiness = {}
            self.trash_rows = []
            self.update_trash_badge(None)
            self.group = None
            self._selection_context = None
            self.clear_selection()
            self._library_source = None
            self.device_name.setText('Media index recovery needed')
            self.device_info.setText('Review recovery before reading or ejecting this iPod')
            self.notice.setText('An interrupted media index update was found. Choose Recover media indexes to review recovery.')
            self.notice.show()
            self.update_preparation(); self.render_library(); self.update_controls()
            return
        self.publication_recovery_pending = False
        self.publication_recovery_groups = []
        with self.covers.sidecar_lock:
            missing_folders = {folder for folder, found in self.covers.sidecars.items() if found is None}
        cached_photos, missing_covers = {}, set()
        for row in self.rows:
            artwork = row.get('artwork')
            if row['kind'] not in ('Photos', 'Music') or (artwork not in self.covers.cache and
                                                       artwork not in self.covers.pending):
                continue
            cached_photos[row['path']] = (row['bytes'], row['mtime_ns'], artwork)
            if row['kind'] == 'Music' and Path(row.get('cover_source') or row['path']).parent in missing_folders:
                missing_covers.add(row['path'])
        self.identity, self.inventory, self.rows, self.readiness, self.apple_info, self.review = result
        self.device_connected = True
        self.update_trash_badge(self.review.get('trash'))
        changed = []
        if cached_photos:
            current_photos = {row['path']: (row['bytes'], row['mtime_ns'], row.get('artwork')) for row in self.rows
                              if row['path'] in cached_photos}
            changed = [stamp[2] for path, stamp in cached_photos.items()
                       if current_photos.get(path) != stamp or path in missing_covers or
                       (stamp[2] in self.covers.cache and self.covers.cache[stamp[2]] is None)]
        # A refresh may discover a new sidecar even when track metadata and
        # previously loaded pixmaps did not change.
        self.covers.invalidate(changed)
        if self.group:
            field = {'Albums': 'album', 'Artists': 'artist', 'Genres': 'genre'}[self.group['group_kind']]
            members = [row for row in self.library_rows()
                       if music_group_key(row, field) == self.group['group_key']]
            if members:
                self.group['members'] = members
            else:
                self.group = None
        previous_videos = len(self.video_selection)
        self.video_selection.intersection_update(self.inventory['Videos']['files'])
        self.stale_videos = previous_videos - len(self.video_selection)
        self.validate_draft()
        self.save_draft()
        category_errors = [(kind, info.get('errors')) for kind, info in self.inventory.items()
                           if isinstance(info, dict) and info.get('errors')]
        if category_errors:
            names = ', '.join(kind for kind, _ in category_errors)
            self.notice.setText(f'{names} catalog needs repair. Open Preparation and choose Review catalog backups; healthy libraries remain available.')
            self.notice.show()
            for kind, errors in category_errors:
                self.record_diagnostic('scan', 'CatalogError', f'{kind}: {errors}', 'partial')
        elif self.discovery_errors:
            self.notice.setText('Other connected iPods could not be read: ' + '; '.join(self.discovery_errors))
            self.notice.show()
        else:
            self.notice.hide()
        self.update_device(); self.update_filters(); self.update_preparation(); self.render_library()
        self.show_progress('Ready. Eject safely when you’re finished.')
        if self.page == 'Screens':
            self.screens_controller.load_screens()
        if self.page == 'Games':
            self.games_controller.load_games()
        if self.page == 'iPod setup':
            self.render_setup()
        elif self.review['linnet']['state']=='absent' and not self.settings.value('setup_seen/'+self.identity['linnet_usb_serial'],False,type=bool):
            self.settings.setValue('setup_seen/'+self.identity['linnet_usb_serial'],True)
            self.navigate('iPod setup')
        elif self.page == 'Trash':
            identity = self.identity
            self.work('trash', lambda: media_trash.inventory(identity), 'Reading Trash…')

    def _complete_change(self, result):
        if result.get('reconnected_identity'):
            self.transition_device(result['reconnected_identity'])
        self.show_progress(result.get('summary', 'Changes saved.'))
        if result.get('needs_preparation'):
            self.prep_results.setPlainText(result['summary']); self.prep_results.show(); self.navigate('Preparation')
        self.refresh()

    def _complete_recover_publication(self, result):
        self.publication_recovery_pending = False
        self.publication_recovery_groups = []
        self.device_connected = True
        self.show_progress('Media indexes recovered. Checking the collection again…')
        self.refresh()

    def _complete_eject(self, result):
        self.transition_device(None)
        if result.get('disconnected'):
            self.device_name.setText('iPod disconnected')
            self.device_info.setText('Choose Find iPod after reconnecting.')
            self.capacity.setValue(0)
            self.show_progress('The previous iPod connection has closed. Choose Find iPod after reconnecting.')
            return
        self.device_name.setText('Safely ejected'); self.device_info.setText('You can disconnect your iPod.'); self.capacity.setValue(0)
        if result.get('already_ejected'):
            self.show_progress('Your iPod was already ejected in Windows. It is safe to disconnect, even if its screen still shows Disk Mode.')
        elif result.get('about_warning'):
            self.show_progress('Your iPod is safe to disconnect. Device details could not be refreshed: '
                               + result['about_warning'])
        else:
            self.show_progress('Your iPod is safe to disconnect.')

    def _complete_device_details(self, result):
        if result.get('link_connection') is not None and result['link_connection'] != self.link_connection:
            self.show_error('The selected iPod changed. Read device details again.')
            return
        fields = dict(row.split('  ', 1) for row in result['rows'] if '  ' in row)
        linked = result.get('link_connection') is not None
        dialog = QDialog(self); dialog.setWindowTitle('iPod details'); dialog.resize(580, 760)
        layout = box(dialog, margins=(26, 24, 26, 22), spacing=16)

        def value(text, style=None):
            shown = '\n'.join(text[at:at + 32] for at in range(0, len(text), 32)) if len(text) > 40 and not any(c.isspace() for c in text) else text
            item = label(shown, style, True)
            item.setTextFormat(Qt.PlainText)
            item.setTextInteractionFlags(Qt.TextSelectableByMouse | Qt.TextSelectableByKeyboard)
            return item

        heading = value(result['name'], 'section')
        if result.get('name_saved'):
            heading.setToolTip('Last saved iPod name')
        layout.addWidget(heading)
        model = fields.get('Model') or fields.get('Model no.') or fields.get('Device', '')
        if fields.get('Device') and fields['Device'] not in (result['name'], model):
            model = fields['Device'] + ' · ' + model
        layout.addWidget(value(model, 'muted'))
        scroll = QScrollArea(); scroll.setWidgetResizable(True); scroll.setFrameShape(QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        body = QWidget(); content = box(body, margins=(0, 0, 8, 0), spacing=18)
        scroll.setWidget(body); body.setAutoFillBackground(False)
        scroll.viewport().setAutoFillBackground(False); layout.addWidget(scroll, 1)

        captions = []

        def section(title, entries):
            if not entries:
                return
            content.addWidget(label(title, 'cardTitle'))
            grid = QGridLayout(); grid.setHorizontalSpacing(22); grid.setVerticalSpacing(10)
            grid.setColumnStretch(1, 1)
            for row, (caption, text) in enumerate(entries):
                caption_label = label(caption, 'muted'); captions.append(caption_label)
                grid.addWidget(caption_label, row, 0, Qt.AlignTop)
                grid.addWidget(value(text), row, 1)
            content.addLayout(grid)

        def entries(pairs):
            return [(caption, fields[key]) for caption, key in pairs if fields.get(key)]

        section('Device', [(caption, text) for caption, text in (
            ('Serial number', result.get('device_serial')),
            ('Connection', 'Linnet Link' if linked else 'Disk Mode')) if text] + entries((
                ('USB serial', 'USB serial'), ('Disk ID', 'Disk ID'), ('USB model', 'USB model'),
                ('USB', 'USB'), ('Volume ID', 'Volume ID'), ('Regulatory model', 'Regulatory'),
                ('Board', 'Board'), ('Family', 'Family'), ('Region', 'Region'))))
        if result['used_percent'] is not None:
            content.addWidget(label('Storage', 'cardTitle'))
            usage = QProgressBar(); usage.setRange(0, 100); usage.setValue(result['used_percent'])
            usage.setFormat(f"{result['used_percent']}% used"); content.addWidget(usage)
            stats = box(horizontal=True, spacing=20)
            for caption, key in (('Used', 'Used'), ('Available', 'Free'), ('Total', 'Capacity')):
                column = box(spacing=4); column.addWidget(label(caption, 'muted'))
                column.addWidget(value(fields.get(key))); stats.addLayout(column, 1)
            content.addLayout(stats)
        section('Storage details', entries((('Drive', 'Drive'), ('Capacity', 'Disk size'),
            ('Drive firmware', 'Drive firmware'), ('Drive serial', 'Drive serial'),
            ('Partition', 'Partition'), ('Sector size', 'Sector'), ('Logical sector', 'Logical sector'),
            ('Physical sector', 'Physical sector'), ('Power state', 'Storage power'),
            ('Audio', 'Audio'), ('Video', 'Video'), ('Photos', 'Photos'), ('Data', 'Data'),
            ('Trash', 'Trash'), ('Other', 'Other'))))
        section('Software and power', entries((('Software', 'Software'), ('Apple software', 'Apple OS'),
            ('Build', 'Build'), ('Health', 'Health'), ('Build ID', 'Build ID'),
            ('Battery', 'Battery'), ('USB charge limit', 'USB charge limit'), ('Read', 'Read'))))
        caption_width = max((item.sizeHint().width() for item in captions), default=0)
        for item in captions:
            item.setFixedWidth(caption_width)
        content.addStretch()
        layout.addWidget(button('Close', dialog.accept, 'primary'), 0, Qt.AlignRight)
        dialog.exec()

    def discover(self):
        self.work('devices', lambda: media_reader.read('devices', cancelled=self.stop_event.is_set),
                  'Looking for your iPod…')

    def recover_publication(self):
        if self.busy or not self.identity or not self.publication_recovery_pending:
            return
        if not self.dialog('Recover interrupted media update',
            'Interrupted index groups: ' + ', '.join(self.publication_recovery_groups) + '\n\n'
            'Recovery checks the saved device record and restores a verified set before the library is read. Keep the iPod connected until it finishes.',
            'Recover media indexes'):
            return
        identity = self.identity
        self.work('recover_publication', lambda: publish.recover_pending_publications(
            **media.target_args(identity)), 'Recovering media indexes…')

    def refresh(self):
        if self.busy:
            return
        if not self.identity or not self.device_connected:
            self.discover(); return
        identity = self.identity
        dimension = 2048 if self.photo_detail.currentIndex() else 640
        verification = 'full' if self.verify_audio.isChecked() else 'metadata'
        def inspect():
            result = media_reader.read('scan', identity=identity, photo_dimension=dimension,
                                       verification=verification, cancelled=self.stop_event.is_set,
                                       progress=self.events.progress.emit)
            if not isinstance(result, dict):
                for index, row in enumerate(result[2]):
                    if self.stop_event.is_set():
                        raise media.Cancelled()
                    if index % 256 == 0:
                        self.events.progress.emit(f'Preparing collection search · {index + 1:,} of {len(result[2]):,} items')
                    if row['category'] in ('original', 'published'):
                        row['_linnet_search_text'] = library_search_text(row)
            return result
        self.work('scan', inspect,
                  'Starting collection scan…')

    @staticmethod
    def same_device(first, second):
        return bool(first and second) and all(first.get(key) == second.get(key)
            for key in ('root', 'disk_serial', 'volume_serial', 'target_id'))

    def transition_device(self, identity, *, reset=False):
        """Clear device-derived state while retaining a draft for its own iPod."""
        self.link_connection = None
        self.apple_font_result = None
        if self.same_device(self.identity, identity) and not reset:
            self.identity = identity
            return
        self.build_editor_open = False
        self.save_draft()
        self.save_choices_draft()
        if self.identity:
            self._recent_private_identifiers.update(str(self.identity.get(field)) for field in
                ('root', 'drive', 'disk_serial', 'volume_serial', 'volume_unique_id') if self.identity.get(field))
        if identity and self.last_preparation and not self.same_device(self.last_preparation[0], identity):
            self.last_preparation = None
            self.prep_results.clear()
            self.prep_results.hide()
        self.covers.invalidate()
        self.identity = identity
        self.device_connected = bool(identity)
        self.publication_recovery_pending = False
        self.publication_recovery_groups = []
        self._selection_context = None
        self.inventory = self.review = self.apple_info = None
        with QSignalBlocker(self.boot_choice), QSignalBlocker(self.method_choice), QSignalBlocker(self.default_choice):
            self.boot_choice.clear()
            self.method_choice.clear()
            self.default_choice.clear()
        self.uninstall_detail.setText('Checking removal options…' if identity else 'Connect an iPod to check removal options.')
        self.rows = []
        self.trash_rows = []
        self.update_trash_badge(None)
        self.readiness = {}
        self.group = None
        self.video_selection.clear()
        self.video_mode = 'all'
        if identity:
            self.load_draft(identity)
        else:
            self.pending, self.pending_keys = [], set()
            self.draft_issues, self.stale_videos, self.queue_outcomes = {}, 0, {}
        self.screen_loaded_for = self.games_loaded_for = None
        self.screen_saved = self.game_saved = None
        self.screen_base = None
        self.screen_dirty = self.game_dirty = self.game_read_failed = False
        self.game_recover_requested=False
        self.custom_games.clear()
        self.screen_state = screen_settings.defaults(identity)
        self.screens_controller.render_screen_items()
        self.screen_status.setText('Loading screen settings…' if identity else
                                   'Connect an iPod in Disk Mode to save screen settings.')
        for check in self.game_checks:
            with QSignalBlocker(check):
                check.setChecked(False)
        self.games_status.setText('Loading games…' if identity else 'Connect an iPod in Disk Mode.')
        self.playlist_draft = None
        self._library_source, self._library_cache, self._library_stats = None, {}, {}
        self._group_cards_cache = {}
        self._library_search = None
        self.capacity.setValue(0)
        self.device_name.setText((identity.get('volume_label') or 'Unnamed iPod').strip() if identity else 'Connect your iPod')
        self.device_info.setText(f"{identity['drive']} · Loading this iPod…" if identity else 'USB · Disk Mode')
        self.update_filters()
        self.update_preparation()
        self.render_library()
        self.update_controls()

    def update_device(self):
        identity = self.identity
        self.device_name.setText((identity.get('volume_label') or 'Unnamed iPod').strip())
        self.device_info.setText(f"{identity['drive']} · {identity['target_name'].replace('Linnet for ', '')} · {identity['disk_serial'][-6:]}\n{size(identity['free_bytes'])} available")
        total = identity.get('total_bytes') or shutil.disk_usage(identity['root']).total
        self.capacity.setValue(int(100 * (1 - identity['free_bytes'] / total)))

    def update_controls(self):
        ready = self.identity is not None and self.device_connected and not self.busy
        from tools.studio.media_capture import available
        self.screenshot_button.setEnabled(not self.busy and available(self.link_connection))
        self.screenshot_button.setToolTip('Capture the screen currently shown on your iPod.' if available(self.link_connection) else
            'Requires Linnet running with a Developer or Trace build over Linnet Link. Unavailable in Disk Mode and recovery.')
        self.update_trash_badge(self.trash_summary)
        self.update_preparation_connection()
        font_ready = ready and apple_font.supported(self.identity) and not self.publication_recovery_pending
        self.apple_font_install.setEnabled(font_ready)
        self.apple_font_remove.setEnabled(font_ready)
        if not apple_font.supported(self.identity):
            self.apple_font_status.setText('Connect a color iPod in Disk Mode to choose its font.')
        elif not self.busy:
            self.apple_font_status.setText(self.apple_font_result or 'Safely eject and restart after changing the font.')
        for item in (*self.cache_buttons, self.backups_open, self.recovery_open):
            item.setEnabled(not self.busy)
        self.cache_clear.setEnabled(not self.busy and bool(self.cache_info and self.cache_info['files']))
        self.choose_device_button.setEnabled(not self.busy)
        self.retry_button.setEnabled(ready)
        self.prep_details.setVisible(self.last_preparation is not None)
        self.prep_details.setEnabled(ready and self.last_preparation is not None and
                                    self.same_device(self.identity, self.last_preparation[0]))
        games_available = bool(self.identity and games.available(self.identity))
        games_unavailable = bool(self.identity and not games_available)
        self.games_unavailable.setVisible(games_unavailable)
        self.games_status.setVisible(not games_unavailable)
        self.games_save.setVisible(games_available)
        for card in self.game_cards:
            card.setVisible(games_available)
        games_ready = ready and games.available(self.identity)
        self.games_save.setEnabled(games_ready and self.games_loaded_for == self.identity['root'])
        packages_ready=games_ready and self.games_loaded_for==self.identity['root']
        self.game_import.setEnabled(packages_ready)
        self.game_remove.setEnabled(packages_ready and self.custom_games.currentItem() is not None)
        self.games_recover.setVisible(bool(self.identity and self.game_read_failed and games.available(self.identity)))
        self.games_recover.setEnabled(ready and self.game_read_failed)
        self.publication_recovery_button.setVisible(self.publication_recovery_pending)
        self.publication_recovery_button.setEnabled(self.publication_recovery_pending and not self.busy)
        self.video_repair_button.setVisible(bool((self.inventory or {}).get('Videos', {}).get('errors')))
        self.video_repair_button.setEnabled(ready and bool((self.inventory or {}).get('Videos', {}).get('errors')))
        supported_games = (games.supported(self.identity)
                           if self.identity and games.available(self.identity) else set())
        for index, check in enumerate(self.game_checks):
            if index not in supported_games and check.isChecked():
                with QSignalBlocker(check):
                    check.setChecked(False)
            check.setEnabled(games_ready and self.games_loaded_for == self.identity['root']
                             and index in supported_games)
        if ready and not games_ready:
            self.games_status.setText('Games are unavailable on this iPod.')
        supported = media.supported_kinds(self.identity) if ready else ()
        self.screens.setEnabled(not self.busy)
        self.screen_save.setEnabled(ready and all(self.screen_state.values()))
        self.screen_reload.setEnabled(ready)
        self.screen_reset.setEnabled(not self.busy)
        self.screen_items.setEnabled(not self.busy)
        self.screen_picker.setEnabled(not self.busy)
        for item in (self.eject_button, self.add_button, self.playlists_button, self.sync_button, self.choose_videos_button, self.queue_add, self.folder_button):
            item.setEnabled(ready)
        self.device_details_button.setEnabled(ready or bool(self.link_connection and not self.busy))
        music_root = Path(self.identity['root']) / 'Music' if ready else None
        convertible = ready and any(row['kind'] == 'Music' and row['category'] == 'original' and
                                    str(row['status']).startswith('Needs conversion:') and
                                    Path(row['path']).is_relative_to(music_root) for row in self.rows)
        self.queue_incompatible.setVisible(convertible)
        self.queue_incompatible.setEnabled(convertible)
        self.update_queue_controls()
        self.prepare_button.setEnabled(ready and self.inventory is not None)
        apple_ready = ready and self.apple_info is not None and self.apple_info['supported']
        self.apple_choose.setEnabled(apple_ready)
        self.apple_update.setText('Retry update' if apple_ready and
                                  self.apple_info.get('can_update') and
                                  self.apple_info['status'] == 'Needs attention' else 'Update library')
        self.apple_update.setEnabled(apple_ready and self.apple_info.get('can_update', False))
        self.choose_videos_button.setEnabled(ready and self.inventory is not None and self.identity.get('media_profile', {}).get('video_enabled', True))
        self.refresh_button.setEnabled(not self.busy)
        self.empty_action.setEnabled(not self.busy)
        self.refresh_button.setVisible(not self.busy)
        for kind, check in self.prep_checks.items():
            check.setEnabled(kind in supported)
        self.update_setup_status(); self.update_install_action(); self.selection_changed()

    def eject(self):
        if not self.identity or not self.device_connected or self.busy:
            return
        self.save_choices_draft()
        identity = self.identity
        def run():
            return media.eject_device(identity)
        self.work('eject', run, 'Safely ejecting your iPod…')

    def capture_screenshot(self):
        from tools.studio.media_capture import available, capture
        if self.busy or not available(self.link_connection):return
        connection = dict(self.link_connection, identity=dict(self.link_connection['identity']))
        self.work('screenshot', lambda: capture(connection), 'Taking an iPod screenshot…')

    def _complete_screenshot(self, result):
        if result['connection'] != self.link_connection:
            self.show_error('The selected iPod changed. Refresh before taking another screenshot.')
            return
        try:
            dialog = ScreenshotDialog(result, self)
        except ValueError as error:
            self.show_error(str(error)); return
        self.show_progress('Screenshot ready. Copy it or save a PNG.')
        try:
            dialog.exec()
        finally:
            dialog.deleteLater()

    def device_details(self):
        if self.busy:
            return
        if self.link_connection:
            from tools.studio.media_link import read_details
            connection = dict(self.link_connection, identity=dict(self.link_connection['identity']))
            self.work('device_details', lambda: read_details(connection), 'Reading iPod details…')
            return
        if not self.identity or not self.device_connected:
            return
        from tools.content import about
        identity = self.identity
        self.work('device_details', lambda: about.read_details(identity, media.refresh),
                  'Reading iPod details…')

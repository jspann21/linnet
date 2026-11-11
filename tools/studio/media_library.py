"""Library browsing, filtering, selection, and Trash actions."""
import os
import subprocess
from datetime import datetime
from pathlib import Path

from PySide6.QtCore import QItemSelection, QItemSelectionModel, QSignalBlocker, Qt
from PySide6.QtWidgets import QDialog, QHeaderView

from tools.content import media_library, media_manager as media, media_trash
from tools.content.media_manager import format_size as size

from .media_models import Artwork, library_search_text, music_group_key
from .media_widgets import box, button, label


class LibraryWorkspace:
    """Library browsing, filtering, selection, and Trash actions."""

    def change_view(self, view):
        self.view, self.group = view, None
        self.settings.setValue('music_view', view)
        self.clear_selection(); self.render_library()

    def back_to_groups(self):
        self.group = None; self.render_library()

    def focus_search(self):
        if self.stack.currentWidget() == self.library:
            self.search.setFocus(); self.search.selectAll()

    def activate_empty(self):
        if self.busy:
            return
        if not self.identity:
            self.refresh()
        elif self.library_rows():
            self.clear_filters()
        elif self.page == 'Trash' and self.trash_summary and self.trash_summary.get('unreadable'):
            self.open_folder(Path(self.identity['root']) / '.LINNET' / 'TRASH')
        elif self.page != 'Trash':
            self.add_media()

    def clear_filters(self, *, render=True):
        self.search_timer.stop()
        with QSignalBlocker(self.search), QSignalBlocker(self.genre_filter), QSignalBlocker(self.artist_filter):
            self.search.clear(); self.genre_filter.setCurrentIndex(0); self.artist_filter.setCurrentIndex(0)
        if render:
            self.render_library()

    def library_rows(self):
        if self.page == 'Trash':
            return [dict(r, kind='Trash', title=r['title'] or 'Removed files', status=datetime.fromtimestamp(r['started']).strftime('%b %d, %Y · %H:%M')) for r in self.trash_rows]
        # Completed scans replace self.rows. Reuse page rows between searches
        # and view changes, returning a new list so model sorting stays local.
        if self._library_source is not self.rows:
            self._library_source, self._library_cache, self._library_stats = self.rows, {}, {}
            self._library_title_cache = {}
            self._library_unique_ids = {}
            self._group_cards_cache = {}
            self._library_search = None
        if self.page in self._library_cache:
            return list(self._library_cache[self.page])
        unique = {}
        for row in self.rows:
            if row['kind'] == self.page and row['category'] in ('original', 'published', 'error'):
                copy = dict(row)
                if row['kind'] == 'Photos':
                    copy['artwork'] = row.get('artwork') or row['path']
                unique[str(Path(row['path'])).casefold()] = copy
        page_rows = list(unique.values())
        albums = len({(r.get('album'), r.get('album_artist') or r.get('artist')) for r in page_rows}) if self.page == 'Music' else 0
        self._library_stats[self.page] = (len(page_rows), sum(r['bytes'] for r in page_rows), albums)
        self._library_cache[self.page] = page_rows
        self._library_unique_ids[self.page] = len({row['id'] for row in page_rows}) == len(page_rows)
        return list(self._library_cache[self.page])

    def render_library(self):
        if self.page not in (*media.KINDS, 'Trash'):
            return
        trash_attention = self.page == 'Trash' and bool(self.trash_summary and self.trash_summary.get('unreadable'))
        context = (self.page, self.view, (self.group['group_kind'], self.group['group_key']) if self.group else None)
        active = self.views.currentWidget()
        previous_rows, previous_ranges = None, []
        if getattr(self, '_selection_context', None) == context and active in (self.table, self.grid):
            previous_rows, previous_ranges = self.model.rows, self.selected_row_ranges()
        self._selection_context = context
        rows = self.library_rows()
        grouped = self.page == 'Music' and self.view != 'Songs' and self.group is None
        title_order = self.page != 'Trash' and not grouped and self.group is None
        if title_order:
            # Filtering preserves this order. Keep the original page order for
            # grouped views, whose representative artwork comes from members.
            if self.page not in self._library_title_cache:
                self._library_title_cache[self.page] = sorted(rows, key=lambda row: row['title'].casefold())
            rows = list(self._library_title_cache[self.page])
        all_rows = rows
        music = self.page == 'Music'
        self.heading_label.set_message(self.group['title'] if self.group else self.page)
        self.segments.setVisible(music and self.group is None)
        for name, item in self.view_buttons.items():
            item.setChecked(name == self.view)
        self.back_button.setVisible(self.group is not None)
        self.genre_filter.setVisible(music); self.artist_filter.setVisible(music)
        self.filters_button.setVisible(music)
        active_filters = sum(bool(combo.currentData()) for combo in (self.genre_filter, self.artist_filter))
        self.filters_button.setText('Hide filters' if self.filters_button.isChecked() else
                                    f'Filters ({active_filters})' if active_filters else 'Filters')
        self.filters_panel.setVisible(music and self.filters_button.isChecked())
        self.sync_button.setVisible(self.page == 'Videos')
        self.playlists_button.setVisible(music)
        self.folder_button.setVisible(self.page != 'Trash')
        self.add_button.setVisible(self.page != 'Trash')
        self.search.setPlaceholderText('Search songs, artists, albums…' if music else f'Search {self.page.lower()}…')
        words = tuple(self.search.text().casefold().split())
        artist, genre = self.artist_filter.currentData(), self.genre_filter.currentData()
        if self.group:
            rows = list(self.group['members'])
        scope_count = len(rows)
        source = (self._library_title_cache[self.page] if title_order else
                  self.group['members'] if self.group else self._library_cache.get(self.page))
        filters = (artist, genre) if music else (None, None)
        previous = self._library_search
        narrower = bool(words and source is not None and previous is not None
                        and previous[0] is source and previous[1] == filters
                        and len(words) >= len(previous[2])
                        and all(word.startswith(old) for word, old in zip(words, previous[2])))
        if narrower:
            rows = previous[3]
        elif music and (artist or genre):
            rows = [r for r in rows if (not artist or (r.get('artist') or 'Unknown artist') == artist)
                    and (not genre or (r.get('genre') or 'Unknown genre') == genre)]
        if words:
            matches = []
            first, *remaining = words
            for row in rows:
                text = row.get('_linnet_search_text')
                if text is None:
                    text = library_search_text(row)
                    row['_linnet_search_text'] = text
                if first in text and (not remaining or all(word in text for word in remaining)):
                    matches.append(row)
            rows = matches
            # Preserve source order separately from the model's in-place sort.
            self._library_search = (source, filters, words, tuple(matches)) if source is not None else None
        else:
            self._library_search = None
        matched_count = len(rows)
        filtered = bool(words or (music and (artist or genre)))
        if grouped:
            cards = self._group_cards_cache.get(self.view) if not filtered else None
            if cards is None:
                groups = {}
                field = {'Albums': 'album', 'Artists': 'artist', 'Genres': 'genre'}[self.view]
                for row in rows:
                    key = music_group_key(row, field)
                    groups.setdefault(key, []).append(row)
                cards = []
                for key, members in groups.items():
                    artists = {r.get('artist') for r in members if r.get('artist')}
                    subtitle = (members[0].get('album_artist') or (next(iter(artists)) if len(artists) == 1 else 'Various artists')) if self.view == 'Albums' else f'{len(members):,} songs'
                    art_row = next((r for r in members if r.get('artwork')), members[0])
                    cards.append(dict(title=members[0].get(field) or f'Unknown {field}', subtitle=subtitle,
                        kind='Music', group_kind=self.view, group_key=key, members=members, artwork=art_row.get('artwork', '') if self.view != 'Genres' else '', cover_source=art_row['path'], bytes=sum(r['bytes'] for r in members)))
                if not filtered:
                    self._group_cards_cache[self.view] = cards
            rows = list(cards)
        # Ordinary pages already have the title tie-break order. Groups keep
        # their own order; Trash rows are rebuilt with formatted timestamps.
        if grouped or self.group is not None or (self.page == 'Trash' and self.table.horizontalHeader().sortIndicatorSection() > 0):
            rows.sort(key=lambda r: (r.get('disc', 0), r.get('track', 0), r['title'].casefold()) if self.group and self.view == 'Albums' else (r['title'].casefold(),))
        columns = ([('title', 'TITLE'), ('album', 'ALBUM'), ('genre', 'GENRE'), ('duration', 'TIME'), ('bytes', 'SIZE')]
                   if music and not grouped else [('title', 'NAME'), ('status', 'STATUS'), ('bytes', 'SIZE')])
        self.model.reset(rows, columns)
        if not grouped and self.group is None:
            header = self.table.horizontalHeader()
            column = max(0, min(header.sortIndicatorSection(), self.model.columnCount() - 1))
            order = header.sortIndicatorOrder()
            if self.page == 'Trash' or column != 0 or order != Qt.AscendingOrder:
                self.model.sort(column, order)
        layout = tuple(columns)
        if getattr(self, '_table_layout', None) != layout:
            self._table_layout = layout
            self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
            for col in range(1, self.model.columnCount()):
                self.table.horizontalHeader().setSectionResizeMode(col, QHeaderView.Interactive)
                self.table.setColumnWidth(col, 220 if col == 1 and music else 130 if col < 3 else 80)
        self.table.fit_columns()
        grid = grouped or self.page in ('Photos', 'Videos')
        self.views.setCurrentWidget(self.grid if grid else self.table)
        if not rows:
            self.views.setCurrentWidget(self.empty)
            self.empty_title.setText('Reading your collection…' if self.busy else 'Connect your iPod' if not self.identity else 'No matches' if all_rows else 'Trash needs review' if trash_attention else 'Trash is empty' if self.page == 'Trash' else f'No {self.page.lower()} yet')
            self.empty_detail.setText('Your collection will appear here when reading finishes.' if self.busy else 'Connect an iPod in Disk Mode, then find it to browse and add media.' if not self.identity else 'Try another search or clear your filters.' if all_rows else 'Some Trash entries could not be read. Open the folder to review them.' if trash_attention else 'Removed files appear here until you restore or permanently delete them.' if self.page == 'Trash' else 'Add files from your computer to get started.')
        self.empty_action.setText('Find iPod' if not self.identity else 'Clear filters' if all_rows else 'Open Trash folder' if trash_attention else 'Add media')
        self.empty_action.setVisible(not self.busy and (not self.identity or bool(all_rows) or self.page != 'Trash' or trash_attention))
        count, total_bytes, albums = ((len(all_rows), sum(r['bytes'] for r in all_rows), 0)
                                      if self.page == 'Trash' else self._library_stats[self.page])
        self.subtitle.set_message(f'{count:,} songs  ·  {albums:,} albums  ·  {size(total_bytes)}' if music else
                              'Restore removed files or delete them permanently to free space.' if self.page == 'Trash' else
                              f'{count:,} files on your iPod  ·  {size(total_bytes)}')
        if self.group:
            members = self.group['members']
            self.subtitle.set_message((self.group['subtitle'] + '  ·  ' if self.view == 'Albums' else '') + f'{len(members):,} songs  ·  {size(sum(r["bytes"] for r in members))}')
        unit = 'songs' if music else 'items'
        if filtered:
            result_count = f'{matched_count:,} of {scope_count:,} {unit}' + (f' · {len(rows):,} {self.view.lower()}' if grouped else '')
            self.subtitle.set_message(result_count, tooltip=result_count + '\n' + self.subtitle.message)
        self.clear_button.setVisible(filtered)
        self.hint.setText('Select videos to move them to Trash. Sync video folder reconciles files changed in Explorer.' if self.page == 'Videos' else
                          'Double-click to explore. Select one or more groups to manage their songs.' if grouped else
                          'Select a removal to restore it, or permanently delete it to free storage.' if self.page == 'Trash' else
                          'Ctrl+click or Shift+click to select multiple files. Double-click for details.')
        self.selection_bar.hide()
        active = self.views.currentWidget()
        if previous_ranges and active in (self.table, self.grid):
            selection = QItemSelection()
            unique = grouped or (self.page != 'Trash' and self._library_unique_ids[self.page])
            if unique and previous_rows == self.model.rows:
                # Model reset invalidates Qt ranges. Rebuild their endpoints
                # directly when every row retains its position and identity.
                for first, last in previous_ranges:
                    selection.select(self.model.index(first, 0), self.model.index(last, 0))
            else:
                selected_keys = set()
                for first, last in previous_ranges:
                    for row in previous_rows[first:last + 1]:
                        selected_keys.add(('group', row['group_kind'], row['group_key']) if 'group_key' in row else ('row', row['id']))
                start = None
                for index in range(len(self.model.rows) + 1):
                    if index < len(self.model.rows):
                        row = self.model.rows[index]
                        key = ('group', row['group_kind'], row['group_key']) if 'group_key' in row else ('row', row['id'])
                        chosen = key in selected_keys
                    else:
                        chosen = False
                    if chosen and start is None:
                        start = index
                    elif not chosen and start is not None:
                        selection.select(self.model.index(start, 0), self.model.index(index - 1, 0))
                        start = None
            if not selection.isEmpty():
                flags = QItemSelectionModel.Select | (QItemSelectionModel.Rows if active is self.table else QItemSelectionModel.NoUpdate)
                active.selectionModel().select(selection, flags)

    def selected_row_ranges(self):
        view = self.views.currentWidget()
        if view not in (self.grid, self.table):
            return []
        selection = view.selectionModel().selection()
        # Row selections already contain compact intervals. Avoid expanding a
        # bulk selection into one Python Qt index per song just to read rows.
        if view is self.table and any(part.left() != 0 or part.right() != self.model.columnCount() - 1 for part in selection):
            spans = [(index.row(), index.row()) for index in view.selectionModel().selectedRows()]
        else:
            spans = [(part.top(), part.bottom()) for part in selection]
        return [(max(0, first), min(len(self.model.rows) - 1, last))
                for first, last in spans if last >= 0 and first < len(self.model.rows)]

    def selected(self):
        unique = {}
        for first, last in self.selected_row_ranges():
            for at in range(first, last + 1):
                row = self.model.rows[at]
                for member in row.get('members', (row,)):
                    unique[member['id']] = member
        return list(unique.values())

    def selection_changed(self, *_):
        rows = self.selected()
        self.hint.setVisible(bool(self.model.rows) and not rows)
        self.selection_bar.setVisible(bool(rows) and self.device_connected)
        self.selection_text.set_message(f'{len(rows):,} selected  ·  {size(sum(r["bytes"] for r in rows))}')
        self.restore_button.setVisible(self.page == 'Trash')
        self.restore_button.setEnabled(self.device_connected and not self.busy)
        retail_photos = bool(rows) and all(r.get('retail_photo') for r in rows)
        self.remove_button.setText('Exclude from preparation' if retail_photos else
                                   'Delete permanently' if self.page == 'Trash' else 'Move to Trash')
        self.remove_button.setEnabled(self.device_connected and not self.busy and
                                      (retail_photos or all(r.get('removable', True) for r in rows)))
        self.info_button.setEnabled(len(rows) == 1 and not self.busy)
        music_roots = (Path(self.identity['root']) / 'Music',
                       Path(self.identity['root']) / 'iPod_Control' / 'Music') if self.identity else ()
        self.prepare_pcm_button.setVisible(self.page == 'Music' and bool(rows))
        self.prepare_pcm_button.setEnabled(self.device_connected and not self.busy and bool(rows) and
            all(row['kind'] == 'Music' and row['category'] == 'original' and
                not row['path'].casefold().endswith('.linnet.wav') and
                any(Path(row['path']).is_relative_to(root) for root in music_roots) for row in rows))

    def clear_selection(self):
        self.table.clearSelection(); self.grid.clearSelection(); self.selection_bar.hide()

    def open_group(self, index):
        row = self.model.rows[index.row()]
        if 'members' in row:
            self.group = row; self.render_library()
        else:
            self.inspect_selection()

    def inspect_selection(self, *_):
        rows = self.selected()
        if len(rows) != 1:
            return
        row = rows[0]
        dialog = QDialog(self); dialog.setWindowTitle('Media details'); dialog.resize(550, 330)
        layout = box(dialog, margins=(28, 28, 28, 28), spacing=15)
        if row.get('artwork'):
            layout.addWidget(Artwork(row, self.covers, dialog), 0, Qt.AlignHCenter)
        layout.addWidget(label(row['title'], 'section', True))
        for field in ('artist', 'album', 'genre', 'status'):
            if row.get(field):
                layout.addWidget(label(f'{field.title()}   {row[field]}', 'muted', True))
        if row.get('category') == 'error' or row.get('apple_only'):
            layout.addWidget(label(row.get('description') or 'This catalog needs repair before preparation.', 'notice', True))
            if row.get('error'):
                layout.addWidget(label(row['error'], 'muted', True))
        layout.addWidget(label(size(row['bytes']), 'muted'))
        location = label(row['path'], 'muted', True); location.setTextInteractionFlags(Qt.TextSelectableByMouse); layout.addWidget(location)
        actions = box(horizontal=True); actions.addWidget(button('Show in folder', lambda: self.open_folder(row['path']), None, 'folder')); actions.addStretch(); actions.addWidget(button('Done', dialog.accept, 'primary')); layout.addLayout(actions)
        dialog.exec()

    def open_folder(self, path):
        try:
            source = Path(path).resolve(strict=True)
            if source.is_dir():
                os.startfile(str(source))
            else:
                subprocess.Popen(['explorer.exe', '/select,', str(source)])
        except OSError as error:
            self.dialog('Cannot open location', f'The file or folder may have moved or the device may be disconnected.\n\n{error}', 'OK', cancel=False)

    def open_media_folder(self):
        if self.busy or not self.identity or self.page not in media.KINDS:
            return
        root = Path(self.identity['root'])
        folder = root / self.page
        self.open_folder(folder if folder.is_dir() else root)

    def _complete_trash(self, result):
        self.trash_rows = result['rows']; self.update_trash_badge(result['summary'])
        self.render_library(); self.show_progress('Trash uses storage until files are permanently deleted.')

    def update_trash_badge(self, summary):
        self.trash_summary = summary
        count = summary.get('count', 0) if summary else 0
        unreadable = summary.get('unreadable', 0) if summary else 0
        self.trash_badge.setText(('99+' if count > 99 else str(count)) if count else '!')
        self.trash_badge.setVisible(bool(self.identity and self.device_connected and (count or unreadable)))
        noun = 'entry' if count == 1 else 'entries'
        detail = (f'{count:,} recoverable {noun} in Trash. Restore removed files or empty Trash to free space.' if count else
                  'Removed media can be restored from Trash until you empty it.')
        if unreadable:
            detail += ' Some Trash entries need review.'
        self.navigation['Trash'].setToolTip(detail)
        self.navigation['Trash'].setAccessibleName(
            f'Trash, {count:,} {noun}' + (', needs review' if unreadable else '') if summary else 'Trash')

    def update_filters(self):
        for picker, field, title in ((self.genre_filter, 'genre', 'All genres'), (self.artist_filter, 'artist', 'All artists')):
            old = picker.currentData()
            with QSignalBlocker(picker):
                picker.clear(); picker.addItem(title, '')
                for value in sorted({row.get(field) or f'Unknown {field}' for row in self.rows if row['kind'] == 'Music' and row['category'] == 'original'}, key=str.casefold):
                    picker.addItem(value, value)
                picker.setCurrentIndex(max(0, picker.findData(old)))

    def remove_selected(self):
        rows = self.selected()
        if self.busy or not self.identity or not self.device_connected or not rows:
            return
        if any(not row.get('removable', True) for row in rows):
            self.show_error('This item cannot be removed. Review its details for recovery guidance.')
            return
        identity = self.identity
        if all(row.get('retail_photo') for row in rows):
            def exclude_photos():
                media.skip_files(identity, [('Photos', row['path']) for row in rows])
                return dict(summary='Photos excluded. Run Prepare to update Linnet; Apple originals are retained.')
            self.work('skipped', exclude_photos, 'Excluding selected photos from preparation…')
            return
        if self.page == 'Trash':
            self.erase_trash(rows)
            return
        if not self.confirm_trash(rows):
            return
        dimension = 2048 if self.photo_detail.currentIndex() else 640
        ffmpeg, ffprobe = self.verified_ffmpeg, self.verified_ffprobe
        def remove():
            result = media_library.remove_items(identity, rows)
            needed = [kind for kind in result['requires_update'] if kind != 'Videos'
                      and kind in media.supported_kinds(identity) and kind not in result['empty_folders']]
            if needed:
                prepared = media.run(identity, needed, photo_dimension=dimension, ffmpeg=ffmpeg, ffprobe=ffprobe,
                    progress=self.events.progress.emit, cancelled=self.stop_event.is_set)
                result['complete'] = prepared['complete']
                result['stopped'] = prepared['stopped']
                result['needs_preparation'] = not prepared['complete']
                result['summary'] = 'Files moved to Trash. Playback libraries updated.' if prepared['complete'] else 'Files moved to Trash. Playback needs preparation: ' + '; '.join(e['message'] for e in prepared['errors'])
            return result
        self.work('remove', remove, 'Moving selected files to Trash…')

    def restore_selected(self):
        rows = self.selected()
        if self.busy or not self.device_connected or self.page != 'Trash' or not rows:
            return
        identity = self.identity
        dimension = 2048 if self.photo_detail.currentIndex() else 640
        ffmpeg, ffprobe = self.verified_ffmpeg, self.verified_ffprobe
        def restore():
            needed = set()
            for row in rows:
                result = media_trash.recover(identity, row['id'])
                needed.update(result.get('requires_update', []))
            needed.intersection_update(media.supported_kinds(identity))
            if needed:
                prepared = media.run(identity, needed, photo_dimension=dimension, ffmpeg=ffmpeg, ffprobe=ffprobe,
                    progress=self.events.progress.emit, cancelled=self.stop_event.is_set)
                if not prepared['complete']:
                    return dict(complete=False, stopped=prepared['stopped'], needs_preparation=True,
                        summary='Files restored. Playback needs preparation: ' + '; '.join(e['message'] for e in prepared['errors']))
            return dict(complete=True, summary='Files and playback libraries restored.')
        self.work('restore', restore, 'Restoring your files…')

    def erase_trash(self, rows):
        if self.busy or not self.identity or not self.device_connected or not rows:
            return
        identity = self.identity
        message = (f'Permanently delete the selected items from Trash and free {size(sum(r["bytes"] for r in rows))}? '
                   'These files cannot be restored afterward.')
        if not self.dialog('Delete permanently?', message, 'Delete permanently', danger=True):
            return
        def erase():
            for row in rows:
                media_trash.recover(identity, row['id'], erase=True)
            return dict(summary='Trash deleted. Storage is now available.')
        self.work('erase', erase, 'Emptying iPod Trash…')

from pathlib import Path

from PySide6.QtCore import (QItemSelection, QItemSelectionModel,
                           QStringListModel, Qt, QSize, QSignalBlocker)
from PySide6.QtGui import QColor, QFont, QGuiApplication
from PySide6.QtWidgets import (QAbstractItemView, QDialog, QHBoxLayout, QLabel,
    QLineEdit, QListView, QListWidget, QListWidgetItem, QPushButton, QVBoxLayout,
    QFrame, QStackedWidget, QStyledItemDelegate, QStyle, QFileDialog,
    QPlainTextEdit, QMessageBox)

from tools.content import playlists as playlist_content


class SongLibraryModel(QStringListModel):
    def __init__(self, tracks, parent=None):
        self.songs = sorted(((identity, title, title.casefold()) for identity, title in tracks.items()),
                            key=lambda song: song[2])
        super().__init__([song[1] for song in self.songs], parent)
        self.shown = [True] * len(self.songs)
        self.applied_shown = self.shown
        self.match_count = len(self.songs)
        self.query = ''

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid():
            return None
        if role == Qt.ToolTipRole:
            return self.songs[index.row()][1]
        return super().data(index, role)

    def identity(self, index):
        # Track IDs are unsigned 64-bit values; keep them out of QVariant's
        # signed integer conversion on Windows.
        return self.songs[index.row()][0]

    def selected_rows(self, selection):
        return (row for span in selection for row in range(span.top(), span.bottom() + 1)
                if self.shown[row])


class SongLibraryView(QListView):
    def selectAll(self):
        model = self.model()
        selection = QItemSelection()
        count = model.rowCount()
        if not model.query:
            if count:
                selection.select(model.index(0, 0), model.index(count - 1, 0))
            self.selectionModel().select(selection, QItemSelectionModel.ClearAndSelect)
            return
        start = None
        for row in range(count + 1):
            shown = row < count and model.shown[row]
            if shown and start is None:
                start = row
            elif not shown and start is not None:
                selection.select(model.index(start, 0), model.index(row - 1, 0))
                start = None
        self.selectionModel().select(selection, QItemSelectionModel.ClearAndSelect)

    def filter(self, text):
        model = self.model()
        query = text.casefold()
        if query != model.query:
            # Keep stable rows so Qt retains current, hidden selections and
            # the full Ctrl/Shift selection state while the query changes.
            shown = [query in song[2] for song in model.songs]
            model.match_count = sum(shown)
            # Empty results show the empty-state panel. Defer row layout until
            # a query has matches, comparing against the last applied mask.
            if model.match_count:
                changed = [row for row, (before, after) in enumerate(zip(model.applied_shown, shown)) if before != after]
                hide = self.setRowHidden
                for row in changed:
                    hide(row, not shown[row])
                model.applied_shown = shown
            model.shown = shown
            model.query = query
        return model.match_count


class TrackDelegate(QStyledItemDelegate):
    def __init__(self, parent, compact=False):
        super().__init__(parent)
        self.compact = compact

    def sizeHint(self, option, index):
        return QSize(160, 36 if self.compact else 32)

    def paint(self, painter, option, index):
        painter.save()
        rect = option.rect
        if option.state & QStyle.State_Selected:
            painter.fillRect(rect, QColor('#ddf4f8'))
        elif option.state & QStyle.State_MouseOver:
            painter.fillRect(rect, QColor('#f0f8fa'))
        title, _, detail = index.data().partition(' · ')
        font = QFont(option.font); font.setWeight(QFont.DemiBold)
        painter.setFont(font); painter.setPen(QColor('#202f35'))
        if self.compact:
            area = rect.adjusted(9, 0, -9, 0)
            painter.drawText(area, Qt.AlignVCenter, painter.fontMetrics().elidedText(index.data(), Qt.ElideRight, area.width()))
            if option.state & QStyle.State_HasFocus:
                painter.setPen(QColor('#16859c'))
                painter.drawRect(rect.adjusted(1, 1, -2, -2))
            painter.restore(); return
        area = rect.adjusted(10, 0, -12, 0)
        title_width = int(area.width() * .56) if detail else area.width()
        area.setWidth(title_width)
        painter.drawText(area, Qt.AlignVCenter, painter.fontMetrics().elidedText(title, Qt.ElideRight, area.width()))
        font.setWeight(QFont.Normal); font.setPixelSize(11)
        painter.setFont(font); painter.setPen(QColor('#617176'))
        area = rect.adjusted(20 + title_width, 0, -12, 0)
        painter.drawText(area, Qt.AlignVCenter, painter.fontMetrics().elidedText(detail, Qt.ElideRight, area.width()))
        painter.setPen(QColor('#edf1f2')); painter.drawLine(rect.bottomLeft(), rect.bottomRight())
        if option.state & QStyle.State_HasFocus:
            painter.setPen(QColor('#16859c'))
            painter.drawRect(rect.adjusted(1, 1, -2, -2))
        painter.restore()


class PlaylistEditor(QDialog):
    def __init__(self, parent, state):
        super().__init__(parent)
        self.state, self.slot = state, None
        self.setWindowTitle('Linnet · Playlists'); self.setObjectName('playlistEditor')
        self.setMinimumSize(640, 420)
        screen = parent.screen() if parent is not None else QGuiApplication.primaryScreen()
        area = screen.availableGeometry() if screen else None
        self.resize(max(640, min(1080, area.width() - 32)) if area else 1080,
                    max(420, min(680, area.height() - 32)) if area else 680)
        layout = QVBoxLayout(self); layout.setContentsMargins(12, 12, 12, 10); layout.setSpacing(8)
        header = QHBoxLayout(); layout.addLayout(header)
        heading = QVBoxLayout(); heading.setSpacing(5); header.addLayout(heading)
        self.label(heading, 'Playlists', 'dialogTitle')
        self.label(heading, 'Your iPod playlists, in your order.', 'muted')
        header.addStretch(); self.count = self.label(header, '', 'badge')
        columns = QHBoxLayout(); columns.setSpacing(8); layout.addLayout(columns, 1)
        left = self.panel(columns, 0, 165)
        self.label(left, 'ON YOUR IPOD', 'eyebrow')
        self.new_button = self.button(left, '+ New playlist', self.create)
        self.import_button = self.button(left, 'Import M3U…', self.import_m3u)
        self.playlists = QListWidget(); self.playlists.setObjectName('playlistNames')
        self.playlists.setItemDelegate(TrackDelegate(self.playlists, compact=True))
        self.playlists.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.playlist_stack, _ = self.list_area(left, self.playlists, 'No playlists yet\n\nCreate your first playlist above.')
        self.delete_button = self.button(left, 'Delete playlist', self.delete, 'danger')
        middle = self.panel(columns, 5)
        self.label(middle, 'PLAYLIST', 'eyebrow')
        self.name = QLineEdit(); self.name.setPlaceholderText('Playlist name'); middle.addWidget(self.name)
        self.name.setToolTip('Up to 31 UTF-8 bytes')
        self.track_count = self.label(middle, '', 'muted')
        self.members = self.track_list()
        self.member_stack, self.empty_members = self.list_area(middle, self.members, '')
        actions = QHBoxLayout(); middle.addLayout(actions)
        self.up_button = self.button(actions, '↑ Up', lambda: self.move(-1))
        self.down_button = self.button(actions, '↓ Down', lambda: self.move(1))
        actions.addStretch(); self.remove_button = self.button(actions, 'Remove', self.remove)
        edges = QHBoxLayout(); middle.addLayout(edges)
        self.top_button = self.button(edges, 'Move to top', lambda: self.move_edge(True))
        self.bottom_button = self.button(edges, 'Move to bottom', lambda: self.move_edge(False))
        right = self.panel(columns, 5)
        self.label(right, 'SONG LIBRARY', 'eyebrow')
        self.search = QLineEdit(); self.search.setPlaceholderText('Search songs, artists, albums')
        self.search.setClearButtonEnabled(True); right.addWidget(self.search)
        self.library_count = self.label(right, '', 'muted')
        self.songs = self.track_list(virtual=True)
        self.songs.setLayoutMode(QListView.Batched)
        self.songs.setBatchSize(256)
        self.songs.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.song_model = SongLibraryModel(state['tracks'], self.songs)
        self.songs.setModel(self.song_model)
        self.song_stack, self.empty_songs = self.list_area(right, self.songs, '')
        self.add_tracks_button = self.button(right, '← Add to playlist', self.add)
        footer = QHBoxLayout(); footer.setSpacing(10); layout.addLayout(footer)
        self.message = self.label(footer, '', 'muted'); self.message.setWordWrap(True)
        footer.setStretch(0, 1)
        self.button(footer, 'Cancel', self.reject)
        self.export_button = self.button(footer, 'Export M3U…', self.export_m3u)
        self.button(footer, 'Save to iPod', self.accept_checked, 'primary')
        self.playlists.currentRowChanged.connect(self.select)
        self.name.textEdited.connect(self.rename)
        self.search.textChanged.connect(self.filter)
        self.songs.doubleClicked.connect(lambda _: self.add())
        self.members.itemSelectionChanged.connect(self.update_actions)
        self.songs.selectionModel().selectionChanged.connect(self.update_actions)
        self.render(); self.filter('')

    def label(self, layout, text, name):
        item = QLabel(text); item.setObjectName(name); item.setTextFormat(Qt.PlainText)
        layout.addWidget(item); return item

    def panel(self, layout, stretch, width=None):
        frame = QFrame(); frame.setObjectName('playlistPanel')
        if width:
            frame.setFixedWidth(width)
        layout.addWidget(frame, stretch)
        inner = QVBoxLayout(frame); inner.setContentsMargins(8, 9, 8, 8); inner.setSpacing(6)
        return inner

    def track_list(self, virtual=False):
        view = SongLibraryView() if virtual else QListWidget()
        view.setObjectName('playlistTracks')
        view.setSelectionMode(QAbstractItemView.ExtendedSelection)
        view.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        view.setVerticalScrollMode(QAbstractItemView.ScrollPerPixel)
        view.setItemDelegate(TrackDelegate(view)); view.setUniformItemSizes(True)
        return view

    def list_area(self, layout, view, text):
        stack = QStackedWidget(); stack.addWidget(view)
        empty = QLabel(text); empty.setObjectName('playlistEmpty'); empty.setWordWrap(True)
        empty.setAlignment(Qt.AlignCenter); empty.setTextFormat(Qt.PlainText)
        stack.addWidget(empty); layout.addWidget(stack, 1)
        return stack, empty

    def button(self, layout, text, action, name=None):
        item = QPushButton(text); item.setCursor(Qt.PointingHandCursor); item.setAutoDefault(False)
        if name:
            item.setObjectName(name)
        item.clicked.connect(action); layout.addWidget(item); return item

    def update_actions(self):
        active = self.slot is not None
        selected = self.members.selectedItems()
        rows = [self.members.row(item) for item in selected]
        self.delete_button.setEnabled(active)
        self.remove_button.setEnabled(active and bool(selected))
        row_set = set(rows)
        self.up_button.setEnabled(active and any(row > 0 and row - 1 not in row_set for row in rows))
        self.down_button.setEnabled(active and any(row + 1 < self.members.count() and row + 1 not in row_set for row in rows))
        self.top_button.setEnabled(self.up_button.isEnabled())
        self.bottom_button.setEnabled(self.down_button.isEnabled())
        self.add_tracks_button.setEnabled(active and next(self.song_model.selected_rows(self.songs.selectionModel().selection()), None) is not None)
        self.new_button.setEnabled(self.playlists.count() < 16)
        self.import_button.setEnabled(self.playlists.count() < 16)
        self.export_button.setEnabled(active)

    def render(self, selected=None):
        with QSignalBlocker(self.playlists):
            self.playlists.clear()
            for slot, playlist in enumerate(self.state['lists']):
                if playlist['name']:
                    item = QListWidgetItem(playlist['name']); item.setData(Qt.UserRole, slot)
                    item.setToolTip(playlist['name']); self.playlists.addItem(item)
                    if slot == selected:
                        self.playlists.setCurrentItem(item)
            if self.playlists.currentRow() < 0 and self.playlists.count():
                self.playlists.setCurrentRow(0)
        self.select()
        self.playlist_stack.setCurrentIndex(0 if self.playlists.count() else 1)
        self.count.setText(f'{self.playlists.count()} / 16 playlists')

    def select(self, *_):
        item = self.playlists.currentItem(); self.slot = item.data(Qt.UserRole) if item else None
        self.name.setEnabled(item is not None)
        playlist = self.state['lists'][self.slot] if item else dict(name='', ids=[])
        self.name.setText(playlist['name'])
        # Clearing a selection emits repeatedly; update controls after the
        # complete list is ready instead of rescanning it for each removed row.
        with QSignalBlocker(self.members):
            self.members.clear()
            for identity in playlist['ids']:
                self.members.addItem(self.member_item(identity))
        self.track_count.setText(f"{len(playlist['ids'])} / 1024 songs" if item else 'No playlist selected')
        self.empty_members.setText('This playlist is empty\n\nSelect songs on the right and choose Add to playlist.' if item else
                                  'Create a playlist to get started\n\nYour songs will appear here in playback order.')
        self.member_stack.setCurrentIndex(0 if playlist['ids'] else 1)
        self.message.setText('Changes are saved when you choose Save to iPod.')
        self.update_actions()

    def member_item(self, identity):
        member = QListWidgetItem(self.state['tracks'].get(identity, f'Missing track · {identity:016x}'))
        member.setToolTip(member.text())
        return member

    def members_changed(self):
        ids = self.state['lists'][self.slot]['ids']
        self.track_count.setText(f'{len(ids)} / 1024 songs')
        self.member_stack.setCurrentIndex(0 if ids else 1)
        self.message.setText('Changes are saved when you choose Save to iPod.')
        self.update_actions()

    def create(self):
        for slot, playlist in enumerate(self.state['lists']):
            if not playlist['name']:
                names = {entry['name'] for entry in self.state['lists']}
                name, suffix = 'New playlist', 2
                while name in names:
                    name = f'New playlist {suffix}'; suffix += 1
                playlist.update(name=name, ids=[]); self.render(slot)
                self.name.setFocus(); self.name.selectAll(); return

    def delete(self):
        if self.slot is not None:
            self.state['lists'][self.slot] = dict(name='', ids=[]); self.render()

    def rename(self, text):
        if self.slot is not None:
            if not text.strip() or '\0' in text or len(text.encode('utf-8')) > 31:
                self.message.setText('Each playlist needs a name of 1–31 UTF-8 bytes.'); return
            self.state['lists'][self.slot]['name'] = text
            self.playlists.currentItem().setText(text)
            self.playlists.currentItem().setToolTip(text)
            self.message.setText('Changes are saved when you choose Save to iPod.')

    def add(self):
        if self.slot is None:
            return
        ids = self.state['lists'][self.slot]['ids']
        selection = self.songs.selectionModel().selection()
        count = sum(sum(self.song_model.shown[span.top():span.bottom() + 1]) for span in selection)
        if len(ids) + count > 1024:
            self.message.setText('A playlist can contain at most 1024 songs.'); return
        added = [self.song_model.songs[row][0] for row in sorted(self.song_model.selected_rows(selection))]
        ids.extend(added)
        with QSignalBlocker(self.members):
            self.members.clearSelection()
            for identity in added:
                self.members.addItem(self.member_item(identity))
            self.members.setCurrentRow(-1)
        self.members_changed()

    def remove(self):
        if self.slot is not None:
            ids = self.state['lists'][self.slot]['ids']
            with QSignalBlocker(self.members):
                for row in sorted((self.members.row(item) for item in self.members.selectedItems()), reverse=True):
                    ids.pop(row)
                    self.members.takeItem(row)
                self.members.clearSelection()
                self.members.setCurrentRow(-1)
            self.members_changed()

    def move(self, direction):
        selected = self.members.selectedItems()
        if self.slot is None or not selected:
            return
        ids = self.state['lists'][self.slot]['ids']
        chosen = [self.members.item(row).isSelected() for row in range(len(ids))]
        positions = range(1, len(ids)) if direction < 0 else range(len(ids) - 2, -1, -1)
        for row in positions:
            next_row = row + direction
            if chosen[row] and not chosen[next_row]:
                ids[row], ids[next_row] = ids[next_row], ids[row]
                chosen[row], chosen[next_row] = chosen[next_row], chosen[row]
        self._rebuild_members(chosen)

    def move_edge(self, top):
        if self.slot is None or not self.members.selectedItems():
            return
        ids = self.state['lists'][self.slot]['ids']
        chosen = [self.members.item(row).isSelected() for row in range(len(ids))]
        picked = [identity for identity, selected in zip(ids, chosen) if selected]
        rest = [identity for identity, selected in zip(ids, chosen) if not selected]
        ids[:] = picked + rest if top else rest + picked
        self._rebuild_members(([True] * len(picked) + [False] * len(rest)) if top else
                              ([False] * len(rest) + [True] * len(picked)))

    def _rebuild_members(self, chosen):
        with QSignalBlocker(self.members):
            self.members.clear()
            for row, identity in enumerate(self.state['lists'][self.slot]['ids']):
                item = self.member_item(identity)
                self.members.addItem(item)
                item.setSelected(chosen[row])
        selected = self.members.selectedItems()
        if selected:
            self.members.setCurrentItem(selected[0], QItemSelectionModel.SelectionFlag.NoUpdate)
            self.members.scrollToItem(selected[0])
        self.message.setText('Changes are saved when you choose Save to iPod.')
        self.update_actions()

    def filter(self, text):
        count = self.songs.filter(text)
        self.library_count.setText(f'{count:,} songs' + (' matching your search' if text else ' available'))
        self.song_stack.setCurrentIndex(0 if count else 1)
        self.empty_songs.setText('No matching songs\n\nTry another title, artist, or album.' if text else
                                'No prepared songs\n\nPrepare your music to add songs to playlists.')
        self.update_actions()

    def _import_name(self, source):
        stem = source.stem.strip().replace('\0', '') or 'Imported playlist'
        name = ''
        for character in stem:
            if len((name + character).encode('utf-8')) > 31:
                break
            name += character
        names = {entry['name'] for entry in self.state['lists']}
        original, suffix = name, 2
        while name in names:
            ending = f' {suffix}'
            name = original.encode('utf-8')[:31 - len(ending)].decode('utf-8', 'ignore') + ending
            suffix += 1
        return name

    def import_m3u(self):
        if self.playlists.count() >= 16:
            self.message.setText('The iPod supports at most 16 playlists. Delete one before importing.'); return
        filename, _ = QFileDialog.getOpenFileName(self, 'Import M3U playlist', '',
                                                   'M3U playlists (*.m3u *.m3u8);;All files (*)')
        if not filename:
            return
        source = Path(filename)
        try:
            matched, unmatched = playlist_content.preview_m3u(source, self.state['tracks'],
                                                                 self.state.get('paths'))
        except (OSError, UnicodeError, ValueError) as error:
            QMessageBox.warning(self, 'Cannot import M3U', str(error)); return
        review = QDialog(self); review.setWindowTitle('Review M3U import')
        review.resize(570, 420); review.setMinimumSize(380, 300)
        layout = QVBoxLayout(review)
        summary = QLabel(f'{len(matched)} matched songs · {len(unmatched)} unmatched entries\n'
                         'A playlist holds at most 1024 songs; the iPod holds 16 playlists.')
        summary.setWordWrap(True); layout.addWidget(summary)
        details = QPlainTextEdit(); details.setReadOnly(True)
        details.setPlainText('\n'.join([f'MATCHED ({len(matched)}):'] +
            [self.state['tracks'][identity] for identity in matched[:30]] +
            ([f'… and {len(matched) - 30} more'] if len(matched) > 30 else []) +
            ['', f'UNMATCHED ({len(unmatched)}):'] +
            [f'Line {number}: {entry} ({reason})' for number, entry, reason in unmatched]))
        layout.addWidget(details, 1)
        buttons = QHBoxLayout(); layout.addLayout(buttons); buttons.addStretch()
        self.button(buttons, 'Cancel', review.reject)
        apply = self.button(buttons, 'Import matched songs', review.accept, 'primary')
        apply.setEnabled(bool(matched) and len(matched) <= 1024)
        if len(matched) > 1024:
            summary.setText(summary.text() + '\nImport is blocked because it exceeds 1024 songs.')
        if review.exec() != QDialog.Accepted:
            return
        for slot, playlist in enumerate(self.state['lists']):
            if not playlist['name']:
                playlist.update(name=self._import_name(source), ids=matched)
                self.render(slot)
                self.message.setText(f'Imported {len(matched)} songs; {len(unmatched)} unmatched entries were skipped after review.')
                return

    def export_m3u(self):
        if self.slot is None:
            return
        playlist = self.state['lists'][self.slot]
        filename, _ = QFileDialog.getSaveFileName(self, 'Export M3U playlist',
                                                   playlist['name'] + '.m3u8',
                                                   'M3U playlists (*.m3u8 *.m3u)')
        if not filename:
            return
        try:
            data = playlist_content.export_m3u(playlist['ids'], self.state['tracks'],
                                                self.state.get('paths'))
            Path(filename).write_bytes(data)
        except (OSError, ValueError) as error:
            QMessageBox.warning(self, 'Cannot export M3U', str(error)); return
        self.message.setText(f'Exported {len(playlist["ids"])} songs to M3U.')

    def accept_checked(self):
        if self.slot is not None and self.name.text() != self.state['lists'][self.slot]['name']:
            self.message.setText('Each playlist needs a name of 1–31 UTF-8 bytes.'); return
        self.accept()

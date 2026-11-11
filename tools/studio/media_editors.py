"""Game and screen editors and their operation results."""
from PySide6.QtCore import QSignalBlocker, Qt
from PySide6.QtWidgets import QDialog, QFileDialog, QListWidgetItem

from tools.content import games
from tools.studio import screen_settings


class GamesController:
    def __init__(self, window):
        self.window = window

    def games_changed(self, *_):
        window = self.window
        if window.games_loaded_for:
            window.game_dirty = {i for i, check in enumerate(window.game_checks) if check.isChecked()} != window.game_saved
            window.save_choices_draft()
            window.games_status.setText('Unsaved game selection. Update games on iPod, then eject safely.' if window.game_dirty else
                                        'Game selection matches the iPod.')

    def recover_games(self):
        window = self.window
        if not window.identity or window.busy or not games.available(window.identity) or not window.game_read_failed:
            return
        if not window.dialog('Replace damaged game settings',
                             'The game selection could not be read. Select games below, then choose Update games on iPod to replace the damaged settings.',
                             'Choose games'):
            return
        window.games_loaded_for = window.identity['root']
        window.game_saved = None
        window.game_dirty = True
        window.game_read_failed = False
        window.game_recover_requested=True
        for index, check in enumerate(window.game_checks):
            with QSignalBlocker(check):
                check.setChecked(index in games.supported(window.identity))
        window.load_choices_draft('games')
        window.games_status.setText('Choose games, then update the iPod to replace its damaged selection.')
        window.save_choices_draft()
        window.update_controls()

    def load_games(self):
        window = self.window
        if (window.identity and window.device_connected and games.available(window.identity) and not window.busy
                and window.games_loaded_for != window.identity['root']):
            identity = window.identity
            window.work('games', lambda: games.choices(identity), 'Reading installed games…')

    def save_games(self):
        window = self.window
        if not window.identity or not window.device_connected or window.busy or not games.available(window.identity):
            return
        selected = {index for index, check in enumerate(window.game_checks) if check.isChecked()}
        identity = window.identity
        recover=window.game_recover_requested
        window.work('save_games', lambda: games.save(identity, selected, recover=recover), 'Updating games on iPod…')

    def import_game(self):
        window=self.window
        if not window.identity or window.busy:return
        path,_=QFileDialog.getOpenFileName(window,'Install prepared game','','Linnet game (*.LGM *.lgm)')
        if not path:return
        identity=window.identity
        window.work('import_game',lambda:games.import_package(identity,path),'Installing game package…')

    def remove_game(self):
        window=self.window;item=window.custom_games.currentItem()
        if not item or not window.identity or window.busy:return
        identity=window.identity;game_id=item.data(Qt.UserRole)
        window.work('remove_game',lambda:games.remove_package(identity,game_id),'Removing game package…')

class ScreensController:
    def __init__(self, window):
        self.window = window

    def render_screen_items(self, *_):
        window = self.window
        name = window.screen_picker.currentText()
        window.screen_fixed.setVisible(name == 'Settings')
        labels = screen_settings.SCREENS[name]
        selected = window.screen_state[name]
        available, unavailable = screen_settings.item_availability(name, window.identity)
        locked = screen_settings.locked_items(name)
        window.screen_unavailable.setText('Not available on this device: ' + ', '.join(labels[i] for i in unavailable))
        window.screen_unavailable.setVisible(bool(unavailable))
        order = [i for i in selected if i in available] + [i for i in available if i not in selected]
        with QSignalBlocker(window.screen_items):
            window.screen_items.clear()
            for i in order:
                shown = 'Games' if name == 'Home' and i == 7 and window.identity and games.available(window.identity) else labels[i]
                item = QListWidgetItem(shown); item.setData(Qt.UserRole, i)
                item.setFlags(item.flags() | Qt.ItemIsUserCheckable | Qt.ItemIsDragEnabled)
                item.setCheckState(Qt.Checked if i in selected else Qt.Unchecked)
                if i in locked:
                    item.setFlags(item.flags() & ~Qt.ItemIsUserCheckable)
                    item.setText(labels[i] + ' (always included)')
                    item.setToolTip('This item is always included by the device.')
                elif labels[i] in screen_settings.HINTS:
                    item.setToolTip(screen_settings.HINTS[labels[i]])
                window.screen_items.addItem(item)
        window.screen_items.setCurrentRow(0)

    def screen_items_changed(self, *_):
        window = self.window
        name = window.screen_picker.currentText()
        selected = [window.screen_items.item(i).data(Qt.UserRole) for i in range(window.screen_items.count())
                    if window.screen_items.item(i).checkState() == Qt.Checked]
        window.screen_state[name] = selected
        window.screen_dirty = window.screen_state != window.screen_saved
        window.save_choices_draft()
        window.screen_status.setText('Unsaved changes. Save to iPod, then eject safely to apply on startup.'
                                   if all(window.screen_state.values()) else 'Select at least one item on each screen before saving.')
        window.update_controls()

    def screen_item_hint(self, item, *_):
        window = self.window
        window.screen_hint.setText(item.toolTip() if item else '')

    def move_screen_item(self, direction):
        window = self.window
        row = window.screen_items.currentRow(); target = row + direction
        if row < 0 or not 0 <= target < window.screen_items.count():
            return
        item = window.screen_items.takeItem(row); window.screen_items.insertItem(target, item)
        window.screen_items.setCurrentRow(target); self.screen_items_changed()

    def reset_screen(self):
        window = self.window
        name = window.screen_picker.currentText()
        window.screen_state[name] = screen_settings.defaults(window.identity)[name]
        self.render_screen_items(); self.screen_items_changed()

    def load_screens(self):
        window = self.window
        if window.identity and window.device_connected and not window.busy and window.screen_loaded_for != window.identity['root']:
            identity = window.identity
            window.work('screens', lambda: screen_settings.read(identity), 'Reading screen settings…')

    def reload_screens(self):
        window = self.window
        if not window.identity or not window.device_connected or window.busy:
            return
        if window.screen_dirty and not window.dialog('Reload screen settings', 'Discard unsaved desktop menu edits and load the current iPod menus?', 'Reload'):
            return
        window.screen_dirty = False
        window.save_choices_draft()
        window.screen_loaded_for = None
        self.load_screens()

    def save_screens(self):
        window = self.window
        if not window.identity or not window.device_connected or window.busy:
            return
        state = {name: list(rows) for name, rows in window.screen_state.items()}
        try:
            screen_settings.encode(state)
        except ValueError as error:
            window.show_error(str(error)); return
        identity = window.identity
        base = {name: list(rows) for name, rows in window.screen_base.items()} if window.screen_base is not None else None
        window.work('save_screens', lambda: screen_settings.save(identity, state, base), 'Saving screen settings…')


class EditorWorkspace:
    """Game and screen editors and their operation results."""

    def _complete_screens(self, result):
        self.screen_state = result; self.screen_saved = {name: list(rows) for name, rows in result.items()}
        self.screen_base = {name: list(rows) for name, rows in result.items()}
        self.screen_loaded_for = self.identity['root']
        self.load_choices_draft('screens')
        self.screens_controller.render_screen_items()
        self.screen_status.setText('Unsaved screen choices restored for this iPod. Save to apply them.' if self.screen_dirty else
                                   'Loaded from iPod. Changes apply after saving and restarting Linnet.')
        self.update_controls(); return

    def _complete_save_screens(self, result):
        self.screen_state = result
        self.screen_base = {name: list(rows) for name, rows in result.items()}
        self.screens_controller.render_screen_items()
        self.screen_loaded_for = self.identity['root']
        self.screen_saved = {name: list(rows) for name, rows in self.screen_state.items()}
        self.screen_dirty = False; self.save_choices_draft()
        self.screen_status.setText('Saved to iPod. Eject safely to apply on startup.'); return

    def _complete_games(self, result):
        selected=result['selected'] if isinstance(result,dict) else result
        self.custom_games.clear()
        for row in result.get('custom',[]) if isinstance(result,dict) else []:
            item=QListWidgetItem(row['title']);item.setData(Qt.UserRole,row['id'])
            item.setToolTip(row['id']+'\n'+row['description']);self.custom_games.addItem(item)
        for index, check in enumerate(self.game_checks):
            with QSignalBlocker(check):
                check.setChecked(index in selected)
        self.games_loaded_for = self.identity['root']
        self.game_saved = set(selected); self.game_read_failed = False;self.game_recover_requested=False
        self.load_choices_draft('games')
        self.games_status.setText('Unsaved game choices restored for this iPod. Update to apply them.' if self.game_dirty else
                                  'Loaded from iPod. Uncheck a game to remove its installed package. Saved progress is retained.')
        self.update_controls(); return

    def _complete_save_games(self, result):
        self.game_saved = {i for i, check in enumerate(self.game_checks) if check.isChecked()}
        self.game_dirty = False;self.game_recover_requested=False; self.save_choices_draft()
        self.games_status.setText('Games updated. Eject safely and restart Linnet to apply.')
        self.update_controls(); return

    def _complete_installed_game(self, result):
        self.games_loaded_for=None
        self.games_status.setText('Game packages updated. Eject safely to apply them.')
        self.games_controller.load_games()

    def _complete_playlists(self, result):
        self.show_progress('Playlists loaded.'); self.edit_playlists(result); return

    def _complete_save_playlists(self, result):
        self.show_progress('Playlists saved to iPod. Eject safely when finished.'); return

    def _complete_apple_selection(self, result):
        self.show_progress('Apple OS folder choices saved. Checking availability…')
        self.refresh(); return

    def _complete_apple_update(self, result):
        self.prep_results.setPlainText(result['summary']); self.prep_results.show()
        self.show_progress(result['summary']); self.refresh(); return

    def open_playlists(self):
        if self.identity and self.device_connected and not self.busy:
            from tools.content import playlists
            identity = self.identity
            self.work('playlists', lambda: playlists.read(identity), 'Reading iPod playlists…')

    def edit_playlists(self, state):
        from tools.content import playlists
        from tools.studio.playlist_editor import PlaylistEditor
        editor = PlaylistEditor(self, state)
        if editor.exec() == QDialog.Accepted:
            self.playlist_draft = state
            self.work('save_playlists', lambda: playlists.save(state), 'Saving playlists to iPod…')

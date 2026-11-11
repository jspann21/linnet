"""Per-device preparation and editor drafts in host settings."""
import hashlib
import json
import os
from pathlib import Path

from PySide6.QtCore import QSignalBlocker

from tools.content import games, media_manager as media
from tools.studio import screen_settings


def _stored_source(name, root):
    path = Path(name)
    return '@device/' + path.relative_to(root).as_posix() if path.is_relative_to(root) else name


def _restored_source(kind, name, root):
    if not name.startswith('@device/'):
        return name
    relative = Path(name[8:])
    parts = tuple(part.casefold() for part in relative.parts)
    if (kind != 'Music' or relative.is_absolute() or relative.drive or '..' in parts or
            not parts or not (parts[0] == 'music' or parts[:2] == ('ipod_control', 'music'))):
        return None
    return str(root / relative)


class DraftWorkspace:
    """Per-device preparation and editor drafts in host settings."""

    @staticmethod
    def choices_key(identity):
        fields = (identity['target_id'], identity['disk_serial'], identity['volume_serial'])
        return 'device_choices/v1/' + hashlib.sha256('\0'.join(fields).encode('utf-8')).hexdigest()

    def save_choices_draft(self):
        if not self.identity:
            return
        key = self.choices_key(self.identity)
        try:
            draft = json.loads(self.settings.value(key, '{}'))
            if not isinstance(draft, dict) or draft.get('version') != 1:
                draft = {}
        except (TypeError, ValueError):
            draft = {}
        draft['version'] = 1
        if self.screen_dirty:
            draft['screens'] = {name: list(rows) for name, rows in self.screen_state.items()}
            draft['screens_base'] = self.screen_base
        elif self.screen_loaded_for == self.identity['root']:
            draft.pop('screens', None)
            draft.pop('screens_base', None)
        if self.game_dirty:
            draft['games'] = [i for i, check in enumerate(self.game_checks) if check.isChecked()]
        elif self.games_loaded_for == self.identity['root']:
            draft.pop('games', None)
        if len(draft) == 1:
            self.settings.remove(key)
        else:
            self.settings.setValue(key, json.dumps(draft))
        self.settings.sync()

    def load_choices_draft(self, section):
        if not self.identity:
            return
        try:
            draft = json.loads(self.settings.value(self.choices_key(self.identity), '{}'))
            if not isinstance(draft, dict) or draft.get('version') != 1:
                return
            if section == 'screens' and isinstance(draft.get('screens'), dict):
                state = draft['screens']
                if set(state) != set(screen_settings.SCREENS) or any(
                    not isinstance(rows, list) or len(rows) != len(set(rows)) or
                    any(type(item) is not int or not 0 <= item < len(screen_settings.SCREENS[name])
                        for item in rows)
                    for name, rows in state.items()):
                    raise ValueError('Saved screen choices are invalid.')
                base = draft.get('screens_base')
                if base is not None:
                    screen_settings.encode(base)
                self.screen_state = {name: list(rows) for name, rows in state.items()}
                self.screen_base = base
                self.screen_dirty = self.screen_state != self.screen_saved
            elif section == 'games' and isinstance(draft.get('games'), list):
                chosen = draft['games']
                games.validate_selection(chosen)
                for index, check in enumerate(self.game_checks):
                    with QSignalBlocker(check):
                        check.setChecked(index in chosen)
                self.game_dirty = set(chosen) != self.game_saved
        except (TypeError, ValueError, KeyError):
            self.record_diagnostic('choices', 'InvalidDraft', 'Saved device choices could not be restored.', 'partial')

    @staticmethod
    def draft_key(identity):
        fields = (identity['target_id'], identity['disk_serial'], identity['volume_serial'])
        digest = hashlib.sha256('\0'.join(fields).encode('utf-8')).hexdigest()
        return f'preparation_draft/v3/{digest}'

    def save_draft(self):
        if not self.identity:
            return
        root = Path(self.identity['root'])
        videos = []
        for path in sorted(self.video_selection):
            try:
                videos.append(Path(path).relative_to(root).as_posix())
            except ValueError:
                continue
        draft = dict(version=3, target_id=self.identity['target_id'],
                     disk_serial=self.identity['disk_serial'],
                     volume_serial=self.identity['volume_serial'],
                     pending=[[kind, _stored_source(name, root)]
                              for kind, name in self.pending],
                     video_mode=self.video_mode, videos=videos,
                     outcomes=[dict(row, source=_stored_source(row['source'], root))
                               for key, row in self.queue_outcomes.items()
                               if key in self.pending_keys])
        self.settings.setValue(self.draft_key(self.identity), json.dumps(draft))
        self.settings.sync()

    def validate_draft(self):
        self.draft_issues = {}
        if not self.identity:
            return
        allowed = media.supported_kinds(self.identity)
        for kind, name in self.pending:
            reason = None
            if kind not in allowed:
                reason = f'{kind} is unavailable on this iPod'
            else:
                try:
                    path = Path(name)
                    if not path.exists():
                        reason = 'Source is missing from this computer'
                    elif path.is_symlink():
                        reason = 'Linked sources must be selected again'
                except (OSError, ValueError):
                    reason = 'Saved source path is invalid'
            if reason:
                self.draft_issues[(kind, os.path.normcase(name))] = reason

    def load_draft(self, identity):
        self.pending, self.pending_keys = [], set()
        self.video_selection, self.video_mode = set(), 'all'
        self.draft_issues, self.stale_videos, self.queue_outcomes = {}, 0, {}
        raw = self.settings.value(self.draft_key(identity), '')
        try:
            draft = json.loads(raw)
            if (not isinstance(draft, dict) or draft.get('version') != 3 or
                    any(draft.get(key) != identity[key] for key in
                        ('target_id', 'disk_serial', 'volume_serial')) or
                    not isinstance(draft.get('pending'), list) or
                    not isinstance(draft.get('videos'), list) or
                    not isinstance(draft.get('outcomes'), list) or
                    draft.get('video_mode') not in ('all', 'selected', 'none')):
                return
            for item in draft['pending']:
                if (not isinstance(item, list) or len(item) != 2 or
                        not all(isinstance(value, str) for value in item) or
                        item[0] not in media.KINDS or not item[1]):
                    continue
                kind, name = item[0], _restored_source(item[0], item[1], Path(identity['root']))
                if name is None:
                    continue
                key = (kind, os.path.normcase(name))
                if key not in self.pending_keys:
                    self.pending.append((kind, name))
                    self.pending_keys.add(key)
            for value in draft['videos']:
                if not isinstance(value, str) or '\\' in value:
                    continue
                relative = Path(value)
                if (relative.is_absolute() or relative.drive or '..' in relative.parts or
                        not relative.parts or relative.parts[0].casefold() != 'videos'):
                    continue
                self.video_selection.add(str(Path(identity['root']) / relative))
            for row in draft['outcomes']:
                if (not isinstance(row, dict) or
                        not all(isinstance(row.get(key), str) for key in
                                ('kind', 'source', 'status', 'reason')) or
                        row['status'] not in ('skipped', 'unfinished')):
                    continue
                source = _restored_source(row['kind'], row['source'], Path(identity['root']))
                if source is None:
                    continue
                key = (row['kind'], os.path.normcase(source))
                if key in self.pending_keys:
                    self.queue_outcomes[key] = dict(row, source=source)
            self.video_mode = draft['video_mode']
            self.validate_draft()
        except (TypeError, ValueError):
            return

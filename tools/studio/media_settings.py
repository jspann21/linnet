"""Host cache, conversion tools, diagnostics, and help UI."""
import json
import os
import re
import shutil
import subprocess
import sys
import tomllib
from datetime import datetime
from pathlib import Path

from PySide6.QtCore import QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import QApplication, QDialog, QFileDialog, QTextEdit

from tools.content import host_cache
from tools.content.media_manager import format_size as size
from tools.studio import media_reader

from .media_widgets import box, button, label


def linnet_version():
    try:
        config = Path(__file__).resolve().parents[2] / 'config' / 'system.toml'
        return str(tomllib.loads(config.read_text(encoding='utf-8'))['system']['version'])
    except (OSError, KeyError, ValueError, TypeError):
        return 'unavailable'


class SettingsWorkspace:
    """Host cache, conversion tools, diagnostics, and help UI."""

    def refresh_cache(self):
        if not self.busy:
            self.work('cache_size', host_cache.overview, 'Measuring computer cache and protected storage…')

    def open_computer_folder(self, path=None):
        if self.busy:
            return
        try:
            path = host_cache._plain(path) if path else host_cache.cache_base()
            path.mkdir(parents=True, exist_ok=True)
            self.open_folder(path)
        except (OSError, ValueError) as error:
            self.show_error(str(error))

    def clear_computer_cache(self):
        if self.busy or not self.cache_info:
            return
        if self.dialog('Clear computer cache',
                       f'Clear reusable media files (last measured: {size(self.cache_info["bytes"])}). '
                       'Thumbnails, metadata and conversions will be recreated when needed. '
                       'The next preparation may take longer.\n\n'
                       'Startup backups, recovery copies, saved selections and iPod files are kept.',
                       'Clear cache', danger=True):
            self.covers.invalidate()
            self.work('cache_clear', host_cache.clear, 'Clearing reusable computer cache files…')

    def change_cache_location(self):
        if self.busy:
            return
        initial = self.cache_info['location'] if self.cache_info else str(host_cache.default_base())
        folder = QFileDialog.getExistingDirectory(self, 'Choose an empty folder for the media cache', str(Path(initial).parent))
        if not folder or Path(folder) == Path(initial):
            return
        if not self.dialog('Move computer cache',
                           f'Move reusable cache files to:\n{folder}\n\n'
                           'Copies are verified before the location changes. Startup backups, recovery copies '
                           'and saved selections keep their existing locations.', 'Move cache'):
            return
        roots = [self.identity['root']] if self.identity else []
        self.covers.invalidate()
        self.work('cache_move', lambda: host_cache.relocate(folder, device_roots=roots,
                  progress=self.events.progress.emit), 'Moving and verifying reusable cache files…')

    def _complete_cache(self, result):
        self.cache_info = result
        self.cache_size.setText(f'{size(result["bytes"])} reusable cache · {result["files"]:,} files')
        self.cache_location.setText(result['location'])
        self.cache_protected.setText(
            f'{size(result["startup_bytes"])} startup backups · '
            f'{size(result["retained_bytes"])} recovery and saved files.\n'
            'Kept when you clear or move the cache.')
        if result['errors']:
            self.cache_size.setText(self.cache_size.text() + '\nSome files could not be measured or changed; they were kept.')
        self.cache_size.setToolTip('\n'.join(result['errors'][:10]))
        if 'freed_bytes' in result:
            self.show_progress(f'Cleared {size(result["freed_bytes"])} of reusable cache. Protected files were kept.')
        else:
            self.show_progress('Computer storage checked. Startup backups and recovery data are protected.')
        self.update_controls()

    def locate_tools(self):
        folder = QFileDialog.getExistingDirectory(self, 'Choose the folder containing FFmpeg and FFprobe')
        if folder:
            self.update_tools(candidate=(str(Path(folder) / 'ffmpeg.exe'),
                                         str(Path(folder) / 'ffprobe.exe')))

    def update_tools(self, *, candidate=None):
        self.tool_check_id += 1
        request = self.tool_check_id
        included = getattr(sys, 'frozen', False)
        if included:
            from tools.studio.targets import ROOT
            paths = tuple(str(ROOT / 'conversion-tools' / (name + '.exe')) for name in ('ffmpeg', 'ffprobe'))
        else:
            paths = candidate or (self.ffmpeg, self.ffprobe)
        if self.tools_label is not None:
            self.tools_label.setText('Checking FFmpeg and FFprobe…')
        def check():
            errors = []
            validated = {}
            for name, value in zip(('FFmpeg', 'FFprobe'), paths):
                try:
                    if not value and candidate is None and not included:
                        value = shutil.which(name.lower())
                    if not value:
                        errors.append(f'{name} is not configured.')
                        continue
                    path = Path(value).resolve(strict=True)
                    if not path.is_file() or path.stem.casefold() != name.casefold():
                        raise OSError(f'expected {name.lower()}.exe')
                    result = subprocess.run([str(path), '-version'], stdin=subprocess.DEVNULL,
                                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                            timeout=5, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
                    if result.returncode:
                        raise OSError(f'version check exited with status {result.returncode}')
                    validated[name] = str(path)
                except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
                    errors.append(f'{name} at {value} could not run: {error}')
            self.events.tools_checked.emit(request, dict(candidate=candidate, validated=validated, errors=errors))
        self.tool_pool.submit(check)

    def record_diagnostic(self, operation, error_type, message, status='failed', cancel_requested=False):
        """Keep bounded support context without source paths or device identifiers."""
        cleaned = str(message)
        cleaned = re.sub(r'(?i)\b[\w.+-]+@[\w.-]+\.[A-Z]{2,}\b', '[email]', cleaned)
        cleaned = re.sub(r'(?i)https?://[^\r\n]+', '[url]', cleaned)
        cleaned = re.sub(r'(?i)\b[A-Z]:[\\/][^\r\n]+', '[path]', cleaned)
        cleaned = re.sub(r'\\\\[^\r\n]+', '[path]', cleaned)
        cleaned = re.sub(r'/(?:Users|home)/[^\r\n]+', '[path]', cleaned)
        cleaned = re.sub(r'(?<!\w)(?:\.{1,2}[\\/])?[\w .-]+[\\/][^\r\n]+', '[path]', cleaned)
        cleaned = re.sub(r'(?i)\b[\w][\w .-]*\.[A-Z0-9]{1,6}\b', '[file]', cleaned)
        identifiers = set(self._recent_private_identifiers)
        if self.identity:
            identifiers.update(str(self.identity.get(field)) for field in
                ('root', 'drive', 'disk_serial', 'volume_serial', 'volume_unique_id') if self.identity.get(field))
        for value in sorted(identifiers, key=len, reverse=True):
            cleaned = re.sub(re.escape(value), '[redacted]', cleaned, flags=re.IGNORECASE)
        cleaned = re.sub(r'\b[0-9A-F]{16,}\b', '[identifier]', cleaned, flags=re.IGNORECASE)
        entry = dict(time=datetime.now().astimezone().isoformat(timespec='seconds'),
                     operation=str(operation)[:40], type=str(error_type)[:60],
                     status=status, cancel_requested=bool(cancel_requested),
                     target=(self.identity or {}).get('target_id', ''), message=cleaned[:500])
        try:
            history = json.loads(self.settings.value('diagnostics/history/v1', '[]'))
            if not isinstance(history, list):
                history = []
        except (TypeError, ValueError):
            history = []
        history.append(entry)
        self.settings.setValue('diagnostics/history/v1', json.dumps(history[-50:]))
        self.settings.sync()

    def collect_hardware_report(self):
        if self.busy:return
        self.work('hardware_report', lambda: media_reader.read('support', identity=self.identity,
            cancelled=self.stop_event.is_set), 'Collecting hardware report…')

    def _complete_hardware_report(self, report):
        from tools.studio.device_support import sharing, contact_link
        contact=sharing()
        self.hardware_report = report
        self.update_setup_status()
        dialog = QDialog(self); dialog.setWindowTitle('Hardware support report'); dialog.resize(700, 500)
        layout = box(dialog, margins=(20,20,20,20))
        layout.addWidget(label(report['summary'], 'section', True))
        layout.addWidget(label(report['privacy'], 'muted', True))
        layout.addWidget(label(contact['instructions'], 'muted', True))
        details = QTextEdit(); details.setReadOnly(True)
        details.setPlainText(json.dumps(report, indent=2)); layout.addWidget(details)
        def save():
            path, _ = QFileDialog.getSaveFileName(dialog, 'Save hardware report', 'linnet-hardware-report.json', 'JSON files (*.json)')
            if path:
                try:
                    Path(path).write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
                    self.show_progress('Report saved. Attach it to your support request.')
                except OSError as error:self.show_error(f'Could not save hardware report: {error}')
        actions=box(horizontal=True);actions.addWidget(button('Save report…',save,'primary'))
        actions.addWidget(button('Copy report',lambda: QApplication.clipboard().setText(json.dumps(report,indent=2))))
        if contact_link():
            actions.addWidget(button('Contact Linnet support',lambda: QDesktopServices.openUrl(QUrl(contact_link()))))
        actions.addStretch();actions.addWidget(button('Close',dialog.reject));layout.addLayout(actions)
        dialog.exec()
        if report.get('recognized') and not self.identity and not self.link_connection and not self.busy:
            # A report can recognize a disk after an earlier discovery failed.
            # Re-run normal discovery rather than treating report data as an
            # installation identity or leaving the old failure on screen.
            self.discover()

    def show_diagnostics(self):
        try:
            entries = json.loads(self.settings.value('diagnostics/history/v1', '[]'))
            if not isinstance(entries, list):
                entries = []
        except (TypeError, ValueError):
            entries = []
        dialog = QDialog(self); dialog.setWindowTitle('Diagnostics'); dialog.resize(700, 440)
        layout = box(dialog, margins=(20, 20, 20, 20))
        layout.addWidget(label('Recent operation errors', 'section'))
        layout.addWidget(label('The last 50 errors are saved on this computer. Device serials, paths, and email addresses are removed from this history.', 'muted', True))
        details = QTextEdit(); details.setReadOnly(True)
        details.setAccessibleName('Recent diagnostic events')
        details.setPlainText('\n\n'.join(f"{e.get('time', '')} · {e.get('operation', '')} · {e.get('type', '')}\n{e.get('message', '')}"
                                         for e in entries) if entries else 'No errors recorded in this session history.')
        layout.addWidget(details)
        actions = box(horizontal=True)
        def export():
            path, _ = QFileDialog.getSaveFileName(dialog, 'Export diagnostics', 'linnet-diagnostics.json', 'JSON files (*.json)')
            if path:
                try:
                    Path(path).write_text(json.dumps(dict(version=1, errors=entries), indent=2) + '\n', encoding='utf-8')
                except OSError as error:
                    self.show_error(f'Could not export diagnostics: {error}')
        actions.addWidget(button('Export support file', export)); actions.addStretch()
        actions.addWidget(button('Hardware report…', lambda: (dialog.accept(), self.collect_hardware_report())))
        actions.addWidget(button('Close', dialog.accept, 'primary'))
        layout.addLayout(actions); dialog.exec()

    def show_help(self):
        self.dialog('About Linnet Media Manager',
                    f'Linnet system version: {linnet_version()}\n\n'
                    'Linnet Media Manager helps prepare music, photos, videos, and notes for supported iPods.\n\n'
                    'Connect your iPod in Disk Mode, choose Find or switch iPod, and use Preparation to review changes. '
                    'Use Eject safely when finished.\n\n' +
                    ('FFmpeg and FFprobe are included for audio and video conversion. '
                     'Includes FFmpeg and x264 under GPLv3 or later; licenses and matching source accompany the app. '
                     if getattr(sys, 'frozen', False) else
                     'For audio and video conversion from source, choose your FFmpeg and FFprobe folder under Settings > Advanced preparation. ') +
                    'Diagnostics in the footer shows recent errors and exports a privacy-filtered support file.',
                    'Close', cancel=False)

    def show_error(self, message, *, alert=False):
        self.notice.setText(message); self.notice.show(); self.show_progress(message)
        if self.stack.currentWidget() != self.library:
            self.prep_results.setPlainText(message); self.prep_results.show()
        if alert or (self.stack.currentWidget() != self.library and self.page != 'Preparation'):
            self.dialog('Needs attention', message, 'OK', cancel=False)

"""Preparation plans, source queues, and publication results."""
import os
import sys
from pathlib import Path

from PySide6.QtCore import QItemSelection, QItemSelectionModel, QSignalBlocker, QTimer, QUrl, Qt
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (
    QAbstractItemView,
    QDialog,
    QFileDialog,
    QHeaderView,
    QLineEdit,
    QTableView,
    QTextEdit,
    QTreeWidget,
    QTreeWidgetItem,
)

from tools.content import apple_os, media_library, media_manager as media, media_status
from tools.content.media_manager import format_size as size

from .media_models import MediaModel
from .media_widgets import ComboBox, box, button, label


class PreparationWorkspace:
    """Preparation plans, source queues, and publication results."""

    def open_queued(self, *_):
        indices = self.queue_table.selectionModel().selectedRows()
        if not self.busy and len(indices) == 1:
            self.open_folder(self.queue_model.rows[indices[0].row()]['path'])

    def update_queue_controls(self, *_):
        count = len(self.queue_table.selectionModel().selectedRows())
        self.queue_remove.setEnabled(not self.busy and count > 0)
        self.queue_open.setEnabled(not self.busy and count == 1)

    def _complete_queue(self, result, error):
        context = self.queue_context
        self.queue_context = None
        if context is None or not context['dialog'].isVisible():
            return
        if not self.same_device(self.identity, context['identity']):
            context['error'].setText('The selected iPod changed while checking these sources. Choose them again.')
            context['error'].show()
            return
        if error:
            context['error'].setText(error)
            context['error'].show()
            return
        additions = result
        self.pending.extend(additions)
        self.pending_keys.update((kind, os.path.normcase(path)) for kind, path in additions)
        for kind, path in additions:
            self.queue_outcomes.pop((kind, os.path.normcase(path)), None)
        self.validate_draft()
        self.save_draft()
        kind, source = context['kind'], Path(context['paths'][0])
        self.settings.setValue(f'import_folder/{kind}', str(source if context['folder'] else source.parent))
        self.prep_checks[kind].setChecked(True)
        context['dialog'].accept()
        self.update_preparation()
        self.navigate('Preparation')
        QTimer.singleShot(0, lambda: self.preparation_scroll.ensureWidgetVisible(self.queue_table))
        self.show_progress(f'{len(additions):,} new queue entries added.' if additions else
                           'These sources are already queued. Review them in Preparation.')
        return

    def _complete_plan(self, result):
        context = self.plan_context
        self.plan_context = None
        if context is None or not self.same_device(self.identity, context['identity']):
            self.show_error('The selected iPod changed while preparing the plan. Review it again.')
            return
        copies = ', '.join(f'{kind}: {count}' for kind, count in result['copy_kinds'].items() if count)
        summary = [f"Prepare: {', '.join(result['selected'])}"]
        if result['videos']:
            noun = 'video' if result['videos'] == 1 else 'videos'
            summary.append(f"Prepare {result['videos']:,} {noun} for playback on this iPod.")
        if result['copies']:
            noun = 'file' if result['copies'] == 1 else 'files'
            summary.append(f"Copy {result['copies']:,} {noun} ({size(result['bytes'])}) · {copies}")
        elif not result['videos']:
            summary.append('Update the selected iPod libraries.')
        preview = [f"File copies: {result['copies']:,} ({size(result['bytes'])})" +
                   (f' · {copies}' if copies else ''),
                   'Videos are prepared separately from these file copies.',
                   f"Videos to prepare: {result['videos']:,}"]
        if result['conversion']:
            preview.append('Video conversion is included. Final output sizes depend on encoding.')
        profile = context['identity'].get('media_profile', {})
        if result['videos'] and profile.get('video_backend') == 'ipvf':
            preview.append(f"This iPod uses Linnet IPVF video ({profile['video_width']} × "
                           f"{profile['video_height']}, {profile['video_fps']} fps).")
        if result.get('audio_conversions'):
            preview.append(f"Convert on computer: {result['audio_conversions']:,} music files; source files stay in place")
        if result.get('audio_unplayable'):
            summary.append(f"Needs conversion: {result['audio_unplayable']:,} music files will be copied but skipped for playback")
        for issue in result.get('audio_issues', []):
            preview.append(f"{Path(issue['source']).name}: {issue['reason']}")
        if 'device_peak_bytes' in result:
            summary.append(f"iPod space budget: {size(result['device_peak_bytes'])} · {size(result['free'])} available")
        if 'computer_peak_bytes' in result:
            summary.append(f"Computer space budget: {size(result['computer_peak_bytes'])} · {size(result['computer_free'])} available")
        preview.append('Space budgets are conservative estimates for prepared media, temporary files, '
                       'and recovery copies. Final media sizes may be smaller.')
        if result['reusable']:
            preview.append('Likely reusable prepared libraries: ' + ', '.join(result['reusable']))
        if result['existing_unverified']:
            preview.append(f"Existing originals checked during preparation: {result['existing_unverified']:,} files")
        if result['excluded']:
            preview.append(f"Queue entries outside this selection: {result['excluded']:,}")
        for source, description in list(result['previews'].items())[:5]:
            preview.append(f'{Path(source).name}: {description}')
        if len(result['previews']) > 5:
            preview.append(f"…and {len(result['previews']) - 5:,} more queued sources")
        preview.append('Sources, available space, and prepared outputs are checked again during preparation.')
        reconciliation = result.get('reconciliation', [])
        changes = []
        if reconciliation:
            changes.append(f'{len(reconciliation)} changes to existing iPod content require approval:')
            root = Path(context['identity']['root'])
            for action in reconciliation:
                destination = Path(action['destination'])
                try:
                    shown = destination.relative_to(root).as_posix()
                except ValueError:
                    shown = destination.name
                if action['action'] == 'replace':
                    changes.append(f"Replace {action['kind']}: {shown} with {Path(action['source']).name} "
                                   f"({size(action['old_bytes'])} → {size(action['new_bytes'])})")
                elif action['action'] == 'retire-library':
                    changes.append(f"Move empty {action['kind']} library to Trash: {shown}")
                elif action['action'] == 'prune-video':
                    changes.append(f"Remove missing video entry: {action.get('title') or shown}")
        title = 'Review preparation changes' if reconciliation else 'Review preparation plan'
        dialog = QDialog(self); dialog.setWindowTitle(title); dialog.setMinimumWidth(580)
        layout = box(dialog, margins=(28, 28, 28, 24), spacing=16)
        layout.addWidget(label(title, 'section'))
        layout.addWidget(label('\n'.join(summary), None, True))
        layout.addWidget(label('Space budgets include temporary files and recovery copies.', 'muted', True))
        if changes:
            layout.addWidget(label('Review changes to existing content', 'section'))
            review = QTextEdit(); review.setReadOnly(True)
            review.setAccessibleName('Changes to existing iPod content')
            review.setPlainText('\n'.join(changes)); review.setMinimumHeight(160)
            layout.addWidget(review)
        details = QTextEdit(); details.setReadOnly(True)
        details.setAccessibleName('Preparation details')
        details.setPlainText('\n'.join(preview)); details.setMinimumHeight(180)
        details.hide()
        toggle = button('Show details', lambda: None); toggle.setCheckable(True)
        def show_details(visible):
            details.setVisible(visible)
            toggle.setText('Hide details' if visible else 'Show details')
            dialog.adjustSize()
        toggle.toggled.connect(show_details)
        layout.addWidget(toggle, 0, Qt.AlignLeft); layout.addWidget(details)
        actions = box(horizontal=True); actions.addStretch()
        actions.addWidget(button('Cancel', dialog.reject))
        actions.addWidget(button('Approve changes and prepare' if reconciliation else 'Start preparation',
                                 dialog.accept, 'primary'))
        layout.addLayout(actions)
        accepted = dialog.exec() == QDialog.Accepted
        if accepted:
            context['approval']['actions'] = reconciliation
            self.submitted, self.submitted_videos = context['pending'], context['submitted_videos']
            self.prep_results.hide()
            self.work('prepare', context['run'], 'Preparing your selected media…')
        return

    def _complete_review_skipped(self, result):
        identity, rows = result
        if not self.same_device(self.identity, identity):
            self.show_error('The selected iPod changed while skipped files were loading. Try again.')
        elif rows:
            self.show_skipped_dialog(identity, rows)
        else:
            self.show_progress('No skipped files on this iPod.')
        return

    def _complete_video_recovery_options(self, review):
        options = review.get('options', [])
        if review.get('status') != 'damaged':
            self.show_progress('The video catalog can be read now. Refresh to update the library.')
            return
        if not options:
            self.dialog('Video catalog needs repair',
                'No complete, validated on-device backup is available. Linnet kept the damaged catalog and prepared videos untouched. Recover the catalog from a trusted backup before preparing videos.',
                'Close', cancel=False)
            return
        dialog = QDialog(self); dialog.setWindowTitle('Review video catalog backups'); dialog.resize(640, 460)
        layout = box(dialog, margins=(20, 20, 20, 20))
        layout.addWidget(label('Restore a verified video catalog', 'section'))
        layout.addWidget(label('This replaces the damaged catalog with the selected on-device backup. The damaged bytes are saved for recovery. Videos added after that backup may need preparation again.', 'muted', True))
        picker = ComboBox(); picker.setAccessibleName('Video catalog backup')
        for option in options:
            picker.addItem(f"{option['name']} · {option['entries']} entries", option)
        layout.addWidget(picker)
        details = QTextEdit(); details.setReadOnly(True); details.setAccessibleName('Backup video titles')
        layout.addWidget(details)
        def describe():
            option = picker.currentData()
            titles = option.get('titles', []) if option else []
            details.setPlainText('\n'.join(str(title) for title in titles[:100]) +
                (f'\n… {len(titles) - 100} more titles' if len(titles) > 100 else ''))
        picker.currentIndexChanged.connect(describe); describe()
        actions = box(horizontal=True)
        actions.addWidget(button('Cancel', dialog.reject)); actions.addStretch()
        actions.addWidget(button('Restore selected backup', dialog.accept, 'primary'))
        layout.addLayout(actions)
        if dialog.exec() == QDialog.Accepted:
            option = picker.currentData()
            identity = self.identity
            if not identity or not self.device_connected:
                self.show_error('The iPod changed. Review its video catalog again.')
                return
            self.work('restore_video_catalog', lambda: media_library.restore_video_catalog_backup(
                identity, option['name'], option['sha256'], review['catalog_sha256']),
                'Restoring the reviewed video catalog backup…')

    def _complete_prepare(self, result):
        failures = result.get('errors', [])
        if failures:
            self.record_diagnostic('prepare', 'PreparationResult',
                f'{len(failures)} preparation step(s) failed or were skipped.', 'partial',
                bool(result.get('stopped')))
            for failure in failures[:10]:
                self.record_diagnostic('prepare', failure.get('status', 'StepError'),
                    failure.get('message', 'Preparation step needs attention.'), 'partial',
                    bool(result.get('stopped')))
        self.last_preparation = (result['identity'], result)
        self.update_controls()
        lines = [result.get('summary') or ('Preparation complete.' if result['complete'] else 'Preparation needs attention.')]
        lines.extend(e['message'] for e in result.get('errors', []))
        self.prep_results.setPlainText('\n'.join(lines)); self.prep_results.show()
        outcomes = {(row['kind'], os.path.normcase(row['source'])): row
                    for row in result.get('source_outcomes', [])}
        submitted_keys = {(kind, os.path.normcase(path)) for kind, path in self.submitted}
        finished = {key for key, row in outcomes.items() if row['status'] == 'completed'}
        self.pending = [item for item in self.pending
                        if (item[0], os.path.normcase(item[1])) not in submitted_keys or
                        (item[0], os.path.normcase(item[1])) not in finished]
        self.pending_keys = {(kind, os.path.normcase(path)) for kind, path in self.pending}
        self.queue_outcomes = {key: row for key, row in outcomes.items()
                               if key in self.pending_keys and row['status'] != 'completed'}
        if result['complete']:
            self.video_selection.difference_update(self.submitted_videos)
        self.validate_draft()
        self.save_draft()
        self.update_preparation(); self.refresh()

    def review_video_catalog(self):
        if self.busy or not self.identity or not self.device_connected:
            return
        identity = self.identity
        self.work('video_recovery_options', lambda: media_library.video_catalog_recovery_options(identity),
                  'Checking on-device video catalog backups…')

    def queue_status(self, kind, path):
        key = (kind, os.path.normcase(path))
        if key in self.draft_issues:
            return self.draft_issues[key]
        outcome = self.queue_outcomes.get(key)
        if outcome:
            action = ('Use Review skipped files to retry.' if outcome['status'] == 'skipped' and kind != 'Videos'
                      else 'Retry with Prepare.')
            return f"{outcome['status'].title()}: {outcome['reason']} {action}"
        return 'Ready'

    def update_preparation(self):
        supported = media.supported_kinds(self.identity) if self.identity else ()
        unavailable = [kind for kind in self.prep_cards if self.identity and kind not in supported]
        self.prep_unavailable.setText(f"{' and '.join(unavailable).capitalize()} aren't available on this iPod."
                                      if unavailable else '')
        self.prep_unavailable.setVisible(bool(unavailable))
        for kind, card in self.prep_cards.items():
            card.setVisible(kind not in unavailable)
        for kind, text in self.prep_labels.items():
            info = (self.inventory or {}).get(kind)
            if self.identity and kind not in supported:
                text.setText(f'{kind} playback is unavailable on this iPod model.')
            elif not info:
                text.setText('Open Disk Mode to check this library.' if self.link_connection else
                             'Reading this library…' if self.identity else
                             'Connect an iPod in Disk Mode to check this library.')
            elif kind == 'Videos':
                scope = ('All existing videos included' if self.video_mode == 'all' else
                         f'{len(self.video_selection)} selected for conversion' if self.video_mode == 'selected' else
                         'No conversions selected')
                text.setText(f"{info['count']:,} {'file' if info['count'] == 1 else 'files'} · {self.readiness.get(kind, 'Not checked')} · " +
                             scope)
            else:
                summary = f"{info['count']:,} {'file' if info['count'] == 1 else 'files'} · {self.readiness.get(kind, 'Not checked')}"
                if kind == 'Music':
                    differences = []
                    if info.get('apple_only'):
                        count = info['apple_only']
                        differences.append(f"{count:,} Apple-only {'track' if count == 1 else 'tracks'} excluded")
                    if info.get('not_in_apple'):
                        count = info['not_in_apple']
                        differences.append(f"{count:,} {'file' if count == 1 else 'files'} not listed in Apple OS")
                    if differences:
                        summary += '\n' + ' · '.join(differences)
                text.setText(summary)
        self.queue_model.reset([dict(title=Path(path).name, kind=kind, path=path,
                                     status=self.queue_status(kind, path))
                                for kind, path in self.pending],
                               [('title', 'FILE'), ('kind', 'TYPE'), ('status', 'STATUS'),
                                ('path', 'SOURCE')])
        self.queue_table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        self.queue_table.horizontalHeader().setSectionResizeMode(3, QHeaderView.Stretch)
        self.queue_table.setVisible(bool(self.pending)); self.queue_remove.setVisible(bool(self.pending))
        self.queue_open.setVisible(bool(self.pending)); self.update_queue_controls()
        self.queue_table.setFixedHeight(min(200, max(75, self.queue_table.horizontalHeader().height() + len(self.pending) * self.queue_table.verticalHeader().defaultSectionSize() + 4)))
        summary = (f'{len(self.pending):,} files or folders queued. Unchanged media is reused.' if self.pending else
                   'Open Disk Mode to prepare existing iPod files or add media from your computer.' if self.link_connection else
                   'Nothing queued. You can prepare existing iPod files or add media from your computer.')
        if self.draft_issues:
            summary += f' {len(self.draft_issues):,} saved source(s) need attention.'
        if self.stale_videos:
            summary += f' {self.stale_videos:,} saved video choice(s) are no longer on this iPod.'
        self.prep_summary.setText(summary)
        if self.apple_info:
            eligibility = '' if self.apple_info['supported'] else self.apple_info.get('eligibility', '')
            self.apple_eligibility.setText(eligibility)
            self.apple_eligibility.setVisible(bool(eligibility))
            lines = [self.apple_info['status']]
            for entry in self.apple_info['details']:
                if entry['kind'] == 'Apple library':
                    if entry['status'] != self.apple_info['status']:
                        lines.append(entry['status'])
                    continue
                if entry['count'] or entry['status'] == 'Needs update':
                    lines.append(f"{entry['kind']}: {entry['count']:,} files · {entry['status']}")
            if self.apple_info.get('issues'):
                lines.append(self.apple_info['issues'][0])
            self.apple_status.setText('\n'.join(lines))
        else:
            self.apple_eligibility.setText('')
            self.apple_eligibility.setVisible(False)
            self.apple_status.setText('Open Disk Mode to check Apple OS availability.' if self.link_connection else
                                      'Checking Apple OS availability…' if self.identity else
                                      'Connect an iPod in Disk Mode to check Apple OS availability.')

    def update_preparation_connection(self):
        self.prep_connection.setVisible(self.identity is None)
        link = self.link_connection
        opening = bool(self.busy and self.active_operation and self.active_operation.tag == 'link_disk')
        can_open = bool(link and link.get('can_open_disk'))
        self.prep_connection_title.setText('Opening Disk Mode…' if opening else 'Disk Mode needed to prepare media')
        self.prep_connection_detail.setText(
            'Keep USB connected. Media Manager will read your libraries when the iPod reconnects.' if opening else
            'Your iPod is connected through Linnet Link. Preparation needs Disk Mode to read and write media. '
            'Choose Open Disk Mode and keep USB connected.' if can_open else
            'Your iPod is connected through Linnet Link, but Disk Mode is not available yet. '
            'Open iPod setup to review connection and setup options.' if link else
            'Connect your iPod in Disk Mode to read its libraries and prepare media. '
            'See iPod setup for connection steps.')
        self.prep_connection_button.setText('Opening Disk Mode…' if opening else 'Open Disk Mode' if can_open else
                                            'Open iPod setup' if link else 'View connection steps')
        self.prep_connection_button.setEnabled(not self.busy)

    def connect_preparation(self):
        if self.busy:
            return
        if self.link_connection and self.link_connection.get('can_open_disk'):
            self.open_link_disk()
        else:
            self.navigate('iPod setup')

    def choose_apple_folders(self):
        if self.busy or not self.apple_info or not self.apple_info['supported']:
            return
        dialog = QDialog(self); dialog.setWindowTitle('Choose folders for Apple OS')
        dialog.resize(620, 640)
        layout = box(dialog, margins=(24, 24, 24, 24), spacing=12)
        layout.addWidget(label('Available in Apple OS', 'section'))
        selectable = self.apple_info['selectable_roots']
        names = [{'Music': 'music', 'Photos': 'photo', 'Videos': 'video'}[kind] for kind in selectable]
        kinds = names[0] if len(names) == 1 else ', '.join(names[:-1]) + ' and ' + names[-1]
        description = (f'Select {kinds} folders on this iPod, outside the Apple-managed library. '
                       'Update the Apple library to use their media in Apple OS too. Files stay in their original locations.')
        layout.addWidget(label(description, 'muted', True))
        layout.addWidget(label('Selecting a folder includes its subfolders. Choose individual subfolders for a narrower selection.', 'muted', True))
        tree = QTreeWidget(); tree.setObjectName('appleFolders')
        tree.setHeaderLabels(['FOLDER', 'APPLE OS'])
        tree.header().setSectionResizeMode(0, QHeaderView.Stretch)
        tree.header().setSectionResizeMode(1, QHeaderView.ResizeToContents)
        items = {}
        selected = self.apple_info['selected']
        for row in self.apple_info['folders']:
            path, kind = row['path'], row['kind']
            parent_path = path.rpartition('/')[0]
            parent = items.get(parent_path)
            item = QTreeWidgetItem(parent or tree, [path.rpartition('/')[2], ''])
            item.setData(0, Qt.UserRole, path)
            items[path] = item
            if kind in selectable:
                item.setFlags(item.flags() | Qt.ItemIsUserCheckable)
                checked = any(path.casefold() == name.casefold() or
                              path.casefold().startswith(name.casefold() + '/') for name in selected)
                item.setCheckState(0, Qt.Checked if checked else Qt.Unchecked)
                item.setText(1, 'Choose for Apple OS')
            else:
                item.setText(1, 'Visible in Apple Notes')
                item.setToolTip(1, 'Apple reads compatible text files in the Notes folder automatically.')
        for path, item in reversed(list(items.items())):
            if path.split('/')[0] not in selectable or not item.childCount():
                continue
            states = {item.child(index).checkState(0) for index in range(item.childCount())}
            # Child choices never imply selecting the parent: it may contain
            # direct media files or acquire additional subfolders later.
            if item.checkState(0) != Qt.Checked:
                item.setCheckState(0, Qt.PartiallyChecked if states != {Qt.Unchecked} else Qt.Unchecked)
        def inherited_choices():
            for path, item in items.items():
                if path.split('/')[0] not in selectable:
                    continue
                parent, ancestor = item.parent(), None
                while parent:
                    if parent.checkState(0) == Qt.Checked:
                        ancestor = parent
                    parent = parent.parent()
                if ancestor:
                    name = ancestor.data(0, Qt.UserRole)
                    item.setFlags(item.flags() & ~Qt.ItemIsUserCheckable)
                    item.setText(1, f'Included by {name}')
                    item.setToolTip(0, f'Clear {name} first to choose individual subfolders.')
                else:
                    item.setFlags(item.flags() | Qt.ItemIsUserCheckable)
                    item.setText(1, 'Choose for Apple OS')
                    item.setToolTip(0, 'Includes this folder and all its subfolders.')
        inherited_choices()
        tree.expandToDepth(0); layout.addWidget(tree, 1)
        def changed(item, column):
            if column != 0 or item.data(0, Qt.UserRole).split('/')[0] not in selectable:
                return
            with QSignalBlocker(tree):
                if item.checkState(0) != Qt.PartiallyChecked:
                    stack = [item.child(index) for index in range(item.childCount())]
                    while stack:
                        child = stack.pop()
                        child.setCheckState(0, item.checkState(0))
                        stack.extend(child.child(index) for index in range(child.childCount()))
                parent = item.parent()
                while parent and parent.data(0, Qt.UserRole):
                    states = {parent.child(index).checkState(0) for index in range(parent.childCount())}
                    if parent.checkState(0) != Qt.Checked or states != {Qt.Checked}:
                        parent.setCheckState(0, Qt.PartiallyChecked if states != {Qt.Unchecked} else Qt.Unchecked)
                    parent = parent.parent()
                inherited_choices()
        tree.itemChanged.connect(changed)
        actions = box(horizontal=True)
        actions.addStretch(); actions.addWidget(button('Cancel', dialog.reject, 'quiet'))
        actions.addWidget(button('Save folders', dialog.accept, 'primary'))
        layout.addLayout(actions)
        if dialog.exec() != QDialog.Accepted:
            return
        folders = [path for path, item in items.items()
                   if path.split('/')[0] in selectable and item.checkState(0) == Qt.Checked and
                   (not path.rpartition('/')[0] or
                    items[path.rpartition('/')[0]].checkState(0) != Qt.Checked)]
        identity = self.identity
        self.work('apple_selection', lambda: apple_os.save_selection(identity, folders),
                  'Saving Apple OS folder choices…')

    def update_apple_os(self):
        if self.busy or not self.identity or not self.apple_info or not self.apple_info.get('can_update'):
            return
        identity = self.identity
        ffprobe = self.verified_ffprobe
        self.work('apple_update', lambda: apple_os.update(identity, ffprobe=ffprobe,
                                                          progress=self.events.progress.emit,
                                                          cancelled=self.stop_event.is_set),
                  'Updating the Apple library…')

    def queue_existing_audio(self):
        if self.busy or not self.identity:
            return
        music_roots = (Path(self.identity['root']) / 'Music',
                       Path(self.identity['root']) / 'iPod_Control' / 'Music')
        additions = []
        for row in self.rows:
            if (row['kind'] != 'Music' or row['category'] != 'original' or
                    not str(row['status']).startswith('Needs conversion:')):
                continue
            path = Path(row['path'])
            key = ('Music', os.path.normcase(str(path)))
            if any(path.is_relative_to(root) for root in music_roots) and key not in self.pending_keys:
                additions.append(('Music', str(path)))
                self.pending_keys.add(key)
        self.pending.extend(additions)
        self.convert_audio.setChecked(True)
        self.prep_checks['Music'].setChecked(True)
        self.validate_draft()
        self.save_draft()
        self.update_preparation()
        self.navigate('Preparation')
        self.show_progress(f'{len(additions):,} incompatible iPod tracks queued for host conversion.')

    def queue_selected_pcm(self):
        if self.busy or not self.identity:
            return
        music_roots = (Path(self.identity['root']) / 'Music',
                       Path(self.identity['root']) / 'iPod_Control' / 'Music')
        rows = self.selected()
        if not rows or any(row['kind'] != 'Music' or row['category'] != 'original' or
                           row['path'].casefold().endswith('.linnet.wav') or
                           not any(Path(row['path']).is_relative_to(root) for root in music_roots)
                           for row in rows):
            return
        additions = [('Music', row['path']) for row in rows
                     if ('Music', os.path.normcase(row['path'])) not in self.pending_keys]
        self.pending.extend(additions)
        self.pending_keys.update((kind, os.path.normcase(path)) for kind, path in additions)
        self.convert_audio.setChecked(True)
        self.prep_checks['Music'].setChecked(True)
        self.validate_draft()
        self.save_draft()
        self.update_preparation()
        self.navigate('Preparation')
        self.show_progress(f'{len(additions):,} iPod tracks queued for PCM preparation on the computer.')

    def add_media(self):
        if self.busy or not self.identity:
            return
        dialog = QDialog(self); dialog.setWindowTitle('Add media'); dialog.setMinimumWidth(480)
        layout = box(dialog, margins=(28, 28, 28, 28), spacing=16)
        layout.addWidget(label('Bring something new', 'section'))
        layout.addWidget(label('Choose files from your computer. Review the queue in Preparation before copying them to your iPod.', 'muted', True))
        picker = ComboBox(); picker.addItems(media.supported_kinds(self.identity)); picker.setCurrentText(self.page if self.page in media.KINDS else 'Music'); layout.addWidget(picker)
        picker.setAccessibleName('Media type to add')
        explanation = label('', 'muted', True); layout.addWidget(explanation)
        def explain():
            explanation.setText('Folders include videos in all subfolders. Other file types are ignored. Originals stay on your computer.' if picker.currentText() == 'Videos' else
                                'Folders include all subfolders and their files, including artwork and other supporting files. Choose individual files to copy only selected media. Originals stay on your computer.')
        picker.currentTextChanged.connect(explain); explain()
        error_label = label('', 'notice', True); error_label.hide(); layout.addWidget(error_label)
        def choose(folder=False):
            kind = picker.currentText()
            initial = self.settings.value(f'import_folder/{kind}', str(Path.home()))
            if folder:
                path = QFileDialog.getExistingDirectory(dialog, f'Add {kind.lower()} folder', initial); paths = [path] if path else []
            else:
                allowed = media.video_source_extensions(self.identity) if kind == 'Videos' else media.extensions(kind)
                paths, _ = QFileDialog.getOpenFileNames(dialog, f'Add {kind.lower()}', initial, kind + ' (' + ' '.join('*' + ext for ext in sorted(allowed)) + ')')
            if paths:
                try:
                    self.queue_sources(kind, paths, dialog, error_label, folder)
                except (OSError, ValueError) as error:
                    error_label.setText(str(error)); error_label.show(); return
        actions = box(horizontal=True); actions.addWidget(button('Choose files', lambda: choose(), 'primary', 'plus')); actions.addWidget(button('Choose a folder', lambda: choose(True), None, 'folder')); layout.addLayout(actions)
        layout.addWidget(button('Cancel', dialog.reject, 'quiet'), 0, Qt.AlignRight)
        dialog.exec()

    def queue_sources(self, kind, paths, dialog, error_label, folder):
        if not self.identity or self.busy:
            raise ValueError('Connect an iPod and wait for the current operation to finish before adding media.')
        if kind not in media.supported_kinds(self.identity):
            raise ValueError(f'{kind} is unavailable on this iPod.')
        identity, known = self.identity, set(self.pending_keys)
        def validate():
            root = Path(identity['root']).resolve(strict=True)
            allowed = media.video_source_extensions(identity) if kind == 'Videos' else media.extensions(kind)
            additions = []
            for value in paths:
                source = Path(value).resolve(strict=True)
                if source.is_relative_to(root):
                    raise ValueError('This source is already on your iPod. Use Refresh to find existing media, then prepare it from Preparation.')
                if not source.is_dir() and (not source.is_file() or source.suffix.lower() not in allowed):
                    raise ValueError(f'{source.name} is not a supported {kind.lower()} file. Choose a supported file or a folder.')
                key = (kind, os.path.normcase(str(source)))
                if key not in known:
                    additions.append((kind, str(source)))
                    known.add(key)
            return additions
        self.queue_context = dict(kind=kind, paths=paths, dialog=dialog, error=error_label,
                                  folder=folder, identity=identity)
        self.work('queue', validate, 'Checking selected sources…')

    def remove_queued(self):
        if self.busy:
            return
        chosen = {index.row() for index in self.queue_table.selectionModel().selectedRows()}
        self.pending = [item for index, item in enumerate(self.pending) if index not in chosen]
        self.pending_keys = {(kind, os.path.normcase(path)) for kind, path in self.pending}
        self.queue_outcomes = {key: row for key, row in self.queue_outcomes.items() if key in self.pending_keys}
        self.validate_draft()
        self.save_draft()
        self.update_preparation()

    def choose_videos(self):
        if not self.inventory or self.busy:
            return
        allowed = media.video_source_extensions(self.identity)
        native = media.native_video_extensions(self.identity)
        sources = [p for p in self.inventory['Videos']['files']
                   if Path(p).suffix.lower() in allowed and Path(p).suffix.lower() not in native]
        search_rows = [(dict(title=Path(path).name, path=path), path.casefold()) for path in sources]
        dialog = QDialog(self); dialog.setWindowTitle('Choose videos to convert'); dialog.resize(700, 520)
        layout = box(dialog, margins=(24, 24, 24, 24)); layout.addWidget(label('Choose video originals', 'section'))
        layout.addWidget(label('Device-format videos are included automatically. Choose all, selected, or no additional conversions.', 'muted', True))
        mode = ComboBox()
        for title, value in (('Convert all existing videos', 'all'),
                             ('Convert selected videos', 'selected'),
                             ('No conversions', 'none')):
            mode.addItem(title, value)
        mode.setCurrentIndex(mode.findData(self.video_mode))
        layout.addWidget(mode)
        search = QLineEdit(); search.setPlaceholderText('Find a video…'); layout.addWidget(search)
        table = QTableView(); model = MediaModel(table); table.setModel(model); table.setSelectionBehavior(QAbstractItemView.SelectRows); table.setSelectionMode(QAbstractItemView.ExtendedSelection); table.setEditTriggers(QAbstractItemView.NoEditTriggers); table.verticalHeader().hide(); layout.addWidget(table)
        selected = set(self.video_selection)
        search_timer = QTimer(dialog)
        search_timer.setSingleShot(True)
        search_timer.setInterval(160)
        def render():
            if model.rows:
                selected.difference_update(r['path'] for r in model.rows)
                selected.update(model.rows[i.row()]['path'] for i in table.selectionModel().selectedRows())
            query = search.text().casefold()
            model.reset([row for row, folded in search_rows if query in folded],
                        [('title', 'VIDEO'), ('path', 'LOCATION')])
            selection = QItemSelection()
            start = None
            for index in range(len(model.rows) + 1):
                chosen = index < len(model.rows) and model.rows[index]['path'] in selected
                if chosen and start is None:
                    start = index
                elif not chosen and start is not None:
                    selection.select(model.index(start, 0), model.index(index - 1, 0))
                    start = None
            if not selection.isEmpty():
                table.selectionModel().select(selection, QItemSelectionModel.Select | QItemSelectionModel.Rows)
            table.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        search.textChanged.connect(search_timer.start)
        search_timer.timeout.connect(render)
        render()
        def accept():
            search_timer.stop()
            render(); self.video_selection = selected; self.video_mode = mode.currentData()
            self.stale_videos = 0
            self.save_draft()
            self.update_preparation(); dialog.accept()
        layout.addWidget(button('Apply video choices', accept, 'primary'), 0, Qt.AlignRight); dialog.exec()

    def prepare(self):
        if self.busy or not self.device_connected or not self.inventory:
            return
        selected = [kind for kind, check in self.prep_checks.items() if check.isChecked() and check.isEnabled()]
        if not selected:
            self.show_error('Choose at least one media type to prepare.'); return
        self.validate_draft()
        stale = [reason for kind, name in self.pending if kind in selected
                 if (reason := self.draft_issues.get((kind, os.path.normcase(name))))]
        if stale:
            self.show_error(f'{len(stale)} queued source(s) need attention: {stale[0]}. Remove or restore them before preparing.')
            self.update_preparation()
            return
        identity = self.identity; pending = [item for item in self.pending if item[0] in selected]
        all_pending = list(self.pending)
        videos = (None if self.video_mode == 'all' else
                  list(self.video_selection) if self.video_mode == 'selected' else [])
        dimension = 2048 if self.photo_detail.currentIndex() else 640
        online_artwork = self.online_artwork.isChecked()
        force, verification = self.rebuild.isChecked(), 'full' if self.verify_audio.isChecked() else 'metadata'
        ffmpeg, ffprobe = self.verified_ffmpeg, self.verified_ffprobe
        convert_audio = self.convert_audio.isChecked()
        approval = {'actions': None}
        def run():
            return media.run(identity, selected, pending, photo_dimension=dimension, force=force,
                music_verification=verification, ffmpeg=ffmpeg, ffprobe=ffprobe, video_selection=videos,
                convert_audio=convert_audio,
                online_artwork=online_artwork, approved_reconciliation=approval['actions'],
                progress=self.events.progress.emit, progress_detail=self.events.conversion.emit,
                progress_stage=self.events.stage.emit,
                cancelled=self.stop_event.is_set)
        self.plan_context = dict(identity=identity, pending=pending,
                                 submitted_videos=set(videos or ()) if 'Videos' in selected else set(),
                                 approval=approval, run=run)
        def inspect_plan():
            result = media_status.plan(identity, all_pending, selected, videos,
                current_inventory=self.inventory, photo_dimension=dimension,
                music_verification=verification, force=force, online_artwork=online_artwork,
                ffprobe=ffprobe, ffmpeg=ffmpeg, convert_audio=convert_audio,
                cancelled=self.stop_event.is_set)
            if result['conversion'] and (not ffmpeg or not ffprobe):
                raise ValueError('Video preparation could not start. ' +
                    ('Reinstall Linnet and try again.' if getattr(sys, 'frozen', False) else
                     'Choose FFmpeg and FFprobe in Settings → Advanced preparation.'))
            return result
        self.work('plan', inspect_plan, 'Checking the preparation plan…')

    def sync_videos(self):
        if self.identity and self.device_connected and not self.busy:
            identity = self.identity
            self.work('sync', lambda: media_library.organize_videos(identity), 'Syncing the video folder…')

    def show_preparation_details(self):
        if not self.identity or self.last_preparation is None:
            return
        identity, report = self.last_preparation
        if not self.same_device(self.identity, identity):
            return
        lines = [report.get('summary') or 'Preparation result', '']
        for step in report.get('steps', [])[:500]:
            name = step.get('file') or step.get('added') or step.get('renamed') or step.get('kind') or 'Step'
            status = step.get('status') or ('copied' if 'added' in step else 'renamed' if 'renamed' in step else 'prepared')
            lines.append(f'{status}: {name}')
        if len(report.get('steps', [])) > 500:
            lines.append(f'... {len(report["steps"]) - 500} more steps are in the saved receipt.')
        if report.get('errors'):
            lines.extend(('', 'Skipped or failed:'))
            for error in report['errors'][:500]:
                name = error.get('file') or error.get('source') or error.get('kind') or 'Operation'
                lines.append(f'{error.get("status", "failed")}: {name} — {error.get("message", "Unknown error")}')
            if len(report['errors']) > 500:
                lines.append(f'... {len(report["errors"]) - 500} more entries are in the saved receipt.')
        receipt = Path(report['receipt'])
        lines.extend(('', f'Saved receipt: {receipt}'))
        dialog = QDialog(self); dialog.setWindowTitle('Preparation details'); dialog.resize(780, 500)
        layout = box(dialog, margins=(24, 24, 24, 24))
        details = QTextEdit(); details.setReadOnly(True); details.setPlainText('\n'.join(lines)); layout.addWidget(details)
        open_receipt = button('Open saved receipt', lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(str(receipt))), 'quiet')
        open_receipt.setEnabled(receipt.is_file())
        layout.addWidget(open_receipt, 0, Qt.AlignLeft)
        dialog.exec()

    def review_skipped(self):
        if self.busy or not self.identity or not self.device_connected:
            return
        identity = self.identity
        self.work('review_skipped', lambda: (identity, media.list_skipped_files(identity)), 'Loading skipped files…')

    def show_skipped_dialog(self, identity, rows):
        dialog = QDialog(self); dialog.setWindowTitle('Skipped files'); dialog.resize(680, 400)
        layout = box(dialog, margins=(24, 24, 24, 24)); layout.addWidget(label('Skipped during preparation', 'section'))
        table = QTableView(); model = MediaModel(table); model.reset([dict(title=row['relative'], kind=row['kind']) for row in rows], [('title', 'FILE'), ('kind', 'TYPE')]); table.setModel(model)
        table.setSelectionBehavior(QAbstractItemView.SelectRows); table.setSelectionMode(QAbstractItemView.ExtendedSelection); table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch); layout.addWidget(table)
        def restore():
            if not self.same_device(self.identity, identity):
                dialog.reject(); self.show_error('The selected iPod changed. Load skipped files again.'); return
            selected = [rows[i.row()] for i in table.selectionModel().selectedRows()]
            if selected:
                dialog.accept()
                def run():
                    media.restore_skipped_files(identity, [(r['kind'], r['relative']) for r in selected])
                    return dict(summary='Selected files will be included in the next preparation.')
                self.work('skipped', run, 'Including selected files again…')
        layout.addWidget(button('Include selected files again', restore, 'primary'), 0, Qt.AlignRight); dialog.exec()

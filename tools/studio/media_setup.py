"""Firmware setup, recovery, startup selection, and removal UI."""
import sys
from pathlib import Path

from PySide6.QtCore import QSignalBlocker, QTimer, Qt
from PySide6.QtWidgets import QDialog, QFileDialog

from tools.content import apple_font, media_manager as media, media_trash
from tools.content.media_manager import format_size as size
from tools.studio import boot_upgrade, installer, startup_default
from tools.studio.targets import TARGETS

from .media_widgets import ComboBox, box, button, label


class SetupWorkspace:
    """Firmware setup, recovery, startup selection, and removal UI."""

    def _complete_uninstall_trash(self, result):
        self.review = result['review']; self.render_setup()
        if not (self.review['uninstall']['available'] or self.review['uninstall'].get('blocker') == 'trash'):
            self.dialog('Cannot remove Linnet', self.review['uninstall']['detail'], 'Close', cancel=False)
            return
        if result['unrecognized']:
            if self.dialog('Trash needs review',
                    'Some Trash entries could not be read. Review them in the Trash folder before removing Linnet.',
                    'Open Trash folder'):
                self.open_folder(Path(self.identity['root']) / '.LINNET' / 'TRASH')
        else:
            self.confirm_uninstall(result['rows'])

    def _complete_setup(self, result):
        self.review = result; self.update_trash_badge(result.get('trash'))
        self.render_setup(); self.show_progress('Device setup is ready.')

    def _complete_uninstall(self, result):
        identity = result.get('reconnected_identity') or self.identity
        self.transition_device(identity, reset=True)
        self.last_preparation = None
        self.prep_results.clear(); self.prep_results.hide()
        self.update_trash_badge(dict(count=0, unreadable=0))
        self.cache_library_dirty = True
        # Removal is complete. Refresh only setup; verifying all remaining
        # media can be slow and must not obscure success or retain Trash rows.
        def inspect():
            try:
                review = installer.assess(identity)
            except (OSError, RuntimeError, ValueError) as error:
                raise RuntimeError('Linnet was removed, but setup could not be refreshed: ' + str(error)) from error
            return dict(review=review, summary=result['summary'])
        self.work('uninstall_setup', inspect, result['summary'])

    def _complete_uninstall_setup(self, result):
        self.identity = result['review']['identity']
        self.identity['linnet'] = result['review']['linnet']
        self.identity['default_boot'] = installer.default_boot(result['review'])
        self.update_device()
        self._complete_setup(result['review'])
        self.navigate('iPod setup')
        self.show_progress(result['summary'])

    def _complete_link_setup(self, result):
        self.transition_device(result['reconnected_identity'])
        self.device_name.setText(self.device_title(self.identity))
        self.device_info.setText('Connected in Disk Mode')
        self.cache_library_dirty = False
        if result.get('publication_recovery_pending'):
            self._complete_scan(result)
            self.navigate('Preparation')
            return
        if result.get('setup_error'):
            self.device_connected = False
            self.update_controls()
            self.cache_library_dirty = True
            self.show_error('Disk Mode connected, but setup could not be read: ' + result['setup_error'])
            return
        self.review = result['review']
        self.update_trash_badge(self.review.get('trash'))
        if result['section'] == 'build':
            self.build_editor_open = True
        self.navigate('iPod setup')
        self.cache_library_dirty = True
        section = result['section']
        target = (self.default_title if section == 'startup' else
                  self.uninstall_panel if section == 'remove' else self.boot_choice)
        def reveal():
            if self.closed or self.page != 'iPod setup' or self.review is not result['review']:
                return
            if section == 'build':
                self.boot_choice.setFocus()
            elif section == 'remove':
                self.uninstall_button.setFocus()
            elif self.default_choice.isVisible():
                self.default_choice.setFocus()
            self.setup.ensureWidgetVisible(target)
            if section == 'remove' and self.uninstall_button.isEnabled():
                self.uninstall_selected()
        # New sections change the scroll content height. Reveal only after
        # Qt has laid them out, so the previous hidden geometry is not used.
        QTimer.singleShot(0, reveal)
        self.show_progress('Choose the default system and use Menu at startup for a one-time choice.'
                           if section == 'startup' else
                           'Review removal options below.' if section == 'remove' else
                           'Choose a firmware ZIP, then apply the update. Your media is kept.')

    def update_setup_status(self):
        loading = self.busy and self.review is None
        self.discovery_issue.setText('A connected iPod needs attention: ' + '; '.join(self.discovery_errors))
        self.discovery_issue.setVisible(bool(self.discovery_errors))
        from tools.studio.device_support import describe
        support_identity = self.identity or (self.link_connection or {}).get('identity')
        if self.identity is None and self.link_connection:
            support_identity = dict(support_identity, **self.link_connection.get('product', {}))
        support = describe(support_identity, self.review)
        self.support_status.setText(self.hardware_report['summary'] if not support_identity and self.hardware_report else
                                   support['title'] + ' — ' + support['detail'])
        report = self.hardware_report or {}
        hardware_context = bool((self.identity and self.device_connected) or self.link_connection or
                                self.discovery_errors or any(report.get(key) for key in
                                ('usb', 'disks', 'recognized', 'runtime', 'errors', 'discovery_issue_count')))
        self.hardware_report_button.setVisible(hardware_context)
        self.hardware_report_button.setEnabled(not self.busy)
        self.setup_progress.setVisible(loading)
        setting_up = bool(self.busy and self.active_operation and self.active_operation.tag=='classic_install')
        link = self.link_connection
        linked_os = bool(link and link['identity']['image_kind'] == 'os' and self.review is None)
        self.connection_help.setVisible(self.identity is None and link is None and not setting_up)
        self.setup_find_button.setVisible(self.identity is None and link is None and not setting_up)
        self.link_disk_button.setVisible(bool(link and link.get('can_open_disk')))
        self.link_disk_button.setEnabled(bool(link and link.get('can_open_disk')) and not self.busy)
        self.setup_find_button.setEnabled(not self.busy)
        if not self.busy:
            from tools.studio.classic_setup import pending
            scope = dict(target_id=link['identity']['target_id'],linnet_usb_serial=link['serial']) if link else self.identity
            resumable = self.review is None and bool(pending(scope))
            self.resume_classic_button.setVisible(resumable)
            if link and resumable:self.link_disk_button.hide()
        show_link_actions = linked_os and self.resume_classic_button.isHidden() and not setting_up
        self.link_setup_panel.setVisible(show_link_actions)
        for action in (self.link_startup_button, self.link_build_button, self.link_uninstall_button):
            action.setEnabled(bool(show_link_actions and link.get('can_open_disk')) and not self.busy)
        self.link_uninstall_detail.setText(
            'Opens Disk Mode automatically, then asks you to confirm removal. Your media folders are kept.'
            if link and link.get('can_open_disk') else
            'Enter Apple Disk Mode, then choose Find iPod to review removal: hold Menu + Select until restart, '
            'then immediately hold Select + Play/Pause until Disk Mode appears.')
        self.resume_classic_button.setEnabled(not self.busy)
        if self.review is not None:
            return
        if link:
            self.setup_device.setText(link['target_name'].replace('Linnet for ', '') + ' · …' + link['serial'][-6:])
            build = installer.BUILD_LABELS[link['identity']['profile']]
            self.setup_software.setText('Connected through Linnet Link' if linked_os else 'Recovery connected')
            self.link_disk_button.setText('Open Disk Mode')
            self.link_build_status.setText(build + ' running')
            self.install_detail.setText('Setup is continuing automatically. Keep USB connected.' if setting_up else
                'Choose Resume Classic installation to continue the saved setup.' if self.resume_classic_button.isVisible() else
                'Choose a setup action below, or open Disk Mode to manage media.' if linked_os and link.get('can_open_disk') else
                'Your iPod is connected. Open Disk Mode to manage files.' if link.get('can_open_disk') else
                'Linnet Link is connected. This iPod’s storage is not available yet.')
            self.install_detail.setVisible(setting_up or not self.resume_classic_button.isHidden() or not link.get('can_open_disk'))
            return
        if self.identity:
            self.setup_device.setText(self.device_title(self.identity))
            self.setup_software.setText('Checking iPod setup…' if loading else 'Choose Refresh to check installed software.')
        else:
            self.setup_device.setText('Looking for your iPod…' if loading else 'Connect your iPod with USB.')
            self.setup_software.setText('Device detection is in progress.' if loading else '')

    def review_uninstall_trash(self):
        if self.busy or not self.identity or not self.device_connected or not self.review or self.review['uninstall'].get('blocker') != 'trash':
            return
        identity = self.identity
        def inspect_trash():
            from tools.content.guard import guarded_path
            current = media.refresh(identity)
            rows = media_trash.removals(current)
            trash = Path(current['root']) / '.LINNET' / 'TRASH'
            guarded_path(trash, Path(current['root']))
            known = {row['id'] for row in rows}
            unrecognized = sum(entry.name not in known for entry in trash.iterdir()) if trash.is_dir() else 0
            return dict(rows=rows, unrecognized=unrecognized, review=installer.assess(current))
        self.work('uninstall_trash', inspect_trash, 'Reading Trash before removal…')

    def render_setup(self):
        review = self.review
        identity = review['identity']
        self.setup_device.setText((identity.get('volume_label') or 'iPod') + ' · ' +
                                  identity['target_name'].replace('Linnet for ', ''))
        self.setup_software.setText('Connected by USB · ' + identity['drive'] + ' · …' + identity['disk_serial'][-6:])
        self.setup_software.setToolTip(installer.build_description(review['linnet']))
        native = review['identity'].get('native_target_id') == review['identity']['target_id']
        available_boot = [option for option in review['boot_options'] if option['kind'] == 'linnet']
        with QSignalBlocker(self.boot_choice):
            self.boot_choice.clear()
            for option in available_boot:
                suffix = (' · Installed' if option.get('active') else
                          ' · Hold Menu' if option.get('chooser_available') else
                          ' · Unavailable' if not option.get('selectable') else '')
                self.boot_choice.addItem(('Developer firmware' if option.get('build') == 'dev' else 'User firmware') + suffix, option)
                self.boot_choice.setItemData(self.boot_choice.count() - 1, option.get('detail', ''), Qt.ToolTipRole)
            selected = next((i for i, option in enumerate(available_boot) if option.get('active')),
                            next((i for i, option in enumerate(available_boot) if option.get('selectable')), -1))
            self.boot_choice.setPlaceholderText('Installation is unavailable')
            self.boot_choice.setCurrentIndex(selected)
        with QSignalBlocker(self.method_choice):
            self.method_choice.clear()
            for method in review['installation_methods']:
                if not method.get('enabled'):
                    continue
                self.method_choice.addItem(method['label'], method)
                self.method_choice.setItemData(self.method_choice.count() - 1, method['detail'], Qt.ToolTipRole)
                if method['id'] == review.get('default_installation_method'):
                    self.method_choice.setCurrentIndex(self.method_choice.count() - 1)
        summary = review['summary']
        if (review['action'] == 'unsupported' and review['target']['boot_layout'] == 's5l-nor' and
                review['linnet']['state'] == 'absent'):
            if (review.get('recovery') or {}).get('strategy') == 'native-nor' and review['recovery'].get('phase') == 'installed':
                summary = ('Linnet Boot is already installed. Start Linnet Boot, enter Linnet Disk Mode, '
                           'then reconnect and refresh to install Linnet OS. Another Boot installation is not needed.')
        elif review['action'] == 'install' and review['linnet']['state'] == 'absent' and review['strategy']!='classic-bootstrap':
            summary = 'Ready to install Linnet. Choose a firmware ZIP below, then install and eject safely.'
        self.install_detail.setText(summary)
        self.install_detail.setVisible(review['action'] in ('unsupported', 'attention', 'resume', 'adopt', 'installed') or
            review['linnet']['state'] not in ('installed', 'absent'))
        self.uninstall_detail.setText('Remove Linnet’s software and settings. Your media folders are kept.'
            if review['uninstall'].get('blocker') == 'trash' else review['uninstall']['detail'] +
            (' Your media folders are kept.' if review['uninstall']['available'] else ''))
        try:
            startup = startup_default.inspect(review)
        except OSError:
            # The volume can disappear between a completed scan and opening
            # this page, including during Classic's automatic mode changes.
            self.transition_device(None)
            self.update_controls()
            self.show_progress('The iPod disconnected. Reconnect it and choose Find iPod.')
            return
        self.startup_state = startup
        labels = {'linnet': 'Linnet', 'rockbox': 'Rockbox', 'apple': 'Apple iPod software'}
        self.default_detail.setText('Hold Menu during startup to choose a system for that start.')
        with QSignalBlocker(self.default_choice):
            self.default_choice.clear()
            for choice in startup['choices'] if startup['supported'] else []:
                self.default_choice.addItem(labels[choice], choice)
            self.default_choice.setCurrentIndex(self.default_choice.findData(startup['default']))
        self.boot_upgrade_available = native and boot_upgrade.available(review)
        self.boot_upgrade_pending = bool(review['recovery'] and review['recovery'].get('phase') == 'boot-upgrade-pending')
        self.update_install_action()

    def update_install_action(self):
        resume = bool(self.review and self.review.get('action') == 'resume')
        from_source = self.build_from_source.isChecked() and not getattr(sys, 'frozen', False) and not resume
        selection = self.firmware_status() if not from_source and not resume else None
        firmware = selection['firmware'] if selection else None
        saved = ((self.review or {}).get('resume') or (self.review or {}).get('classic_setup') or {})
        desired = saved.get('build') if resume else firmware['build'] if firmware else None
        if desired:
            with QSignalBlocker(self.boot_choice):
                for index in range(self.boot_choice.count()):
                    if (self.boot_choice.itemData(index) or {}).get('build') == desired:
                        self.boot_choice.setCurrentIndex(index)
                        break
        option = self.boot_choice.currentData(); method = self.method_choice.currentData()
        review = self.review
        can_install = bool(review and any(item.get('selectable') and item['kind'] == 'linnet'
                                         for item in review['boot_options']) and
                           any(item.get('enabled') for item in review['installation_methods']))
        installed = bool(review and review['linnet']['state'] == 'installed')
        upgrade = bool(review and getattr(self, 'boot_upgrade_available', False))
        pending = bool(review and getattr(self, 'boot_upgrade_pending', False))
        self.install_panel.setVisible(installed or can_install or upgrade or pending)
        self.install_title.setText('Installed firmware' if installed else 'Finish installation' if resume else 'Install Linnet')
        self.installed_build.setVisible(installed)
        profile = (review or {}).get('linnet', {}).get('profile')
        labels = {'release': 'User firmware', 'dev': 'Developer firmware', 'trace': 'Developer trace firmware'}
        self.installed_build.setText(labels.get(profile, 'Linnet firmware') if installed else '')
        self.installed_build.setToolTip(installer.build_description(review['linnet']) if installed else '')
        candidate = bool(installed and review['linnet'].get('candidate_pending'))
        next_build = labels.get((review or {}).get('linnet', {}).get('next_profile'), 'Firmware')
        self.build_summary.setText(next_build + ' ready to start. Eject safely to finish the update.' if candidate else
                                  'Includes diagnostics and Linnet Link.' if installed and profile in ('dev', 'trace') else
                                  'For everyday listening.' if installed else 'Your media stays on the iPod.')
        forced_editor = not installed or bool(review and review['action'] == 'resume') or pending
        show_editor = bool(can_install or upgrade or pending) and (self.build_editor_open or forced_editor)
        self.build_editor.setVisible(show_editor)
        self.build_summary.setVisible(not installed or candidate or not show_editor)
        self.build_editor_button.setVisible(installed and (can_install or upgrade) and not forced_editor)
        self.build_editor_button.setEnabled(not self.busy)
        self.build_editor_button.setText('Hide options' if self.build_editor_open else 'Change firmware…')
        self.uninstall_panel.setVisible(bool(review and (review['linnet']['state'] != 'absent' or
                                                        review['recovery'] or review['uninstall']['available'])))
        if review is None and not self.link_connection and not (self.busy and self.active_operation and self.active_operation.tag=='classic_install'):
            self.install_detail.hide()
        enabled = not self.busy and self.identity is not None and self.review is not None and bool(option and option.get('selectable'))
        linnet = option and option['kind'] == 'linnet'
        self.build_from_source.setEnabled(not self.busy and not resume and not getattr(sys, 'frozen', False))
        self.build_title.setVisible(from_source)
        self.boot_choice.setVisible(from_source)
        self.source_build_detail.setVisible(from_source)
        self.source_build_detail.setText('Includes diagnostics and Linnet Link.' if option and option.get('build') == 'dev' else
                                         'For everyday use.')
        self.source_options_button.setVisible(not resume)
        self.source_options_button.setEnabled(not self.busy and not resume)
        self.source_options.setVisible(not resume and self.source_options_button.isChecked())
        self.firmware_button.setEnabled(not self.busy and not resume)
        self.firmware_button.setVisible(not resume)
        self.firmware_label.setVisible(bool(self.firmware_path) and not from_source and not resume)
        self.firmware_label.set_message(Path(self.firmware_path).name if self.firmware_path else '', self.firmware_path)
        if resume:
            title = 'Saved ' + ('Developer' if desired == 'dev' else 'User') + ' firmware'
            detail = 'Your saved installation files will be used. You do not need to select a ZIP.'
            state, action = 'saved', 'Choose ZIP…'
        elif from_source:
            title = 'Compile ' + ('Developer' if option and option.get('build') == 'dev' else 'User') + ' firmware'
            detail = 'Builds this source checkout on your computer before installing it on the iPod.'
            state, action = 'source', 'Use a ZIP…'
        else:
            title, detail, state, action = (selection[key] for key in ('title', 'detail', 'state', 'action'))
        self.firmware_summary.setText(title)
        self.boot_detail.setText(detail)
        self.firmware_button.setText(action)
        if self.firmware_picker.property('state') != state:
            self.firmware_picker.setProperty('state', state)
            for widget in (self.firmware_picker, self.firmware_summary):
                widget.style().unpolish(widget); widget.style().polish(widget); widget.update()
        show_methods = bool(linnet and option.get('selectable') and method)
        for widget in (self.method_title, self.method_choice):
            widget.setVisible(show_methods and self.method_choice.count() > 1)
        self.method_detail.setVisible(show_methods and self.method_choice.count() > 1)
        self.method_detail.setText(method.get('detail', '') if method and linnet else '')
        enabled = enabled and bool(method and method.get('enabled')) if linnet else enabled and not bool(option and option.get('active'))
        if linnet and not resume and not from_source:
            enabled = enabled and bool(firmware)
        self.method_choice.setEnabled(bool(linnet and method) and not self.busy)
        self.boot_choice.setEnabled(from_source and not self.busy and self.review is not None and self.boot_choice.count() > 0)
        self.install_button.setEnabled(enabled)
        self.install_hint.setText('Keep USB connected while installation resumes.' if resume else
            'Media and settings are kept. Eject safely when finished.' if installed else
            'Keep USB connected until installation finishes.')
        self.install_hint.setVisible(bool(firmware or from_source or resume))
        self.uninstall_button.setEnabled(not self.busy and self.device_connected and bool(
            self.identity and self.review and (self.review['uninstall']['available'] or
                                              self.review['uninstall'].get('blocker') == 'trash')))
        show_default = bool(self.review and self.review['linnet']['state'] == 'installed')
        self.startup_panel.setVisible(show_default)
        for widget in (self.default_title, self.default_detail, self.default_choice, self.default_button,
                       self.boot_upgrade_button, self.boot_upgrade_detail):
            widget.setVisible(show_default)
        startup = getattr(self, 'startup_state', dict(supported=False, choices=[], default='linnet'))
        self.default_row.setVisible(show_default and startup['supported'])
        self.default_detail.setVisible(show_default and startup['supported'])
        self.default_choice.setVisible(show_default and startup['supported'])
        self.default_button.setVisible(show_default and startup['supported'])
        self.default_choice.setEnabled(not self.busy and startup['supported'])
        self.default_button.setEnabled(not self.busy and startup['supported'] and
                                       self.default_choice.currentData() != startup['default'])
        upgrade = getattr(self, 'boot_upgrade_available', False)
        pending = getattr(self, 'boot_upgrade_pending', False)
        self.boot_upgrade_button.setVisible(show_default and (upgrade or pending))
        self.boot_upgrade_button.setText('Resume startup selector update' if pending else 'Update startup selector')
        self.boot_upgrade_button.setEnabled(not self.busy and (pending or upgrade))
        self.boot_upgrade_detail.setVisible(show_default and not startup['supported'])
        self.boot_upgrade_detail.setText('Finish the interrupted startup update before changing the default.' if pending else
            'Update Boot to enable the Menu startup selector and default choices.' if upgrade else
            'This Boot version cannot save a default startup choice from Media Manager.')
        if resume:
            self.install_button.setText('Resume installation')
        elif linnet and not from_source and not firmware:
            self.install_button.setText('Choose firmware first')
        elif option is None:
            self.install_button.setText('Install firmware')
        elif not option.get('selectable'):
            self.install_button.setText('Installation unavailable')
        else:
            self.install_button.setText(('Build and update' if installed else 'Build and install') if from_source else
                                        ('Update firmware' if installed else 'Install firmware') if linnet else 'Set as default boot')

    def toggle_source_options(self, expanded):
        self.source_options.setVisible(expanded)
        self.source_options_button.setArrowType(Qt.DownArrow if expanded else Qt.RightArrow)

    def toggle_setup_editor(self):
        if self.busy:
            return
        self.build_editor_open = not self.build_editor_open
        self.update_install_action()
        if self.build_editor_open:
            self.reveal_setup_editor(self.boot_choice if self.build_from_source.isChecked() else self.firmware_button)

    def reveal_setup_editor(self, widget):
        review = self.review
        def reveal():
            if not self.closed and self.page == 'iPod setup' and self.review is review and self.build_editor.isVisible():
                self.setup.ensureWidgetVisible(widget)
        QTimer.singleShot(0, reveal)

    def install_selected(self):
        self.update_install_action()
        if not self.device_connected or not self.install_button.isEnabled():
            return
        option, method, identity = self.boot_choice.currentData(), self.method_choice.currentData(), self.identity
        selected_zip = getattr(self, '_firmware_preview', None)
        resume = self.review.get('action') == 'resume'
        from_source = self.build_from_source.isChecked() and not getattr(sys, 'frozen', False) and not resume
        build_label = 'Developer firmware' if option.get('build') == 'dev' else 'User firmware'
        description = ((f'Resume installing the saved {build_label.lower()}?' if resume else
                        f"Install {build_label.lower()} on {self.device_title(identity)}?") +
                       ('\n\nFirmware will be compiled from this source checkout.' if from_source else
                        '\n\nFirmware ZIP: ' + Path(self.firmware_path).name if not resume else '') +
                       ('\nStartup option: ' + method['label'] if self.method_choice.count() > 1 else '')
                       if option['kind'] == 'linnet' else
                       f"Make {option['label']} the default startup software?")
        if option['kind'] == 'linnet' and self.review.get('action') in ('replace', 'adopt'):
            description += ('\n\nYour settings, selected theme, playback history, and media libraries '
                            'will be preserved. ' + ('The confirmed OS will be retained for rollback.'
                            if method['strategy'] == 'native-volume' else
                            'Superseded Linnet system files will be archived.'))
        if not self.dialog('Resume installation' if resume else 'Install firmware',
                           description + '\n\nKeep the iPod connected until installation finishes.',
                           'Resume' if resume else 'Install'):
            return
        if not from_source and not resume:
            self.update_install_action()
            if not self.install_button.isEnabled():
                self.reveal_setup_editor(self.firmware_picker)
                return
            if getattr(self, '_firmware_preview', None) != selected_zip:
                self.show_progress('The firmware file changed. Review the selection and install again.')
                return
        firmware = None if from_source or not self.firmware_path else Path(self.firmware_path)
        if option['kind'] == 'linnet' and self.review.get('action') == 'resume':
            from_source, firmware = False, None
        operation = (lambda: installer.install(identity, progress=self.events.progress.emit,
            build=option['build'], installation_method=method['id'], firmware=firmware,
            from_source=from_source,instruction=self.events.setup_instruction.emit,
            cancelled=self.stop_event.is_set)) if option['kind'] == 'linnet' else lambda: installer.restore(identity, expected=option['id'])
        self.work('classic_install' if method and method['strategy']=='classic-bootstrap' else 'install',
                  operation, 'Updating your iPod setup…')

    def close_setup_instruction(self):
        if self.setup_instruction_dialog is not None:
            self.setup_instruction_dialog.done(QDialog.Accepted)
            self.setup_instruction_dialog.deleteLater()
            self.setup_instruction_dialog = None

    def resume_classic_setup(self):
        if self.busy:return
        from tools.studio.classic_setup import pending, run
        scope = dict(target_id=self.link_connection['identity']['target_id'],
                     linnet_usb_serial=self.link_connection['serial']) if self.link_connection else self.identity
        choices=pending(scope)
        if not choices:return
        selected=choices[0]
        if len(choices)>1:
            dialog=QDialog(self); dialog.setWindowTitle('Resume Classic installation')
            layout=box(dialog,margins=(24,24,24,24))
            layout.addWidget(label('Choose the Classic installation to resume.',None,True))
            picker=ComboBox()
            for item in choices:picker.addItem('Classic · '+item['serial'][-6:]+' · '+item['phase'],item)
            layout.addWidget(picker); layout.addWidget(button('Resume installation',dialog.accept,'primary'))
            if dialog.exec()!=QDialog.Accepted:return
            selected=picker.currentData()
        self.work('classic_install',lambda:run(selected['path'],progress=self.events.progress.emit,
            instruction=self.events.setup_instruction.emit,cancelled=self.stop_event.is_set),
            'Resuming Classic installation…')

    def open_link_disk(self):
        if self.busy or not self.link_connection:return
        from tools.studio.media_link import open_disk
        connection = self.link_connection
        self.work('link_disk',lambda:open_disk(connection,progress=self.events.progress.emit),
                  'Opening Disk Mode automatically…')

    def open_link_setup(self, section):
        if self.busy or not self.link_connection or not self.link_connection.get('can_open_disk'):
            return
        from tools.studio.media_link import open_setup
        connection = self.link_connection
        self.work('link_setup', lambda: open_setup(connection, section=section, progress=self.events.progress.emit),
                  'Connecting your iPod’s setup settings…')

    def save_default_startup(self):
        if not self.default_button.isEnabled():
            return
        identity, choice = self.identity, self.default_choice.currentData()
        self.work('startup_default', lambda: startup_default.set_default(identity, choice),
                  'Saving default startup…')

    def upgrade_startup_selector(self):
        if not self.boot_upgrade_button.isEnabled():
            return
        identity = self.identity
        pending = self.boot_upgrade_pending
        option = self.boot_choice.currentData()
        build = (option or {}).get('build', self.review['linnet'].get('build', 'dev'))
        from_source = self.build_from_source.isChecked() and not getattr(sys, 'frozen', False)
        firmware = None if from_source or not self.firmware_path else Path(self.firmware_path)
        selection, _error = self.selected_firmware()
        if not from_source and selection:
            build = selection['build']
        if not pending and not (from_source or firmware and selection):
            self.build_editor_open = True
            self.update_install_action()
            self.reveal_setup_editor(self.firmware_picker)
            self.show_progress('Choose a firmware ZIP below, then try Update startup selector again.')
            return
        if not pending and not self.dialog('Update startup selector',
                f'Update startup firmware on {self.device_title(identity)}? Linnet and the saved Apple software will remain available.\n\nKeep the iPod connected until the update finishes.',
                'Update startup selector'):
            return
        self.work('boot_upgrade', lambda: boot_upgrade.upgrade(identity, build=build,
                  firmware=firmware, from_source=from_source, progress=self.events.progress.emit),
                  'Updating startup selector…')

    def uninstall_selected(self):
        if not self.uninstall_button.isEnabled():
            return
        if self.review['uninstall'].get('blocker') == 'trash':
            self.review_uninstall_trash()
        else:
            self.confirm_uninstall()

    def confirm_uninstall(self, trash_rows=()):
        identity = self.identity
        previous = self.review['uninstall']['previous']
        startup = 'rockbox' if previous == 'Rockbox' else 'apple'
        message = (f"Restore {previous} as the default startup software and remove Linnet OS from "
                   f"{self.device_title(identity)}?\n\n"
                   "Linnet settings, prepared libraries, and system files in .LINNET will be deleted. "
                   "Your Music, Photos, Videos, and Notes folders will remain. "
                   "Apple OS may need a library sync to show media that was prepared only for Linnet.\n\n"
                   "Keep the iPod connected until removal finishes, then eject it safely.")
        if trash_rows:
            message = ('Trash isn’t empty. Continuing will permanently empty Trash '
                       f'({size(sum(row["bytes"] for row in trash_rows))}). These files cannot be restored afterward.\n\n' + message)
        if not self.dialog('Remove Linnet OS?', message,
                           'Empty Trash and remove Linnet' if trash_rows else 'Restore and remove Linnet',
                           danger=True, secondary=('Review Trash', lambda: self.navigate('Trash')) if trash_rows else None):
            return
        def remove():
            for row in trash_rows:
                media_trash.recover(identity, row['id'], erase=True)
            return installer.uninstall(identity, progress=self.events.progress.emit,
                                       expected=startup, instruction=self.events.setup_instruction.emit)
        self.work('uninstall', remove, 'Removing Linnet OS…')

    def install_apple_font(self):
        if not self.apple_font_install.isEnabled():
            return
        identity = dict(self.identity)
        self.work('apple_font_install', lambda: apple_font.install(identity,
            progress=self.events.progress.emit, cancelled=self.stop_event.is_set), 'Preparing Apple Podium Sans…')

    def remove_apple_font(self):
        if not self.apple_font_remove.isEnabled():
            return
        identity = dict(self.identity)
        self.work('apple_font_remove', lambda: apple_font.remove(identity), 'Selecting the included theme font…')

    def _complete_apple_font(self, result):
        self.apple_font_result = result['summary']
        self.apple_font_status.setText(result['summary'])
        self.show_progress(result['summary'])

    def choose_firmware(self):
        initial = Path(self.firmware_path) if self.firmware_path else Path.home() / 'Downloads'
        if not initial.exists():
            initial = initial.parent if initial.parent.is_dir() else Path.home()
        path, _ = QFileDialog.getOpenFileName(self, 'Choose firmware ZIP', str(initial),
                                            'Linnet firmware ZIP (*.zip)')
        if path:
            self.firmware_path = path
            self.settings.setValue('firmware_bundle', path)
            self.build_from_source.setChecked(False)
            self.update_install_action()

    def selected_firmware(self):
        selection = self.firmware_status()
        return selection['firmware'], '' if selection['firmware'] else selection['detail']

    def firmware_status(self):
        def notice(title, detail, state='warning', action='Choose ZIP…', firmware=None):
            return dict(title=title, detail=detail, state=state, action=action, firmware=firmware)
        if not self.firmware_path:
            return notice('No firmware selected',
                'Choose a User or Developer firmware ZIP for this iPod. Keep the file zipped.', 'empty')
        try:
            from tools.content.host_state import file_stamp
            from tools.studio.firmware_bundle import describe
            path = Path(self.firmware_path)
            if path.is_dir():
                return notice('Select the ZIP file', 'Choose the downloaded firmware ZIP, rather than a folder or extracted files.')
            stamp = (str(path), file_stamp(path.stat()))
            cached = getattr(self, '_firmware_preview', None)
            if cached is None or cached[0] != stamp:
                info = describe(path)
                if file_stamp(path.stat()) != stamp[1]:
                    return notice('Firmware file changed', 'The ZIP changed while it was being checked. Select it again to continue.')
                self._firmware_preview = (stamp, info)
            info = self._firmware_preview[1]
            if self.identity and info['target_id'] != self.identity['target_id']:
                return notice('Firmware for a different iPod',
                    'This ZIP is for ' + TARGETS[info['target_id']]['name'].replace('Linnet for ', '') +
                    '. Choose firmware for the connected ' + TARGETS[self.identity['target_id']]['name'].replace('Linnet for ', '') + '.',
                    action='Change ZIP…')
            build_label = 'Developer firmware' if info['build'] == 'dev' else 'User firmware'
            return notice(TARGETS[info['target_id']]['name'].replace('Linnet for ', '') + ' · ' + build_label,
                'Version ' + info['version'] + ' · ' + ('Diagnostics and Linnet Link.' if info['build'] == 'dev' else 'For everyday use.'),
                'ready', 'Change ZIP…', info)
        except FileNotFoundError:
            return notice('Selected ZIP not found',
                'The file is no longer at its saved location. Locate it or choose a new download.', action='Locate ZIP…')
        except PermissionError:
            return notice('Firmware file cannot be opened', 'Choose a copy of the ZIP that you can open on this computer.')
        except OSError:
            return notice('Firmware file unavailable',
                'The drive or folder containing this ZIP is unavailable. Reconnect it or choose another copy.')
        except (ValueError, KeyError, TypeError) as error:
            detail = str(error).lower()
            if 'current linnet' in detail:
                return notice('Choose a current firmware ZIP',
                    'This ZIP does not contain the firmware details Linnet needs. Download a current User or Developer firmware ZIP.')
            if 'checksum' in detail or 'invalid firmware bundle' in detail:
                return notice('Firmware ZIP is damaged', 'Download the firmware again, then select the new copy.')
            return notice('Firmware ZIP cannot be used',
                'Choose an original Linnet firmware download. If this is the correct ZIP, try downloading it again.')

    def firmware_source_changed(self, enabled):
        self.settings.setValue('build_from_source', enabled)
        self.update_install_action()

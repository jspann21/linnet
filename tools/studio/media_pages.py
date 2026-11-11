"""Construction of the Media Manager workspace pages."""
import sys

from PySide6.QtCore import QTimer, Qt
from PySide6.QtGui import QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QAbstractItemView,
    QFrame,
    QLineEdit,
    QListView,
    QListWidget,
    QProgressBar,
    QStackedWidget,
    QTableView,
    QTextEdit,
    QToolButton,
    QWidget,
)

from tools.content import games
from tools.studio import screen_settings

from .media_models import CardDelegate, LibraryTable, MediaModel, TrackDelegate
from .media_widgets import CheckBox, ComboBox, ControlRow, StatusLabel, action_menu, box, button, icon, label


def build_library_page(window):
    page = QWidget(); outer = box(page, margins=(30, 28, 26, 20), spacing=12)
    heading = QWidget(); titles = box(heading, spacing=5)
    window.heading_label = StatusLabel('Music'); window.heading_label.setObjectName('title')
    window.subtitle = StatusLabel(''); window.subtitle.setObjectName('subtitle')
    titles.addWidget(window.heading_label); titles.addWidget(window.subtitle)
    tools = QWidget(); top = box(tools, horizontal=True, spacing=8)
    window.library_options, library_actions = action_menu('Library actions', (
        ('Playlists…', window.open_playlists),
        ('Open iPod folder', window.open_media_folder),
        ('Sync video folder', window.sync_videos),
    ))
    window.playlists_button, window.folder_button, window.sync_button = library_actions
    top.addWidget(window.library_options)
    window.add_button = button('Add media', window.add_media, 'primary', 'plus'); top.addWidget(window.add_button)
    outer.addWidget(ControlRow(heading, tools, breakpoint=None))
    toolbar = box(horizontal=True)
    window.segments = QFrame(); window.segments.setObjectName('segments'); segments = box(window.segments, horizontal=True, margins=(4, 4, 4, 4), spacing=2)
    window.view_buttons = {}
    for name in ('Songs', 'Albums', 'Artists', 'Genres'):
        item = button(name, lambda _=False, n=name: window.change_view(n), 'segment'); item.setCheckable(True)
        window.view_buttons[name] = item; segments.addWidget(item)
    toolbar.addWidget(window.segments)
    window.back_button = button('Back', window.back_to_groups, 'quiet', 'back'); toolbar.addWidget(window.back_button); toolbar.addStretch()
    outer.addLayout(toolbar)
    toolbar = box(horizontal=True)
    window.search = QLineEdit(); window.search.setPlaceholderText('Search your collection'); window.search.setClearButtonEnabled(True); window.search.setMinimumWidth(180)
    window.search.addAction(icon('search'), QLineEdit.LeadingPosition)
    window.search.setAccessibleName('Search collection')
    window.search.setToolTip('Search this collection (Ctrl+F). All search words must match.')
    window.search_timer = QTimer(window); window.search_timer.setSingleShot(True); window.search_timer.setInterval(160); window.search_timer.timeout.connect(window.render_library)
    window.search.textChanged.connect(lambda: window.search_timer.start()); toolbar.addWidget(window.search)
    window.filters_button = button('Filters', window.render_library, 'quiet', 'settings')
    window.filters_button.setCheckable(True)
    window.filters_button.setAccessibleName('Show or hide music filters')
    toolbar.addWidget(window.filters_button)
    window.clear_button = button('Clear', window.clear_filters, 'quiet')
    window.clear_button.setToolTip('Clear search and filters'); toolbar.addWidget(window.clear_button)
    outer.addLayout(toolbar)
    window.filters_panel = QWidget(); filters = box(window.filters_panel, horizontal=True)
    window.genre_filter = ComboBox(); window.genre_filter.addItem('All genres', ''); window.genre_filter.currentIndexChanged.connect(window.render_library)
    window.artist_filter = ComboBox(); window.artist_filter.addItem('All artists', ''); window.artist_filter.currentIndexChanged.connect(window.render_library)
    window.genre_filter.setAccessibleName('Filter by genre')
    window.artist_filter.setAccessibleName('Filter by artist')
    filters.addWidget(window.genre_filter, 1); filters.addWidget(window.artist_filter, 1)
    outer.addWidget(window.filters_panel)
    window.notice = label('', 'notice', True); window.notice.hide(); outer.addWidget(window.notice)
    window.model = MediaModel(window)
    window.table = LibraryTable(); window.table.setModel(window.model); window.table.setShowGrid(False); window.table.setAlternatingRowColors(False)
    window.table.setSelectionBehavior(QAbstractItemView.SelectRows); window.table.setSelectionMode(QAbstractItemView.ExtendedSelection)
    window.table.setEditTriggers(QAbstractItemView.NoEditTriggers); window.table.setSortingEnabled(True)
    window.table.sortByColumn(0, Qt.AscendingOrder)
    window.table.setWordWrap(False)
    window.table.setVerticalScrollMode(QAbstractItemView.ScrollPerPixel); window.table.verticalHeader().hide(); window.table.verticalHeader().setDefaultSectionSize(57)
    window.table.horizontalHeader().setDefaultAlignment(Qt.AlignLeft | Qt.AlignVCenter)
    window.table.setItemDelegate(TrackDelegate(window.covers, window.table)); window.table.setMouseTracking(True)
    window.grid = QListView(); window.grid.setModel(window.model); window.grid.setViewMode(QListView.IconMode)
    window.grid.setResizeMode(QListView.Adjust); window.grid.setMovement(QListView.Static); window.grid.setUniformItemSizes(True)
    window.grid.setLayoutMode(QListView.Batched); window.grid.setBatchSize(50); window.grid.setSpacing(4)
    window.grid.setSelectionMode(QAbstractItemView.ExtendedSelection); window.grid.setVerticalScrollMode(QAbstractItemView.ScrollPerPixel)
    window.grid.setItemDelegate(CardDelegate(window.covers, window.grid)); window.grid.setMouseTracking(True)
    window.covers.views = [window.table, window.grid]
    window.views = QStackedWidget(); window.views.addWidget(window.table); window.views.addWidget(window.grid)
    window.empty = QWidget(); empty = box(window.empty, margins=(30, 60, 30, 60)); empty.addStretch()
    window.empty_title = label('Your collection starts here', 'section'); window.empty_title.setAlignment(Qt.AlignCenter)
    window.empty_detail = label('Connect an iPod in Disk Mode to browse its media.', 'muted', True); window.empty_detail.setAlignment(Qt.AlignCenter)
    empty.addWidget(window.empty_title); empty.addWidget(window.empty_detail)
    window.empty_action = button('Find iPod', window.activate_empty, 'primary')
    empty.addWidget(window.empty_action, 0, Qt.AlignHCenter)
    empty.addStretch(); window.views.addWidget(window.empty)
    outer.addWidget(window.views, 1)
    window.table.selectionModel().selectionChanged.connect(window.selection_changed)
    window.grid.selectionModel().selectionChanged.connect(window.selection_changed)
    window.grid.activated.connect(window.open_group)
    window.table.activated.connect(window.inspect_selection)
    window.selection_bar = QFrame(); window.selection_bar.setObjectName('selectionBar')
    selection = box(window.selection_bar, margins=(16, 9, 12, 9))
    window.selection_text = StatusLabel(''); window.selection_text.setMinimumWidth(140)
    selected_tools = QWidget(); selected_actions = box(selected_tools, horizontal=True, spacing=8)
    window.selection_options, item_actions = action_menu('More', (
        ('Details', window.inspect_selection),
        ('Prepare as PCM', window.queue_selected_pcm),
        (None, None),
        ('Clear selection', window.clear_selection),
    ))
    window.info_button, window.prepare_pcm_button, window.clear_selection_action = item_actions
    selected_actions.addWidget(window.selection_options)
    window.restore_button = button('Restore', window.restore_selected, 'primary', 'back')
    selected_actions.addWidget(window.restore_button)
    window.remove_button = button('Move to Trash', window.remove_selected, 'danger', 'trash')
    selected_actions.addWidget(window.remove_button)
    selection.addWidget(ControlRow(window.selection_text, selected_tools, breakpoint=None))
    outer.addWidget(window.selection_bar); window.selection_bar.hide()
    window.hint = label('Double-click an item for details. Ctrl+click or Shift+click to select multiple files.', 'muted', True); outer.addWidget(window.hint)
    return page

def build_preparation_page(window):
    page = QWidget(); page.setObjectName('workspace')
    outer = box(page, spacing=0)
    scroll, layout = window.scroll_page()
    scroll.setObjectName('preparationScroll')
    window.preparation_scroll = scroll
    outer.addWidget(scroll, 1)
    window.heading(layout, 'Prepare your media', 'Turn your files into an iPod-ready collection. Your originals stay on the device.')
    window.prep_connection = QFrame(); window.prep_connection.setObjectName('recoveryNote')
    connection = box(window.prep_connection, margins=(20, 18, 20, 18), spacing=9)
    window.prep_connection_title = label('Disk Mode needed to prepare media', 'cardTitle', True)
    window.prep_connection_detail = label('', 'muted', True)
    connection.addWidget(window.prep_connection_title); connection.addWidget(window.prep_connection_detail)
    window.prep_connection_button = button('Open Disk Mode', window.connect_preparation, 'primary')
    connection.addWidget(window.prep_connection_button, 0, Qt.AlignLeft)
    layout.addWidget(window.prep_connection)
    window.prep_unavailable = label('', 'notice', True)
    window.prep_unavailable.hide(); layout.addWidget(window.prep_unavailable)
    window.prep_checks, window.prep_labels, window.prep_cards = {}, {}, {}
    descriptions = {'Music': 'Build the song library and album artwork', 'Photos': 'Create thumbnails and zoomable photos', 'Videos': 'Validate existing videos and prepare them for this iPod', 'Notes': 'Prepare text and Markdown for the reader'}
    for kind in ('Music', 'Photos', 'Videos', 'Notes'):
        card = QFrame(); card.setObjectName('card')
        card_layout = box(card, margins=(20, 18, 20, 18), spacing=12)
        window.prep_cards[kind] = card
        summary = QWidget(); row = box(summary, horizontal=True, spacing=15)
        check = CheckBox(kind); check.setChecked(True); check.setMinimumWidth(110)
        window.prep_checks[kind] = check; row.addWidget(check)
        text = box(spacing=5); text.addWidget(label(descriptions[kind], 'cardTitle', True))
        state = label('Connect an iPod in Disk Mode to check this library.', 'muted', True)
        window.prep_labels[kind] = state; text.addWidget(state); row.addLayout(text, 1)
        if kind == 'Videos':
            window.video_options, video_actions = action_menu('Video options', (
                ('Choose videos…', window.choose_videos),
                ('Review catalog backups…', window.review_video_catalog),
            ))
            window.choose_videos_button, window.video_repair_button = video_actions
            card_layout.addWidget(ControlRow(summary, window.video_options))
        else:
            card_layout.addWidget(summary)
        layout.addWidget(card)

    queue_card = QFrame(); queue_card.setObjectName('card')
    queue = box(queue_card, margins=(20, 18, 20, 18), spacing=12)
    queue_tools = QWidget(); queue_actions = box(queue_tools, horizontal=True, spacing=8)
    window.queue_add = button('Add media…', window.add_media, None, 'plus'); queue_actions.addWidget(window.queue_add)
    window.queue_options, queue_items = action_menu('Queue actions', (
        ('Add incompatible iPod music', window.queue_existing_audio),
        ('Show selected source', window.open_queued),
        ('Remove selected from queue', window.remove_queued),
    ))
    window.queue_incompatible, window.queue_open, window.queue_remove = queue_items
    window.queue_remove.setToolTip('Remove selected entries from this queue. Source files stay in place.')
    queue_actions.addWidget(window.queue_options)
    queue.addWidget(ControlRow(label('Up next', 'cardTitle'), queue_tools))
    window.queue_model = MediaModel(window)
    window.queue_table = QTableView(); window.queue_table.setModel(window.queue_model)
    window.queue_table.verticalHeader().hide(); window.queue_table.setShowGrid(False)
    window.queue_table.setSelectionBehavior(QAbstractItemView.SelectRows)
    window.queue_table.setSelectionMode(QAbstractItemView.ExtendedSelection)
    window.queue_table.setEditTriggers(QAbstractItemView.NoEditTriggers)
    window.queue_table.setMinimumHeight(125); window.queue_table.setMaximumHeight(200)
    queue.addWidget(window.queue_table)
    window.queue_table.selectionModel().selectionChanged.connect(window.update_queue_controls)
    window.queue_table.activated.connect(window.open_queued)
    shortcut = QShortcut(QKeySequence('Delete'), window.queue_table, activated=window.remove_queued)
    shortcut.setContext(Qt.WidgetWithChildrenShortcut)
    layout.addWidget(queue_card)
    window.prep_results = QTextEdit(); window.prep_results.setReadOnly(True)
    window.prep_results.setMinimumHeight(130); window.prep_results.hide(); layout.addWidget(window.prep_results)

    apple_card = QFrame(); apple_card.setObjectName('card')
    apple_layout = box(apple_card, margins=(20, 18, 20, 18), spacing=9)
    apple_layout.addWidget(label('Available in Apple OS', 'cardTitle'))
    apple_layout.addWidget(label('Use media from your own iPod folders in Apple OS too. Choose folders outside the Apple-managed library, then update the Apple library whenever their files change.', 'muted', True))
    window.apple_status = label('Connect an iPod in Disk Mode to check Apple OS availability.', 'muted', True)
    apple_layout.addWidget(window.apple_status)
    window.apple_eligibility = label('', 'muted', True); apple_layout.addWidget(window.apple_eligibility)
    apple_actions = box(horizontal=True, spacing=8)
    window.apple_choose = button('Choose folders…', window.choose_apple_folders, None, 'folder')
    window.apple_update = button('Update library', window.update_apple_os, None, 'refresh')
    apple_actions.addWidget(window.apple_choose); apple_actions.addWidget(window.apple_update); apple_actions.addStretch()
    apple_layout.addLayout(apple_actions); layout.addWidget(apple_card)
    layout.addStretch()

    footer = QFrame(); footer.setObjectName('editorFooter')
    footer_layout = box(footer, margins=(34, 16, 34, 16))
    window.prep_summary = label('Unchanged media is reused. Choose which videos to convert.', 'muted', True)
    controls = QWidget(); actions = box(controls, horizontal=True, spacing=8)
    window.prep_options, detail_actions = action_menu('More', (
        ('Review skipped files…', window.review_skipped),
        ('View preparation details', window.show_preparation_details),
    ))
    window.retry_button, window.prep_details = detail_actions
    window.prep_details.setVisible(False)
    actions.addWidget(window.prep_options)
    window.prepare_button = button('Prepare selected media', window.prepare, 'primary', 'prepare')
    actions.addWidget(window.prepare_button)
    footer_layout.addWidget(ControlRow(window.prep_summary, controls))
    outer.addWidget(footer)
    return page

def build_screens_page(window):
    page = QWidget(); page.setObjectName('workspace')
    outer = box(page, spacing=0)
    scroll, layout = window.scroll_page()
    outer.addWidget(scroll, 1)
    window.heading(layout, 'Screens', 'Choose what appears on your iPod and arrange it in your preferred order.')
    window.screen_state = screen_settings.defaults()
    window.screen_base = None
    window.screen_loaded_for = None
    editor = QFrame(); editor.setObjectName('card')
    contents = box(editor, margins=(20, 18, 20, 18), spacing=12)
    picker = QWidget(); picker_row = box(picker, horizontal=True, spacing=12)
    picker_row.addWidget(label('Edit menu', 'muted'))
    window.screen_picker = ComboBox(); window.screen_picker.addItems(['Home', 'Music', 'Settings', 'Extras'])
    window.screen_picker.setAccessibleName('Screen to edit')
    window.screen_picker.setMaximumWidth(240)
    window.screen_picker.currentTextChanged.connect(window.screens_controller.render_screen_items)
    picker_row.addWidget(window.screen_picker)
    window.screen_options, screen_actions = action_menu('Screen options', (
        ('Reset this screen', window.screens_controller.reset_screen),
        ('Reload from iPod…', window.screens_controller.reload_screens),
    ))
    window.screen_reset, window.screen_reload = screen_actions
    contents.addWidget(ControlRow(picker, window.screen_options))
    contents.addWidget(label('Check items to include them. Drag to reorder, or use the move buttons.', 'muted', True))
    window.screen_fixed = label('Shuffle, Repeat, and Edit Menus are always available in Settings.', 'muted', True)
    contents.addWidget(window.screen_fixed)
    window.screen_items = QListWidget(); window.screen_items.setObjectName('screenItems')
    window.screen_items.setMinimumHeight(350)
    window.screen_items.setDragDropMode(QAbstractItemView.InternalMove)
    window.screen_items.setDefaultDropAction(Qt.MoveAction)
    window.screen_items.itemChanged.connect(window.screens_controller.screen_items_changed)
    window.screen_items.model().rowsMoved.connect(window.screens_controller.screen_items_changed)
    contents.addWidget(window.screen_items)
    controls = box(horizontal=True, spacing=8)
    controls.addWidget(button('Move up', lambda: window.screens_controller.move_screen_item(-1)))
    controls.addWidget(button('Move down', lambda: window.screens_controller.move_screen_item(1)))
    controls.addStretch(); contents.addLayout(controls)
    window.screen_unavailable = label('', 'muted', True); contents.addWidget(window.screen_unavailable)
    window.screen_hint = label('', 'muted', True); contents.addWidget(window.screen_hint)
    window.screen_items.currentItemChanged.connect(window.screens_controller.screen_item_hint)
    layout.addWidget(editor)
    layout.addStretch()
    footer = QFrame(); footer.setObjectName('editorFooter')
    actions = box(footer, margins=(34, 16, 34, 16))
    window.screen_status = label('Connect an iPod in Disk Mode to save screen settings.', 'muted', True)
    window.screen_save = button('Save to iPod', window.screens_controller.save_screens, 'primary')
    actions.addWidget(ControlRow(window.screen_status, window.screen_save))
    outer.addWidget(footer)
    window.screens_controller.render_screen_items()
    return page

def build_games_page(window):
    page, layout = window.scroll_page()
    window.heading(layout, 'Games', 'Game availability depends on your iPod model.')
    window.games_loaded_for = None
    window.games_unavailable = label('Games aren’t available on this iPod.', 'notice', True)
    window.games_unavailable.hide(); layout.addWidget(window.games_unavailable)
    window.game_checks, window.game_cards = [], []
    for index, (title, description) in enumerate(games.GAMES):
        card = QFrame(); card.setObjectName('card')
        row = box(card, horizontal=True, margins=(20, 18, 20, 18), spacing=15)
        check = CheckBox(title); check.setChecked(True); check.setMinimumWidth(145)
        check.toggled.connect(window.games_controller.games_changed)
        window.game_checks.append(check); row.addWidget(check)
        row.addWidget(label(description, 'muted', True), 1)
        window.game_cards.append(card)
        layout.addWidget(card)
    window.games_status = label('Connect an iPod with games to check installed games.', 'muted', True)
    layout.addWidget(window.games_status)
    window.games_recover = button('Replace damaged game settings', window.games_controller.recover_games)
    window.games_recover.setToolTip('Choose a new game selection after a damaged settings file could not be read.')
    layout.addWidget(window.games_recover, 0, Qt.AlignLeft)
    window.games_save = button('Update games on iPod', window.games_controller.save_games, 'primary')
    layout.addWidget(window.games_save, 0, Qt.AlignLeft)
    layout.addWidget(label('Independent game packages', 'section'))
    layout.addWidget(label('Install a prepared game file for this iPod. Removing a game preserves its saved progress.', 'muted', True))
    window.custom_games = QListWidget(); window.custom_games.setMinimumHeight(100)
    window.custom_games.currentItemChanged.connect(lambda *_: window.update_controls())
    layout.addWidget(window.custom_games)
    package_controls=box(horizontal=True)
    window.game_import=button('Install game file…', window.games_controller.import_game)
    window.game_remove=button('Remove selected game', window.games_controller.remove_game)
    package_controls.addWidget(window.game_import);package_controls.addWidget(window.game_remove);package_controls.addStretch()
    layout.addLayout(package_controls)
    window.game_recover_requested=False
    layout.addStretch()
    return page

def build_setup_page(window):
    page, layout = window.scroll_page()
    window.heading(layout, 'iPod setup', 'Startup, Linnet software, and recovery.')
    window.build_editor_open = False

    header = QFrame(); header.setObjectName('card')
    details = box(header, margins=(20, 18, 20, 18), spacing=8)
    window.setup_device = label('Connect your iPod', 'cardTitle', True); details.addWidget(window.setup_device)
    window.setup_software = label('Connect with USB to get started.', 'muted', True); details.addWidget(window.setup_software)
    window.support_status = label('', 'muted', True); details.addWidget(window.support_status)
    window.discovery_issue = label('', 'notice', True); window.discovery_issue.hide(); details.addWidget(window.discovery_issue)
    window.hardware_report_button = button('Save hardware report…', window.collect_hardware_report)
    details.addWidget(window.hardware_report_button, 0, Qt.AlignLeft)
    window.install_detail = label('', 'notice', True); window.install_detail.hide(); details.addWidget(window.install_detail)
    window.setup_progress = QProgressBar(); window.setup_progress.setRange(0, 0)
    window.setup_progress.setTextVisible(False); window.setup_progress.hide(); details.addWidget(window.setup_progress)
    window.link_disk_button = button('Open Disk Mode', window.open_link_disk)
    window.link_disk_button.hide(); details.addWidget(window.link_disk_button, 0, Qt.AlignLeft)
    window.connection_help = QWidget(); window.connection_help.setMaximumWidth(700)
    instructions = box(window.connection_help, spacing=16)
    instructions.addWidget(label('Enter Apple Disk Mode', 'cardTitle', True))
    instructions.addWidget(label('If your iPod does not appear, follow these steps:', 'muted', True))
    steps = (
        ('Unlock the controls', 'Turn the <b>Hold</b> switch off.'),
        ('Restart the iPod', 'Hold <b>Menu + Select</b> (the center button) until the iPod restarts.'),
        ('Enter Disk Mode', '<b>Immediately</b> hold <b>Select + Play/Pause</b> until Disk Mode appears.'),
    )
    for number, (title, instruction) in enumerate(steps, 1):
        row = box(horizontal=True, spacing=12)
        marker = label(str(number), 'setupStepNumber'); marker.setFixedSize(24, 24)
        marker.setAlignment(Qt.AlignCenter); row.addWidget(marker, 0, Qt.AlignTop)
        text = box(spacing=5)
        text.addWidget(label(title, 'setupStepTitle', True))
        description = label(instruction, 'muted', True); description.setTextFormat(Qt.RichText)
        text.addWidget(description); row.addLayout(text, 1)
        instructions.addLayout(row)
    instructions.addWidget(label('Keep USB connected, then choose Find iPod below.', 'muted', True))
    details.addSpacing(8)
    details.addWidget(window.connection_help)
    window.setup_find_button = button('Find iPod', window.discover, 'primary', 'refresh')
    details.addWidget(window.setup_find_button, 0, Qt.AlignLeft)
    window.resume_classic_button = button('Resume Classic installation', window.resume_classic_setup, 'primary')
    window.resume_classic_button.hide(); details.addWidget(window.resume_classic_button, 0, Qt.AlignLeft)
    layout.addWidget(header)

    def card(parent_layout):
        frame = QFrame(); frame.setObjectName('card'); parent_layout.addWidget(frame)
        return frame, box(frame, margins=(20, 18, 20, 18), spacing=12)

    window.link_setup_panel = QWidget(); linked = box(window.link_setup_panel, spacing=14)
    _, link_startup = card(linked)
    row = box(horizontal=True); row.addWidget(label('Startup', 'cardTitle'), 1)
    window.link_startup_button = button('Read startup settings', lambda: window.open_link_setup('startup'))
    row.addWidget(window.link_startup_button); link_startup.addLayout(row)
    link_startup.addWidget(label('Check the default system and available startup choices.', 'muted', True))
    _, link_build = card(linked)
    row = box(horizontal=True); row.addWidget(label('Linnet', 'cardTitle'), 1)
    window.link_build_button = button('Change build…', lambda: window.open_link_setup('build'))
    row.addWidget(window.link_build_button); link_build.addLayout(row)
    window.link_build_status = label('', 'muted', True); link_build.addWidget(window.link_build_status)
    _, link_removal = card(linked)
    row = box(horizontal=True); row.addWidget(label('Remove Linnet', 'cardTitle'), 1)
    window.link_uninstall_button = button('Remove…', lambda: window.open_link_setup('remove'), 'danger')
    row.addWidget(window.link_uninstall_button); link_removal.addLayout(row)
    window.link_uninstall_detail = label('', 'muted', True); link_removal.addWidget(window.link_uninstall_detail)
    linked.addWidget(label('Media Manager opens Disk Mode automatically for these actions.', 'muted', True))
    window.link_setup_panel.hide(); layout.addWidget(window.link_setup_panel)

    window.startup_panel, startup = card(layout)
    window.default_title = label('Startup', 'cardTitle'); startup.addWidget(window.default_title)
    window.default_row = QWidget(); window.default_row.setMaximumWidth(620); row = box(window.default_row, horizontal=True, spacing=12)
    row.addWidget(label('Start automatically', 'muted'))
    window.default_choice = ComboBox(); window.default_choice.setMaximumWidth(300)
    window.default_choice.currentIndexChanged.connect(window.update_install_action); row.addWidget(window.default_choice, 1)
    window.default_button = button('Save', window.save_default_startup, 'primary'); row.addWidget(window.default_button)
    row.addStretch(); startup.addWidget(window.default_row)
    window.default_detail = label('Hold Menu during startup to choose a system for that start.', 'muted', True)
    startup.addWidget(window.default_detail)
    window.boot_upgrade_button = button('Update startup selector', window.upgrade_startup_selector)
    startup.addWidget(window.boot_upgrade_button, 0, Qt.AlignLeft)
    window.boot_upgrade_detail = label('', 'muted', True); startup.addWidget(window.boot_upgrade_detail)

    window.install_panel, build = card(layout)
    row = box(horizontal=True); window.install_title = label('Linnet', 'cardTitle'); row.addWidget(window.install_title, 1)
    window.build_editor_button = button('Change firmware…', window.toggle_setup_editor)
    row.addWidget(window.build_editor_button); build.addLayout(row)
    window.installed_build = label('', 'muted', True); build.addWidget(window.installed_build)
    window.build_summary = label('', 'muted', True); build.addWidget(window.build_summary)
    window.build_editor = QWidget(); install = box(window.build_editor, spacing=14); build.addWidget(window.build_editor)
    install.addWidget(label('Firmware', 'section'))
    window.firmware_path = str(window.settings.value('firmware_bundle', ''))
    window.firmware_picker = QFrame(); window.firmware_picker.setObjectName('setupFirmwarePicker')
    selection = box(window.firmware_picker, margins=(16, 14, 16, 14), spacing=7)
    row = box(horizontal=True, spacing=14)
    window.firmware_summary = label('', 'setupFirmwareTitle', True)
    window.firmware_summary.setTextFormat(Qt.PlainText); row.addWidget(window.firmware_summary, 1)
    window.firmware_button = button('Choose ZIP…', window.choose_firmware, None, 'folder')
    window.firmware_button.setToolTip('Select the downloaded firmware ZIP without extracting it.')
    row.addWidget(window.firmware_button, 0, Qt.AlignTop); selection.addLayout(row)
    window.boot_detail = label('', 'setupFirmwareDetail', True)
    window.boot_detail.setTextFormat(Qt.PlainText); selection.addWidget(window.boot_detail)
    window.firmware_label = StatusLabel(''); window.firmware_label.setObjectName('setupFirmwareFile')
    window.firmware_label.setAccessibleName('Selected firmware file'); selection.addWidget(window.firmware_label)
    install.addWidget(window.firmware_picker)
    window.source_options = QFrame(); window.source_options.setObjectName('setupSourceOptions')
    source = box(window.source_options, margins=(16, 12, 16, 12), spacing=9)
    window.source_options_button = QToolButton(); window.source_options_button.setObjectName('setupSourceToggle')
    window.source_options_button.setText('Build from source (advanced)')
    window.source_options_button.setCursor(Qt.PointingHandCursor)
    window.source_options_button.setToolTip('Compile your own changes from a configured Linnet source checkout.')
    window.source_options_button.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
    window.source_options_button.setArrowType(Qt.RightArrow); window.source_options_button.setCheckable(True)
    window.source_options_button.toggled.connect(window.toggle_source_options)
    install.addWidget(window.source_options_button, 0, Qt.AlignLeft); install.addWidget(window.source_options)
    window.build_from_source = CheckBox('Compile firmware on this computer')
    window.build_from_source.setChecked(not getattr(sys, 'frozen', False) and
        window.settings.value('build_from_source', False, type=bool))
    window.build_from_source.toggled.connect(window.firmware_source_changed); source.addWidget(window.build_from_source)
    window.source_note = label('Source builds require a configured Linnet source checkout. '
        'This installed app uses User or Developer firmware ZIPs.' if getattr(sys, 'frozen', False) else
        'Uses this source checkout and requires the Linnet build tools.', 'muted', True)
    source.addWidget(window.source_note)
    window.build_title = label('Firmware to compile', 'setupStepTitle'); source.addWidget(window.build_title)
    window.boot_choice = ComboBox(); window.boot_choice.setMaximumWidth(500)
    window.boot_choice.currentIndexChanged.connect(window.update_install_action); source.addWidget(window.boot_choice)
    window.source_build_detail = label('', 'muted', True); source.addWidget(window.source_build_detail)
    window.source_options.setVisible(window.build_from_source.isChecked())
    window.source_options_button.setChecked(window.build_from_source.isChecked())
    window.method_title = label('Startup options', 'section'); install.addWidget(window.method_title)
    window.method_choice = ComboBox(); window.method_choice.setMaximumWidth(500)
    window.method_choice.currentIndexChanged.connect(window.update_install_action); install.addWidget(window.method_choice)
    window.method_detail = label('', 'muted', True); install.addWidget(window.method_detail)
    action = box(horizontal=True, spacing=14)
    window.install_button = button('Install firmware', window.install_selected, 'primary'); action.addWidget(window.install_button, 0, Qt.AlignLeft | Qt.AlignTop)
    window.install_hint = label('', 'muted', True); action.addWidget(window.install_hint, 1)
    install.addLayout(action)

    window.uninstall_panel, removal = card(layout)
    row = box(horizontal=True); row.addWidget(label('Remove Linnet', 'cardTitle'), 1)
    window.uninstall_button = button('Remove…', window.uninstall_selected, 'danger'); row.addWidget(window.uninstall_button)
    removal.addLayout(row)
    window.uninstall_detail = label('', 'muted', True); removal.addWidget(window.uninstall_detail)
    layout.addStretch()
    return page

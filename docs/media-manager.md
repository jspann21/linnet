# Linnet Media Manager

Media Manager is the Windows app for installing Linnet, preparing media, and
managing your iPod. Download **LinnetSetup.exe** and the firmware ZIP for your
model from [Releases](https://github.com/jspann21/linnet/releases). Run the
installer, then open **Linnet** from the Start menu. The optional portable ZIP
runs after extraction.

[Install firmware](#install-or-restore-firmware) ·
[Add media](#prepare-your-library) ·
[Playlists and games](#playlists-games-and-screens) ·
[Font and storage](#ipod-theme-font) ·
[Firmware updates](#update-firmware-and-startup-settings) ·
[Remove Linnet](#reinstall-or-remove-linnet) ·
[Troubleshooting](#troubleshooting)

## Connect your iPod

Connect with USB and choose **Find or switch iPod**. The first read can take
longer for a large collection; progress appears in the activity area. Wait for
it to finish, or choose **Stop** to cancel.

Media preparation needs **Disk Mode**. If Linnet is running, choose **Open Disk
Mode** in Media Manager when offered. Otherwise, open **Settings → USB Disk
Mode** on the iPod and reconnect. For an iPod without Linnet, follow **Enter
Apple Disk Mode** on the app's **iPod setup** screen.

**iPod setup** identifies the device and explains any installation requirements.
See [supported hardware](hardware.md) and [storage compatibility](storage.md).
Classic CE-ATA drives are unsupported.

## Install or restore firmware

Installation and use are at your own risk. Linnet is provided without warranty;
to the extent permitted by law, its copyright holders and contributors are
not liable for device damage or data loss. Read the [warranty and liability
notice](../NOTICE.txt) and keep independent backups before changing your iPod.

1. Download the ZIP for your iPod: choose `-user.zip` for everyday use or
   `-dev.zip` for diagnostics and Linnet Link. Classic 6G, 6.5G and 7G share the
   `classic6g-7g` download.
2. Open **iPod setup → Choose ZIP…** and select the whole ZIP without extracting
   it. Check the model and build type displayed by the app.
3. Choose **Install firmware**, review the prompts, and allow Windows
   administrator approval when requested. Keep USB connected until setup finishes.
4. Prepare your media, then choose **Eject safely** to start Linnet. A fresh
   Classic finishes in Linnet Disk Mode, ready for preparation. For other
   installation routes, eject to start Linnet, then enter **Settings → USB Disk
   Mode** on the iPod and reconnect before preparing media.

For a first Classic installation, the app asks you to turn Hold off and hold
**Menu + Select** for about 12 seconds. Release when the app tells you to. The
remaining restarts are automatic. If interrupted, reconnect the same iPod and
choose **Resume Classic installation**. Other interrupted installations offer
**Resume installation**.

Monochrome 4G, Photo/Color, Video, and Classic 6G, 6.5G and 7G support Rockbox
alongside Linnet and Apple software in the startup chooser. Setup verifies
Rockbox files for your model. To add Rockbox later, copy its firmware files
without installing its bootloader, then choose **Startup software → Rockbox**
in Media Manager to register them. See [startup settings](#update-firmware-and-startup-settings)
for automatic startup and one-time choices.

If you use an existing Rockbox bootloader to start Linnet, the chooser uses
the saved original Rockbox runtime. Classic setup also recognizes an existing
Rockbox dualboot bootloader and saves a complete flash backup for removal.

Setup preserves existing media and Apple firmware and saves recovery backups.
Keep those backups for removal or recovery; their location is listed under
[Computer storage](#computer-storage-settings).

## Prepare your library

1. Connect the iPod in Disk Mode.
2. Open **Music**, **Photos**, **Videos**, or **Notes** and add files or folders.
   Adding to the queue does not copy files yet.
3. Open **Preparation**, select the media types to update, and review the video
   choices. Choose **Prepare selected media**.
4. Review the plan, including space requirements and changes to existing files.
   Choose **Start preparation**, or **Approve changes and prepare** when shown.
5. Review any skipped files or errors, then choose **Eject safely** and restart
   Linnet.

| Media | What preparation does |
| --- | --- |
| Music | Builds the song library and album artwork; converts unsupported audio when enabled. |
| Photos | Creates thumbnails and photos sized for the iPod. |
| Videos | Checks compatible files and converts others for the selected model. |
| Notes | Prepares text and Markdown for the reader; Markdown appears as plain text. |

Available features depend on the iPod. Preparation also updates the on-device
**Files** browser. See [device controls](navigation.md), [Files](files.md), and
[video formats](video-format.md) for playback details.

Preparing Music also includes compatible tracks that iTunes already copied to
the iPod. To make files from `/Music` available in Apple software too, use
[Available in Apple OS](#make-folders-available-in-apple-os).

Queue choices are saved for each iPod. After changing source files, prepare the
affected media again; unchanged work is reused. Replacements and library
removals appear for review before approval. Space estimates include temporary
files and recovery copies, so they may exceed the final media size.

**Stop** cancels at a safe point. To review results, open **Preparation → More →
View preparation details**. Use **Review skipped files…** to retry repaired
sources. Search the library with **Ctrl+F** and refresh it with **F5**.

### Audio and video preparation

The installed and portable apps include FFmpeg and FFprobe for conversion and
media checks. They work automatically. If these components cannot start,
run **LinnetSetup.exe** again or extract a fresh portable ZIP.

**Settings → Media preparation** controls photo detail, conversion of
unsupported music to WAV, and online artwork lookup. Higher photo detail uses
more space and takes longer to prepare.

## Playlists, games, and screens

### Playlists

After preparing Music, open **Music → Library actions → Playlists…**. Create a
playlist, add songs, and arrange their order. Choose **Save to iPod**, then
eject safely. Playlists support up to 16 lists with 1,024 songs each.

**Import M3U…** reads UTF-8 `.m3u` or `.m3u8` playlists. Review the matches,
choose **Import matched songs**, then **Save to iPod**. Unmatched entries are
skipped. **Export M3U…** saves the selected playlist to your computer.

### Games

On Photo, Video, and Classic, open **Games**, select the games you want, and
choose **Update games on iPod**. Eject safely and restart Linnet. Built-in game
packages are included with the app and firmware ZIPs.

Use **Install game file…** for a compatible custom game package. **Remove
selected game** removes it while retaining scores and progress. See
[creating games](game-authoring.md) for custom packages.

In the iPod's Games browser, **Play/Pause** controls your current music. Choose
music before launching a game to keep it playing during gameplay. Music Quiz
uses its own clues and restores the previous Music queue when it exits.

### Screen menus

Open **Screens**, choose which menu items appear and their order, then **Save
to iPod**, eject, and restart. The iPod's **Settings → Edit Menus** changes the
same menus. **Screen options → Reload from iPod…** replaces your desktop draft
with the current device choices. If both interfaces changed the same menu,
review the conflict before saving. See [device menus](navigation.md).

## Make folders available in Apple OS

Preparing media for Linnet and updating Apple's library are separate actions.
On a supported device:

1. Prepare Music for Linnet first.
2. Open **Preparation → Available in Apple OS → Choose folders…** and select
   existing folders under Music, Photos, or Videos.
3. Review readiness, then choose **Update library**. Repeat after changing files
   in those folders.

Apple library updates require a supported iPod with FAT32 storage. Monochrome
iPods offer Music only; Photo / Color offers Music and Photos; Video and Classic
also support Videos. Classic may ask you to reconnect in Apple Disk Mode to read
its Apple identifier. Compatible plain-text Notes are read directly by Apple OS.
Existing Apple tracks and playlists are preserved.
See [supported devices, formats, and folder requirements](apple-stock-music.md#supported-boundary).

## iPod theme font

1. Connect a color iPod in Disk Mode.
2. Open **Settings → iPod appearance → Choose font → Download and use Apple
   font (3.8 MB)**.
3. Eject safely and restart Linnet.

The app downloads verified firmware directly from Apple and extracts Podium
Sans on the computer. Compatible color themes use it; other themes retain
their included font. Choose **Use included font**, eject, and restart to switch
back. The font is kept through firmware updates and reinstalls.

Apple's software license is linked in Settings. See the
[font notice](../sdk/assets/PODIUM-SANS-NOTICE.txt) for source and usage details.

## Computer storage settings

Open **Settings → Computer storage** to see cache usage and location. Under
**Manage cache**:

- **Change location…** moves reusable media data to an empty local folder.
- **Clear cache…** deletes reusable files; preparation recreates them when needed.
- **Open cache folder** and **Refresh size** help inspect usage.

Clearing or moving the cache keeps iPod files, saved selections, and recovery
backups. These actions are unavailable while another operation uses the cache.

The default cache is `%LOCALAPPDATA%/Linnet/Cache/Media`. Saved state and
receipts stay under `%LOCALAPPDATA%/Linnet/Media`; startup backups stay under
`%LOCALAPPDATA%/Linnet/Installer`. Use **Backups and recovery → Open folder** to
open those locations. Keep recovery backups even if you clear the cache or
remove the desktop app.

## Update firmware and startup settings

Connect in Disk Mode and open **iPod setup → Change firmware…**. Choose a new
matching ZIP, then **Update firmware**. This also switches between User and
Developer builds. Eject safely when the app says the update is ready. Media and
settings are kept.

When **Startup** offers **Start automatically**, choose the default system and
**Save**. Its startup selector also lets you hold **MENU** while the iPod starts
for a one-time choice. If the app shows **Read startup settings** while Linnet
Link is connected, choose it to open Disk Mode and read these controls.

**Update startup selector**, when offered, updates the bootloader. Select a
matching ZIP first. An interrupted update offers **Resume startup selector
update**; finish it before changing other startup settings.

## Reinstall or remove Linnet

To reinstall, use **Change firmware…**, choose a matching ZIP, and **Update
firmware**. Settings and prepared media are retained.

To remove Linnet:

1. Restore anything you want to keep from **Trash**.
2. Open **iPod setup → Remove Linnet → Remove…**.
3. Review the previous startup software and confirm removal. If Trash contains
   files, review them first; **Empty Trash and remove Linnet** permanently
   deletes them.
4. Keep USB connected through any restarts and follow the app's connection
   prompts until cleanup finishes.

Removal restores the saved previous startup software and deletes Linnet's
system files, settings, and prepared libraries. The Music, Photos, Videos, and
Notes folders remain. Media prepared only for Linnet may need an Apple library
sync to appear in Apple OS. A verified recovery backup is required.

**Classic:** User builds support removal directly from Linnet Disk Mode. Keep
the original bootloader backup. Media Manager safely ejects into the saved
recovery monitor and restores the original boot flash. If older firmware rejects
recovery eject, update to the latest Classic User ZIP, eject safely, start the
updated Linnet OS, then choose **Settings → USB Disk Mode** and reconnect to
retry removal. This uses the updated OS's Disk Mode even if the installed Boot
image is older. During removal, the iPod
restarts into the previous startup software; its USB disk must appear in
Windows for cleanup to finish. Follow the reconnect dialog if it does not.
If cleanup is interrupted, reconnect the same iPod and choose **Remove…** again.

## Remove media and use Trash

Use a library's removal controls to remove content. Open **Trash**, select an
item, and choose **Restore** to recover it, or delete it permanently to free
space. Items in Trash still occupy iPod storage. Removing an item from the
preparation queue only removes the queued action; its source file is kept.

A removed prepared library can be recreated by preparing its originals again.
Review the app's removal confirmation to see which files will change.

## Troubleshooting

| Problem | What to do |
| --- | --- |
| iPod not found | Enter Apple Disk Mode using the instructions in **iPod setup**, try a direct USB port, then **Find or switch iPod** again. |
| USB detected but storage unavailable | Assign the existing FAT32 volume a drive letter in Windows Disk Management, then search again. |
| Reading the collection takes a long time | Large libraries take longer, especially on the first read. Check progress or choose **Stop**. If iTunes is syncing, let it finish before refreshing. |
| Firmware ZIP moved, missing, or rejected | Choose **Choose ZIP…** again and select a ZIP for the detected model. Leave it unextracted. |
| Classic installation reports an unreadable disk block | Windows reported a bad block for this iPod during the installation attempt. Back up readable files and have the disk checked before resuming; reinstalling cannot repair damaged sectors. |
| Classic installation cannot read or write files | Check the USB connection and that Windows can open the iPod drive. Repeated failures can indicate disk damage, but a generic access error alone does not establish that cause. |
| Preparation was interrupted | Reconnect the same iPod in Disk Mode, refresh, and review its status before preparing again. Completed work is reused. |
| **Media index recovery needed** | Choose **Recover media indexes** and keep the iPod connected until recovery and the rescan finish. Leave its recovery files in place. |
| Videos show **Needs repair** | Open **Preparation → Video options → Review catalog backups…**. Select a valid backup and choose **Restore selected backup**. Videos added after that backup may need preparation again. If none is valid, keep the files for recovery. |

### Send a hardware report

Open **iPod setup → Save hardware report…**, review it, then choose **Save
report…** or **Copy report**. **Contact Linnet support** opens a GitHub issue;
include the problem and any storage changes, then attach or paste the report.
Nothing uploads automatically. Reports exclude serial numbers, usernames, local
paths, and media. GitHub issues and attachments are public.

For operation errors, open **Diagnostics** in the footer and **Export support
file**. Review it before sharing: an error may contain a personal media name.
Preparation results are available under **Preparation → More → View preparation
details**.

### Take an iPod screenshot

With a Developer or Trace build running through Linnet Link, choose
**Screenshot…** in the device card at the bottom of the sidebar. Use **Copy
image** or **Save PNG…** in the preview. Screenshots require the running Linnet
OS; they are unavailable in Disk Mode and User builds.

<details>
<summary>Running Media Manager from source</summary>

Follow the [Windows source setup](../README.md#develop-on-windows). Supply
`ffmpeg.exe` and `ffprobe.exe` on PATH, or choose their folder under **Settings →
Advanced preparation → FFmpeg (source builds)**.

To compile firmware during installation, open **iPod setup → Build from source
(advanced)** and enable **Compile firmware on this computer**, then choose User
or Developer firmware. This requires the configured source checkout and
compiler. See [source installation](../README.md#open-media-manager-from-source)
and [boot and recovery tooling](bootstrap-tooling.md) for developer commands.

</details>

# Builds and downloads

Linnet is provided without warranty. Installation and use are at your own risk;
to the extent permitted by law, its copyright holders and contributors are
not liable for device damage, data loss, or repair and recovery costs. See
`NOTICE.txt` in each package and the warranty and liability terms in `LICENSE`
(`LICENSE.txt` in firmware ZIPs). Keep independent backups before changing your
device.

The **Build downloads** GitHub Actions workflow builds on version-tag pushes
(`v*`), pull requests, and manual runs. After it succeeds, open the run
under **Actions** and download `linnet-downloads`. These build artifacts expire
after 14 days. Public release assets remain on the repository's Releases page.

## Download contents

| File | Purpose |
| --- | --- |
| `LinnetSetup.exe` | Recommended Windows x64 installer: installs Media Manager, adds a Start menu shortcut, and provides an uninstall entry |
| `linnet-media-windows.zip` | Optional portable Media Manager, including `Linnet Media.exe`, Python, Qt, media preparation helpers, and Apple database helpers |
| `linnet-photo4g-user.zip` / `linnet-photo4g-dev.zip` | iPod Photo firmware |
| `linnet-video5g-user.zip` / `linnet-video5g-dev.zip` | iPod Video firmware |
| `linnet-ipod4g-user.zip` / `linnet-ipod4g-dev.zip` | Monochrome fourth-generation iPod firmware |
| `linnet-classic6g-7g-user.zip` / `linnet-classic6g-7g-dev.zip` | iPod Classic 6G and 7G firmware (including 6.5G) and initial Boot setup; follow Media Manager's connection and button prompts |
| `linnet-source.zip` | Repository source at the exact commit used for these builds, including build scripts and vendor notices |
| `SHA256SUMS.txt` | SHA-256 checksums of the downloads and these instructions |

Download **LinnetSetup.exe**, run it, and install. Open **Linnet** from the Start
menu. The installer supports Windows 10 and later with x64 application support.
It installs for the current user under `%LOCALAPPDATA%/Programs/Linnet` without
requesting administrator access. A desktop shortcut is optional. Close Linnet
before installing or uninstalling; setup will not close a running app for you.

Remove the desktop app through **Windows Settings → Apps → Installed apps →
Linnet → Uninstall**. Saved settings, caches, and recovery backups are retained.
This removes the computer app; iPod firmware is managed separately inside Linnet.

For portable use, extract the entire optional ZIP, then open
`Linnet Media/Linnet Media.exe`. Keep `_internal` beside the executable. Both
packages include the same app and helpers. End users do not need Python, a
compiler, or WSL. FFmpeg and FFprobe are included and selected automatically;
media conversion works without a separate installation or folder selection.
The installer and app are currently unsigned.

The included conversion tools use FFmpeg and x264 under GPLv3 or later. Their
licenses, complete matching source archives, pinned dependency details and build
script are included in `_internal/conversion-tools`. The executables remain
separate and replaceable. See [FFmpeg's licensing information](https://ffmpeg.org/legal.html).

Choose `-user.zip` for everyday use or `-dev.zip` for diagnostics and Linnet Link. Media Manager reads the build type and iPod model from the ZIP and displays them automatically.

Classic 6G, 6.5G and 7G use the same `classic6g-7g` download.

Select the whole firmware ZIP for the detected target inside Media Manager
**without extracting it**. Media Manager selects the required files for your
installation. Each ZIP has a `README.txt`, `LICENSE.txt`, `NOTICE.txt`, a checksum manifest, and a `firmware/`
folder containing `linnet-os`, `linnet-boot-standard`, and `linnet-boot-multiboot`
images and reports. Both bootloader variants support verified Rockbox startup
choices, including files added after installing Linnet. Packaging runs image verification and the installer's bundle
validation. User OS/Boot images exclude developer tools. Developer ZIPs include diagnostics and Linnet Link; Media Manager binds their firmware to the connected iPod without compiling on the computer. The Classic
ZIP additionally includes a precompiled bootstrap template; Media Manager binds it to the inspected iPod, prompts for DFU, then owns the
remaining Link, NOR and native Disk Mode installation steps. See
[hardware support](https://github.com/jspann21/linnet/blob/main/docs/hardware.md) for tested configurations and compatibility.

Firmware and Media Manager packages include licensed fallback fonts. Optional
Apple Podium Sans is downloaded directly from Apple and extracted on the user's
computer through [Settings → iPod appearance → Choose font](https://github.com/jspann21/linnet/blob/main/docs/media-manager.md#ipod-theme-font).
Keep that font and the downloaded Apple firmware out of release assets and source.

## Publish a version

1. Review the successful build from the commit to release. Check the packaged
   Windows app and firmware on the supported devices.
2. Preserve the [GPLv3 license](https://github.com/jspann21/linnet/blob/main/LICENSE), copyright notices, and vendor
   notices, and provide complete corresponding source alongside binaries.
   Confirm redistribution rights for included fonts and other assets before
   public distribution.
3. Push a version tag such as `v1.0.0` on that commit. This starts a fresh build
   and creates a **draft GitHub Release** containing all downloads and checksums.
4. Review the draft's downloads and edit its notes to describe that version and
   its hardware limits, then publish it from GitHub Releases.

All packages must build successfully before the draft is created. Rerunning a
tag build can update its draft assets; the workflow refuses to replace a
published release. Branch pushes do not trigger a build; manual runs on branches
produce Actions artifacts. No personal access token is needed: only the tag release job has
repository write permission, through GitHub's built-in token.

## Build locally on Windows

Install Git for Windows and Python 3.14 x64, then run from the repository root:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r tools/studio/requirements-build.txt
.\.venv\Scripts\python.exe linnet setup
.\.venv\Scripts\python.exe linnet doctor
.\.venv\Scripts\python.exe linnet release bundle --target photo4g-p98 --build user --output out/releases/linnet-photo4g-user.zip
.\.venv\Scripts\python.exe linnet release bundle --target photo4g-p98 --build dev --output out/releases/linnet-photo4g-dev.zip
.\.venv\Scripts\python.exe linnet release bundle --target classic6g-n25 --build user --output out/releases/linnet-classic6g-7g-user.zip
.\.venv\Scripts\python.exe linnet release bundle --target classic6g-n25 --build dev --output out/releases/linnet-classic6g-7g-dev.zip
.\.venv\Scripts\python.exe linnet release media --output out/releases/linnet-media-windows.zip --installer-output out/releases/LinnetSetup.exe
```

Repeat both bundle types for the remaining targets. Classic uses the internal
target ID `classic6g-n25` and the shared `classic6g-7g` download name. Installer
packaging downloads and verifies the pinned Inno Setup compiler, then prepares
it in portable mode under `out/toolchains/`. Its version and checksum are in
[`config/inno-setup.json`](https://github.com/jspann21/linnet/blob/main/config/inno-setup.json). The installed app version
comes from `config/system.toml`. Omit `--installer-output` to build only the
portable ZIP. Output files must not already exist; choose a fresh output
directory when rebuilding. Local
builds use the current working files, while GitHub uses the checked-out commit.
Keep generated artifacts in `out/`, which is excluded from Git.

Desktop branding uses `assets/linnet.ico` for the app, installer, uninstaller,
and shortcuts, with matching PNG artwork in the setup wizard. Regenerate these
assets from the boot artwork with `python -m tools.studio.media_icon`.
The running app and both shortcuts share the `Linnet.MediaManager` taskbar ID.

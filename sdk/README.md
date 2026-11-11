# Linnet UI SDK

Author static screens in constrained TSX and compile them to native C. Native
services own playback and behavior; TSX describes presentation. There is no
JavaScript runtime, CSS engine, or device-side app loader.

For native game projects and prepared assets, see [Creating Linnet games](../docs/game-authoring.md).

For environment setup, see [Develop on Windows](../README.md#develop-on-windows).

## Compile and preview

Use the same system configuration for content compilation, simulation, and the
firmware build. For example:

```console
python linnet ui compile --system targets/photo4g-p98/system.toml --output out/ui
python linnet simulator --system targets/photo4g-p98/system.toml --all-scenes --output out/simulator
```

Open `out/simulator/index.html`. The simulator uses the native renderer and
writes PNG frames plus `report.json`. Without `--all-scenes`, it starts at
Home; `--tape path/to/events.json` replays input instead.

An input tape is a JSON list of events with `kind`, `value`, and optional
`repeat` and `capture`. Kind 0 renders, 1 turns the wheel, 2 sets the button
mask, 3 sets Hold, and 4 sets headphone presence. Button masks are the same as
[Link input](../docs/device-link.md#diagnostics-input-and-screenshots).
The runner bounds total events and captures.

Rebuild the OS to include changes. Generated `.lapp` and `.ltheme` files are host
artifacts, not packages you can copy onto a device to install.

## Start an app or theme

An authored app is a static UI screen with optional trusted C canvas callbacks,
compiled into the firmware. Adding interactive behavior beyond the existing UI
components requires native firmware code. Built-in screens live in `ui/system/`;
the compiler also discovers custom screens in `apps/<name>/`. It has no runtime
app loader or separate application lifecycle.

To create a custom screen or theme, copy a template into an unused folder from
the repository root. Create `apps/` when starting your first custom screen:

```powershell
New-Item -ItemType Directory -Force apps | Out-Null
Copy-Item -Recurse sdk/templates/app apps/starter
Copy-Item -Recurse sdk/templates/theme themes/starter
```

The default Photo/reference configuration is the simplest starting point:

1. Copy `config/system.toml` to an ignored working path such as `out/starter-system.toml`.
2. Add `local.example.starter` to its ordered `[ui] apps` array. Keep Home first and preserve required destinations.
3. Set `[ui] theme = "local.example.starter-theme"` if using the theme template.
4. Add a matching label and destination to the reference Home menu in `ui/system/home/screen.tsx`.
5. In `tools/studio/menu_definition.py`, append
   `Item('starter', 'Starter', 'local.example.starter', device_editable=False)`
   to the Home items and append its zero-based item index to Home's default
   order. Home uses this menu definition to choose which rows appear; changing
   TSX alone does not expose a new row. Each menu group supports at most 16
   items. Saved menu orders override defaults and must also include the new item.
6. Pass `--system out/starter-system.toml` to compile, simulator, and OS build.

The compiler selects at most 64 screens. Targets using native presentation
profiles also need a matching screen under that profile's `screens/<app-id>/`
directory and a menu entry in the profile's Home screen. A reference screen is
not automatically scaled into a native profile. Firmware memory limits still
apply when adding screens.

Choose unique lowercase dot-separated IDs before sharing an app or theme.
Update its manifest, system selection, and navigation references together.
The optional **UI Demo** is `ui/system/ui-demo`, with ID `linnet.system.ui-demo`.
Select it in `[ui] apps` and set `[features] demo = true` to enable it for
previews. To reach it on the device in a custom build, include its item index
in Extras' default order in `tools/studio/menu_definition.py`. The Notes screen
is `ui/system/notes`, backed by `services/notes`.

## TSX essentials

A screen exports one function with no arguments and one JSX return. Import
components by name from `@linnet/ui`. Arbitrary expressions, loops, closures,
spreads, and runtime JavaScript are rejected.

```tsx
import { Screen, Text } from "@linnet/ui";
export default function Scene() {
  return (
    <Screen>
      <Text x={4} y={4} w={212} h={20} fg="theme.accent">Library</Text>
    </Screen>
  );
}
```

Coordinates are relative to the parent. Rectangles must remain inside it.
Reference screens use a 220 × 176 canvas; native profiles use their actual
display dimensions.

| Need | Components or properties |
| --- | --- |
| Structure and text | `Screen`, `View`, `Text`, `Dialog`, `Toast` |
| Navigation | `Menu`, `VirtualList`, `Grid`, `Scrollbar`; paired `items` and `targets` |
| Status | `StatusBar`, `ProgressBar` |
| Images and media | `Icon`, `Image`, `AlbumArt`, `PhotoSurface`, `VideoSurface` |
| Native drawing | `CanvasSurface` with a declared C callback |
| Focus and updates | `focus`, `flags`, and `deps` |

Static strings are printable ASCII. Each scene supports at most 256 nodes,
16 nesting levels, and one initial focus. Each static `items` or `targets` array
holds at most 64 entries. For exact properties,
bounds, flags, and dependencies, see [the parser](../tools/content/tsx.py) and
[native UI API](../ui/runtime/ui.h).

## Themes and resources

Themes live in `themes/*/theme.toml`. Required colors are `foreground`,
`background`, `accent`, `muted`, and `panel`. Use semantic colors such as
`theme.accent` so screens follow runtime theme changes.

The compiler can bundle up to 16 compatible themes. Profile editions replace
shared themes by stable ID; target editions take precedence over both.
Target allowlists restrict availability. All themes preserve scene order and
service bindings. Settings applies a selection immediately; optional theme
state records persist it.

**Album Colors** derives the palette from music artwork using Linnet's histogram
and contrast algorithm. The original media pixels are not recolored.

Assets use repository-contained paths and bounded PPM inputs. Fonts are baked
alpha glyphs with optional proportional advances. Keep each font's own notice
with it. Bundled fonts use the [DejaVu notices](assets/FONT-LICENSE.txt).
Themes with `font_source = "apple-podium-sans16"` can use an optional,
locally extracted [Podium Sans font](assets/PODIUM-SANS-NOTICE.txt); their `font`
and `font_license` fields describe the bundled fallback. Apple glyphs are not
stored in the source tree or release packages. Use Media Manager's
[Settings → iPod appearance → Choose font](../docs/media-manager.md#ipod-theme-font) to prepare
the optional file. Missing or invalid files use the bundled font.

The simulator also accepts `--apple-font path/to/PODIUM.FNT` for local previews.
It validates and loads the same packed font format as firmware. Its output may
contain the extracted font and should remain local.

Use `--menus path/to/MENUS.BIN` to preview an encoded Media Manager menu selection
and exercise the on-device editor with hidden shortcuts. The simulator validates
the 80-byte menu envelope before loading it.

To bake a suitably licensed replacement font:

```console
python -m tools.content.font --font path/to/font.ttf --output themes/starter/font.json
```

Set the theme's `font` and `font_license` paths accordingly. Normal builds use
the saved font data, not fonts installed on the host.

## Native callbacks

Declare `canvas` capability and a source/symbol mapping in the app manifest.
Callbacks receive `ln_ui_canvas_context` and must obey the drawing bounds,
resolved colors, and bounded work budget. They are trusted C linked into the OS;
the TSX compiler does not sandbox them.

The [compiler guide](../docs/ui-compiler.md) explains generated artifacts and
runtime contracts. Physical-device checks remain necessary for timing, sound,
input hardware, and display quality.

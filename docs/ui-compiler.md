# Static UI compiler

For authoring and commands, use the [SDK](../sdk/README.md). This page describes
the compiler/runtime boundary.

The compiler discovers selected app and theme manifests, validates constrained
TSX, resolves the target's presentation, and emits immutable native C. Generic
screens may use the reference canvas; a target or shared presentation profile
can supply a complete native screen set. M25 and N25 use the shared 320 × 240
profile.

## Outputs

| Output | Purpose |
| --- | --- |
| `linnet_ui.c` | Linked scene tables, resources, palettes, fonts, and native callbacks |
| `scenes.json` / `LNUI` | Host view of scenes, theme variants, geometry, and enabled scenes |
| `APPL`, `THME` | Compiled host app/theme artifacts; not device-installed modules |
| `CNTN` | Artifact lengths and hashes, selected source hashes, and content identity |
| `FONT` | Baked font input with bounded glyph cells, alpha coverage, and optional advances |

Content identity excludes informational host paths. Normal builds use checked-in
font data rather than discovering system fonts. Native generation packs glyphs
losslessly and deduplicates identical immutable arrays.

Themes may declare `font_source = "apple-podium-sans16"` alongside their
bundled fallback font and its license. This sets the optional-font flag without
embedding Apple glyphs. The compiler rejects baking the known extracted Apple
font into generated C. Public packaging also checks recursively included assets
for known Apple font files and firmware inputs.

## Runtime contract

[ui.h](../ui/runtime/ui.h) defines node kinds, semantic colors, geometry flags,
focus, and callback interfaces. The renderer resolves dynamic selection, counts,
clipping, and theme state. There is no runtime TSX/JSON parser.

Disabled feature scenes retain stable IDs and positions but omit native nodes.
Menu links and navigation respect the enabled mask. The first scene must remain
enabled because it owns startup and Home.

All bundled themes preserve scene identity and order. Switching themes changes
the active scene table, font, palette, and metrics; media pixels retain their
own colors. Native callbacks must obey their drawing bounds and work budget.

On color targets, display settings read the optional `/.LINNET/PODIUM.FNT`
once during startup. The renderer selects it only for marked themes after size,
geometry, and CRC validation. Missing or invalid data leaves the bundled font
active. The bounded packed buffer remains in RAM for the session; rendering
performs no font I/O. See [the font format](formats.md#content-and-transport).

Implementation: [compiler](../tools/content/compiler.py),
[TSX parser](../tools/content/tsx.py), [layouts](../tools/content/layouts.py),
[native renderer](../ui/runtime/ui.c), and
[simulator](../tools/simulator/runner.py).

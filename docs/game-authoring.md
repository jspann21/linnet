# Creating Linnet games

Games are native ARM `.LGM` files, loaded individually from `/.LINNET/GAMES`.
Photo, Video and Classic use separate target builds. Adding a game requires no
firmware registry entry or rebuild. There is no interpreter or general game engine.

## Start a project

Copy `sdk/templates/game` into your project:

```text
author.game/
  game.toml       # Identity, version, targets, capabilities, stack and schema
  README.md       # Gameplay, controls and preparation instructions
  LICENSE
  src/game.c      # Native callbacks and state
  assets/         # Source artwork, kept on the host
  levels/         # Source tables or authored levels
  tools/          # Optional author-run preparation scripts
```

Change the template's ID, author, license and description. IDs are permanent,
lowercase ASCII dot names such as `author.game`, at most 63 bytes. Versions and
device variants share an ID; forks use a new one. Built-in `linnet.*` IDs are
reserved. The catalogue holds at most 128 games and retains save slots after
removal.

From the Linnet checkout with development dependencies installed:

```powershell
python linnet games build --manifest path/to/game.toml --target video5g-m25 --output out/my-game
python linnet games inspect out/my-game/XXXXXXXX.LGM --target video5g-m25
python linnet games preview --manifest path/to/game.toml --target video5g-m25 --output out/my-preview
```

The report identifies the actual hash-named package. View the preview's
`index.html`; optional `--tape path.json` supplies simulator input events.
Preview compiles portable C for the desktop after validating its ARM build.
It cannot qualify ARM execution, caches, timing or storage. Preview saves live
only in memory, and music uses four sample titles.

## Interface and limits

Implement `ln_game_entry_point` using [game.h](../sdk/include/linnet/game.h).
Return every callback: start, input, tick, deadline, draw, playing, revision,
take_cue and stop. Use SDK input/result constants. MENU should return
`LN_GAME_EXIT`. Hold suspends input/ticks and the game clock. Deadlines use
wrapping microseconds; return `LN_GAME_NO_DEADLINE` when idle. Callbacks must
return promptly and never wait for I/O.

ABI 1/layout 1 reserves one 256 KiB slot including code, assets and zero-filled
state. Unloading does not return this reservation to the heap. BSS is omitted
from the file. Stack budget is at most 8 KiB. Builds require ARM-only soft-float
code and the SDK linker layout. Unresolved symbols, relocations, incompatible
CPU instructions and dynamic stack frames are rejected. Static frame reports
do not prove cumulative stack use; qualify call chains and assembly on hardware.
Keep private firmware addresses out of games.

Fonts and the clipped RGB565 canvas are borrowed. Use direct bounded loops or
inline bulk helpers in [draw.h](../sdk/include/linnet/draw.h). Retain no borrowed
framebuffer or service pointers beyond their lifetime. Declare
`capabilities = ["music"]` when needed and release sessions in stop.

Progress is limited to 256 bytes with a positive schema. Specify the saved schema
when reading; write only the current schema. Saves copy buffers
immediately. Success means accepted into the bounded queue, not durable yet.
Best scores and progress are bound to the permanent ID and survive removal.
Handle unavailable storage or a full queue.

## Prepare on the host

The build converts source images to ready-to-draw RGB565 or visible spans and
JSON integer tables to native arrays:

```toml
[[build.assets]]
name = "ship"
source = "assets/ship.png"
size = [24, 24]                  # Optional host resize
background = [0, 0, 0]          # Flatten partial alpha on host

[[build.tables]]
name = "level_cells"
source = "levels/cells.json"
type = "u8"                    # u8, u16 or s16
```

Include generated `game_assets.h`; use `ln_game_blit(canvas,x,y,&ship)` or the
native table array. Binary transparency needs no background. Optional `crop`
is `[x,y,width,height]`. The linker removes unused assets. Source assets,
scripts, symbols and reports stay on the host. Media Manager installs matching
prepared packages; players need no compiler. Installation never runs game scripts.

Precompute authored boards, text metrics, paths and lookup data where useful;
keep input-dependent gameplay native. Avoid per-frame allocation, source parsing,
asset conversion and per-pixel indirect calls. Measure worst-case frames, launch,
stack/RAM and music coexistence on hardware. Smaller files must not compromise
performance. Native games execute with firmware privileges; install trusted code.

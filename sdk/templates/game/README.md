# Wheel Counter

Turn the wheel to fill the screen. Menu exits. The iPod saves your highest value.

Copy this folder and change the permanent namespaced ID, author, license, title,
and supported targets in `game.toml`. Sources use only `sdk/include/linnet/`.
The same native source builds for each declared target; installation selects the
matching prepared executable. Generated output belongs under an ignored `out/`.
The copied template uses the included GPL-3.0-or-later `LICENSE`; keep its notice
with derived source and record the applicable license accurately in the manifest.

See [Creating Linnet games](../../../docs/game-authoring.md) for the format and
performance requirements. The package toolchain reports file, resident, and stack
usage separately; validate cumulative stack and timing on hardware.

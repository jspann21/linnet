# Native games

Each `linnet.*` folder is an independent target-specific game project. Its source
specializes the shared native implementation at compile time; section removal
keeps unrelated gameplay and artwork out of the resulting package. Base firmware
contains the loader and services, with no built-in game implementation.

Build the release library with `python linnet games build-library --target
video5g-m25 --output out/game-packages/video5g-m25`, substituting the desired target.
The output includes immutable `.LGM` files, `library.json`, and license notices.
For a new game, use the [authoring guide](../docs/game-authoring.md) and SDK template.

Word Search boards are authored and validated on the host. To regenerate them,
run `python games/linnet.word-search/tools/prepare.py` before building.

Original game code and assets follow the repository's
[GPL-3.0-or-later license](../LICENSE). The release library retains those notices.
Third-party exceptions retain their own terms: Solitaire court artwork is
adapted from Austin Gabriel's Public Domain Playing Cards (CC0), as credited in
the native artwork headers and generated release notice.

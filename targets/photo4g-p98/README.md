# iPod Photo A1099

The `photo4g-p98` target has a 220 × 176 color display. Its default firmware
includes the game loader; install game packages through Media Manager. Games
appear under **Extras → Games** by default. See
[hardware support](../../docs/hardware.md) for tested configurations and features.

## Games

Media Manager and firmware ZIPs include 18 built-in game packages. The controls
below cover three examples; see [Games](../../README.md#games) for the full list.

- **Comet Run:** turn the wheel or press Previous/Next to steer. Select spends
  40% energy on a shield; energy recharges during play. Three lives, increasing
  meteor density, and a saved best score.
- **Pulse Path:** watch the lights, then repeat the sequence with the wheel and
  Select. Shapes and clockwise numbers distinguish the four colored pads.
  Mistakes cost a life and replay the pattern; complete all 24 rounds to win.
- **Solitaire:** draw-one Klondike with seven columns and four foundations.
  Wheel chooses a pile, Previous/Next chooses a face-up card in a column, and
  Select picks up or places a card or sequence. Green outlines show legal
  destinations. The center preview shows held cards and compressed selections.
  Menu cancels a pickup. Play opens Resume, Undo last move, and New deal;
  up to 16 moves, draws, recycles, and reveals can be undone. New deals require
  confirmation. Completed games add to the saved win count.

In the arcade games, Play pauses/resumes and Menu returns to Games. Each game
has its own native Photo layout; text and controls are not scaled from the Video
screen. The game rules and audio live in `services/games/`; the
`presentations/photo220/` drawing code and artwork are selected by
`game_presentation`. Court interiors retain the CC0 artwork credited in
`../../services/games/presentations/photo220/solitaire_courts.h`, with ranks
and suits drawn separately for legibility.

Use **Games** in Linnet Media Manager while the iPod is in Disk Mode to select
installed games. Choose **Update games on iPod**, eject safely, and restart
Linnet to apply the selection.

Ordinary firmware reserves 7.75 MiB for the resident OS and its stacks, followed
by the separate 256 KiB game slot within the 8 MiB OS region. Cover Flow artwork,
update staging, diagnostic capture, and recovery have separate reservations.

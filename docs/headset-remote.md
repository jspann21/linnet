# Classic headset remote

The Classic OS supports the optional headphone-jack controller on the 2008
120 GB (6.5th generation) and 2009 thin 160 GB (7th generation) boards. It probes
the controller rather than using storage capacity to select a driver. An absent
or unresponsive controller leaves clickwheel input available. Boot and the NOR
monitor do not run this driver.

Apple three-button 3.5 mm headsets use these commands in music playback:

| Cord button | Command |
| --- | --- |
| Center, once | Play/pause |
| Center, twice | Next track |
| Center, three times | Previous track, or restart the current track |
| + / − | Change volume by 1 dB |
| Hold + / − | Repeat volume changes after half a second |

Center clicks are grouped with a 450 ms gap after release. Holding center has no shutdown or
voice-command action. Volume repeats stop after five seconds; release and press
again to continue. This bounds gain changes if a release event is lost. Video
uses center for play/pause and double/triple clicks for ten-second forward/back
seeks. Commands work in menus and with Hold locked. Unplugging discards pending
clicks and held keys. Microphone recording is separate work.

## Hardware interface

The controller interface is described in
[Mikey controller reverse engineering](https://terminalbytes.com/reverse-engineering-ipod-classic-mikey-chip/):
I2C bus 0, on-wire write address `0x72` (HAL seven-bit address `0x39`);
register 0 mode `0x2f` (reserved readback bits are masked);
register 4 center indications; register 5
volume edge pairs (up `0x04`/`0x08`, down `0x01`/`0x02`). Accessory identification
uses bits 4/5, not volume buttons. Unplugging resets the controller. Center
release timing can lag after inactivity. Linnet has no center hold commands.

The driver checks the mode bits with `(mode & 0x77) == 0x27` and reads bit 0
of register 4 as the center pressed level. Bit 2 can appear alone after release
(`0x84`), so Linnet ignores it and requires a released interval before another
press. The high idle markers are not buttons.

The driver samples every 20 ms in the foreground, with interrupts enabled.
Insertion has two settling intervals, and failed transfers clear all pending
commands and defer another probe for two seconds. It checks mode readback every
100 ms to catch reset/reconnection. Playback gain limits still apply.

## Development telemetry

Link `INPUT_STATE` retains its 16-word layout. Classic OS words 10–15 add:

Use `python linnet device input-state --serial <Linnet-USB-serial>` to read the
named counters and raw register values.
Media Manager's shareable hardware report includes driver availability,
controller presence, armed mode, headphone connection and bus failures. It
omits pressed keys and command history. A controller is probed when headphones
are connected; absence before that probe does not establish a hardware limit.

| Word | Meaning |
| --- | --- |
| 10 | `0x524d0000` signature; bits 0 present, 1 armed, 2 headphones, 3 bus fault, 4/5 up/down held, 6 center latched, 7 repeat limit; bits 8–10 last command (1 play/pause, 2 next, 4 previous), bits 11–12 pending click count |
| 11 | Registers 0, 4, 5, 6, in successive bytes |
| 12 | Successful samples |
| 13 | Failed transfers |
| 14 | Center rises accepted |
| 15 | Up presses in low 16 bits, down presses in high 16 bits; saturating |

Read these counters alongside playback state during physical qualification.
Compilation does not establish headset compatibility. Keep individual device
results in local artifacts, and qualify 2008 and 2009 hardware separately.

# Classic runtime power

Classic OS uses three CPU rates: 54 MHz for light work, 108 MHz for work that
needs more headroom, and the existing 216 MHz video boost. Boot and the resident
monitor retain their original clock policy. Hardware results belong in local
qualification artifacts; no battery-life percentage is inferred from clocks.

## Clock selection

The 54 MHz state enables only the CPU divide-by-two field in `0x3c500004`.
The normal source remains PLL0 at 108 MHz. AHB stays at 54 MHz and APB at 27 MHz.
The SDRAM timing, audio source, peripheral clocks, external timer and voltage
settings are preserved. Lower CPU frequency reduces its active clock rate;
it does not by itself establish an energy saving for a fixed amount of work.

The foreground policy requests 108 MHz during startup, storage transfers,
display work, user activity, media opening/seeking, and other busy services.
Music may enter 54 MHz with one second of PCM queued and returns to 108 MHz
below half a second. A slow foreground turn delays another low-clock attempt
for two seconds. An underrun disables low-clock playback for that player
generation. Failed clock transitions disable the optimization and restore the
normal clock; failure to restore stops further executive work.

The policy leaves video clock ownership with the video service. Handoffs restore
108 MHz so Boot and monitor code receive the required clock layout.

## Idle deadlines

The wheel wakes the OS by interrupt. Hold and headphone switches, including
headset remote sampling, have an explicit 20 ms deadline. Release OS may wait
up to that deadline rather than waking every 5 ms. Earlier audio, storage,
display, power and service deadlines still take precedence. Development Link
retains its 1 ms USB poll ceiling.

## Peripheral clocks

The SoC clock resource owner shares PLL2 between 32 kHz audio and the 216 MHz
CPU boost. The final consumer may release a PLL started by Linnet only after
both consumers have switched away. A validated PLL inherited already running
is preserved. Paused audio retains its codec clock and its audio lease so the
existing buffered pause/resume path remains available.

A full audio stop drains PCM, mutes the headphone and line amplifiers at maximum
attenuation, and powers down the codec. The Classic board then holds the
CS42L55 RESET pin low before stopping MCLK and gating I2S. The next cold start
restores clocks and performs the codec's initialization sequence. Removing
MCLK after a power-down register acknowledgement alone is insufficient: the
external codec must also enter reset. PMU rails remain available for headset
sensing. This follows the playback shutdown sequence in Cirrus
[CS42L55 DS773F1, section 4.11.1](https://media.digikey.com/pdf/Data%20Sheets/Cirrus%20Logic%20PDFs/CS42L55.pdf).

DMA0 is gated only with no enabled channels or pending completion/error bits,
after audio playback and display work have stopped. PCM and LCD transfer
entry points restore its clock before touching the controller.

Workload decisions belong in `targets/classic6g-n25/runtime_power.c`, board
wiring in that target's `board.c`, shared S5L clock mechanics in
`hal/s5l8702/clock_resources.c`, and codec register sequencing in
`hal/codecs/cs42l55.c`. Kernel services supply portable workload hints through
`hal/api/power_policy.h`.

## Evidence

`python linnet device classic-power-state --serial <Linnet-USB-serial>` reads
live clock registers, clock residency, queue headroom, transition failures,
fallback counters and peripheral clock gates. Residency includes WFI; it is
not active CPU time or a current measurement. Changes should be qualified
against idle/wake, playback formats, pause/resume, seeking, storage recovery,
video boost/restoration and headset controls on each hardware revision.
Digital silence and clean DMA counters do not establish analog silence;
headphone listening must check active output, full stop and restart separately.

Register facts are described in the public
[S5L clock register definitions](https://raw.githubusercontent.com/Rockbox/rockbox/master/firmware/export/s5l87xx.h)
and [clock-domain notes](https://raw.githubusercontent.com/Rockbox/rockbox/master/firmware/target/arm/s5l8702/clocking-s5l8702.h).
These hardware references inform Linnet's original power policy and
implementation.

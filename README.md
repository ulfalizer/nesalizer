# Nesalizer #

A work-in-progress NES emulator. Uses a low-level pixel-based renderer that
simulates the real PPU, going to through the motions of sprite evaluation and
pixel selection, and omits most prediction and catch-up for straightforward and
easy-to-debug code. This makes many effects that require special handling in
other emulators work automagically. The emulation thread peaks at around 37% of
one core on my two-year-old 2600K Core i7, but there's still room for
improvement.

Still lacks a GUI and save states, so not worth using yet from a user's
perspective. Will include some cool unique features later on. :)

## Building ##

Linux-only so far. Dependencies are <b>readline</b> and <b>SDL 2</b>. Build
with

    $ make CONF=release

See the <b>Makefile</b> for other options.

## Running ##

    $ ./nes <rom file>

Controls are currently hardcoded (in <b>input.cpp</b>) as follows:

<table>
  <tr><td>D-pad </td><td>Arrow keys </td></tr>
  <tr><td>A     </td><td>X          </td></tr>
  <tr><td>B     </td><td>Z          </td></tr>
  <tr><td>Start </td><td>Return     </td></tr>
  <tr><td>Select</td><td>Right shift</td></tr>
</table>

### Automatic testing ###

A set of test ROMs listed in <b>test.cpp</b> can be run automatically with

    $ make TEST=1
    $ ./nes

This requires https://github.com/christopherpow/nes-test-roms to first be
cloned into a directory called <i>tests</i>. All tests listed are expected to
pass.

## Compatibility ##

iNES mappers (support circuitry inside cartridges) supported so far: 0, 1, 2, 3, 4, 5 (including ExGrafix, split screen, and PRG RAM swapping), 7, 9, 11, 71, 232.

Supports tricky-to-emulate games like Mig-29 Soviet Fighter, Bee 52, Uchuu Keibitai SDF, Just Breed, and Battletoads.

<b>No PAL support yet.</b> PAL support will likely never be as good as NTSC support in any emulator due to much
trickier and less explored timing (plus you lose 10 FPS for games that also have an NTSC version). PAL ROMs can
often be recognized by having "(E)" in their name.

## Screenshots ##

### Bucky O'Hare ###

![Bucky O'Hare](https://raw.github.com/ulfalizer/nesalizer/screenshots/bucky.png)

### Battletoads ###

![Battletoads](https://raw.github.com/ulfalizer/nesalizer/screenshots/battletoads.png)

### Rad Racer 2 ###

![Rad Racer 2](https://raw.github.com/ulfalizer/nesalizer/screenshots/radracer2.png)

# Nesalizer #

A work-in-progress NES emulator. Uses a low-level pixel-based renderer that simulates the real PPU
and omits most prediction and catch-up for straightforward and easy-to-debug code. Still lacks a
GUI and save states, so not worth using yet from a user's perspective. Will include some cool
unique features later on. :)

## Building ##

Linux-only so far. Dependencies are <b>readline</b> and <b>SDL 1.2</b>. For Ubuntu, the following
should work:

    $ apt-get install libreadline-dev libsdl1.2-dev  
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

A set of test ROMs listed in <b>test.cpp</b> can be run automatically with

    $ make TEST=1  
    $ ./nes

This requires https://github.com/christopherpow/nes-test-roms to first be cloned into a directory
called <i>tests</i>.

## Compatibility ##

iNES mappers (support circuitry inside cartridges) supported so far: 0, 1, 2, 3, 4, 7, 9, 11, 71.

Supports tricky-to-emulate games like Mig-29 Soviet Fighter, Bee 52, and Battletoads.

No PAL support yet. PAL support will likely never be as good as NTSC support in any emulator due to much
trickier and less explored timing (plus you lose 10 FPS for games that also have an NTSC version).

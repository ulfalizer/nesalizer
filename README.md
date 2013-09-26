# Nesalizer #

A work-in-progress NES emulator. Uses a low-level pixel-based renderer that simulates the real PPU
and omits most prediction and catch-up for straightforward and easy-to-debug code. Still lacks a
GUI and save states, so not worth using yet from a user's perspective. Will include some cool
unique features later on. :)

## Building ##

Linux-only so far. Dependencies are <b>readline</b> and <b>SDL 1.2</b>. For Ubuntu, the following
should work:

> $ apt-get install libreadline-dev libsdl1.2-dev  
> $ make CONF=release

See the <b>Makefile</b> for other options.

## Running ##

> $ ./nes \<rom file\>

Controls are currently hardcoded (in <b>input.cpp</b>) to the arrow keys and <b>X</b> and <b>Z</b> for
<b>A</b> and <b>B</b>.

## iNES mappers (support circuitry inside cartridges) supported so far ##

0, 1, 2, 3, 4, 7, 9, 11, 71

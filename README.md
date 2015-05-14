Nesalizer
=========

A work-in-progress NES emulator with a real-time rewind feature that correctly reverses sound.

Some other cool features are planned :). Still lacks a GUI and persistent (on-disk) save states.

## Video demonstration ##

See [this video](https://www.youtube.com/watch?v=qCQkYrQo9fI) for a demonstration of rewinding. The channel has some other videos as well.

## Building ##

SDL2 is used for the final output and is the only dependency. You currently need a \*nix system.

The only \*nix/POSIX dependencies are the timing functions in [**src/timing.cpp**](src/timing.cpp), which should be trivial to port. A quick-and-dirty experimental port to Windows has already been done by miker00lz, but contributions are welcome. One GCC extension (case ranges) is used currently.

Commands for building on Ubuntu:

    $ apt-get install libsdl2-dev
    $ make CONF=release
    
Parallel builds (e.g., `make CONF=release -j8`) are supported too.

See the *Makefile* for other options. The built-in movie recording support has sadly bitrotted due to libav changes.

## Running ##

    $ ./nes <ROM file>

Controls are currently hardcoded (in [**src/input.cpp**](src/input.cpp) and [**src/sdl_backend.cpp**](src/sdl_backend.cpp)) as follows:

<table>
  <tr><td>D-pad       </td><td>Arrow keys   </td></tr>
  <tr><td>A           </td><td>X            </td></tr>
  <tr><td>B           </td><td>Z            </td></tr>
  <tr><td>Start       </td><td>Return       </td></tr>
  <tr><td>Select      </td><td>Right shift  </td></tr>
  <tr><td>Rewind      </td><td>R (hold down)</td></tr>
  <tr><td>Save state  </td><td>S            </td></tr>
  <tr><td>Load state  </td><td>L            </td></tr>
  <tr><td>(Soft) reset</td><td>F5           </td></tr>
</table>

The save state is in-memory and not saved to disk yet.

## Technical ##

Uses a low-level renderer that simulates the rendering pipeline in the real PPU (NES graphics processor), following the model in [this timing diagram](http://wiki.nesdev.com/w/images/d/d1/Ntsc_timing.png) that I put together with help from the NesDev community. (It won't make much sense without some prior knowledge of how graphics work on the NES. :)

Most prediction and catch-up (two popular emulator optimization techniques) is omitted in favor of straightforward and robust code. This makes many effects that require special handling in some other emulators work automagically. The emulator currently manages about 6x emulation speed on a single core on my old 2600K Core i7 CPU.

The current state is appended to a ring buffer once per frame. During rewinding, states are loaded in the reverse order from the buffer. Individual frames still run "forwards" during rewinding, but audio is added in reverse from the end of the audio buffer instead of from the beginning. Getting things to line up properly at frame boundaries requires some care.

A thirty-minute rewind buffer uses around 1.3 GB of memory for most games. There's no attempt to compress states yet, so a lot of memory is wasted. The length of the rewind buffer can be set by changing *rewind_seconds* in [**src/save\_states.cpp**](src/save_states.cpp) and rebuilding.

## Compatibility ##

iNES mappers (support circuitry inside cartridges) supported so far: 0, 1, 2, 3, 4, 5 (including ExGrafix, split screen, and PRG RAM swapping), 7, 9, 10, 11, 13, 28, 71, 232. This covers the majority of ROMs.

Supports tricky-to-emulate games like Mig-29 Soviet Fighter, Bee 52, Uchuu Keibitai SDF, Just Breed, and Battletoads.

Supports both PAL and NTSC. NTSC ROMs are recommended due to 10 extra FPS and PAL conversions often being half-assed. PAL roms can usually be recognized from having "(E)" in their name. (This is often the only way for the emulator to detect them without using a database, as very few ROMs specify the TV system in the header.)

## Coding style ##

For functions and variables with external linkage, the documentation appears at the declaration in the header. For stuff with internal linkage, the documentation is in the source file. The headers start with a short blurb.

The source is mostly C-like C++, but still strives for modularization, implementation hiding, and clean interfaces. Internal linkage is used for "private" data. Classes might be used for general-purpose objects with multiple instances, but there aren't any of those yet. I try to reduce clutter and boilerplate code.

All .cpp files include headers according to this scheme:

    #include "common.h"
    
    <#includes for local headers>
    
    <#includes for system headers>

*common.h* includes general utility functions and types as well as common system headers. Nesalizer still builds very quickly, so over-inclusion of system headers isn't a huge deal.

The above setup allows most headers to assume that common.h has been included, which simplifies headers and often makes include guards redundant.

If you spot stuff that can be improved (or sucks), tell me (or contribute :). I appreciate reports for small nits too.

## Automatic testing ##

A set of test ROMs listed in *test.cpp* can be run automatically with

    $ make TEST=1
    $ ./nes

This requires https://github.com/christopherpow/nes-test-roms to first be cloned into a directory called *tests*. All tests listed in *test.cpp* are expected to pass.

## Thanks ##

 * Shay Green (blargg) for the [blip\_buf](https://code.google.com/p/blip-buf/) waveform synthesis library and lots of test ROMs.

 * Quietust for the Visual 2A03 and Visual 2C02 simulators.

 * beannaich, cpow, Disch, kevtris, Movax21, Lidnariq, loopy, thefox, Tepples, and lots of other people on the [NesDev](http://nesdev.com) forums and wiki and in #nesdev for help, docs, tests, and discussion.

## Contact ##

moc.liamg[ta]rezilaflu in reverse. I'm currently [looking for a job](https://linkedin.com/in/magnussonulf), so feel free to e-mail regarding that too. :)

## Other stuff ##

A tutorial I wrote (with help from Lidnariq and Quietust) on reading circuits in Visual 6502 and other simulators based on the same framework can be found [here](http://wiki.nesdev.com/w/index.php/Visual_circuit_tutorial). Some obscure behaviors were reverse-engineered from studying circuits and running simulations.

## Screenshots ##

(Not sure why I took these with linear filtering enabled. It's a bit ugly in retrospect.)

### Bucky O'Hare ###

![Bucky O'Hare](https://raw.github.com/ulfalizer/nesalizer/screenshots/bucky.png)

### Battletoads ###

![Battletoads](https://raw.github.com/ulfalizer/nesalizer/screenshots/battletoads.png)

### Castlevania III ###

![Castlevania III](https://raw.github.com/ulfalizer/nesalizer/screenshots/cv3.png)

### Rad Racer 2 ###

![Rad Racer 2](https://raw.github.com/ulfalizer/nesalizer/screenshots/radracer2.png)

## License ##

[GPLv2](http://www.gnu.org/licenses/gpl-2.0.html)

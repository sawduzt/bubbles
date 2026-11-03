# bubbles

A Windows screensaver. It is the stock Bubbles saver cloned bubble for bubble, except the
physics know about your real monitors instead of treating the desktop as one big
rectangle, so on an offset or L shaped setup nothing drifts through the dead space.

![screenshot](docs/screenshot.jpg)

## what it does

- L shaped and offset monitor layouts, including the little preview pane in the Windows
  screen saver settings
- a settings page with toggles: size, film, merge, depth, wobble, pop, iridescent, bloom,
  mood, backdrop, droplets, hero
- merging bubbles actually fuse with a filleted join instead of showing a flat shared wall
- bubbles eject from the corner, collide, slosh in local puffs, never fade
- the same exe is also a windowed play app, see below

## install

1. download `bubbles.scr`
2. right click it and pick Install, or drop it into `C:\Windows\System32`
3. windows will warn you because the exe is unsigned. more info, run anyway.
4. settings, personalisation, lock screen, screen saver, pick Bubbles

Settings and toggles live in HKCU, nothing else on the machine is touched. The settings
page is also reachable from the screen saver list's Settings button, or by running the file
with `/c`.

## the play app

Same binary. Rename it to anything with `play` in the name, or run it with `/w`, and it
opens as a normal window with a field of bubbles to mess with. Click pops one, drag shoves
the field around, right click spawns at the cursor, scroll on a bubble resizes it. F2 opens
the settings.

## build

Needs mingw-w64 (`g++` and `windres`) on PATH. Then either run `build.bat` or:

```
windres src/resource.rc -O coff -o build/resource.o -I src
g++ -O2 -municode -mwindows -static -o build/bubbles.scr src/bubbles.cpp build/resource.o -lwinmm -lgdi32 -luser32 -ld3d9
```

## how it works

D3D9 presentation with a GDI path kept alongside it, a bubble sprite atlas baked from
`src/sprite.bin` (regenerate it with `tools/make_sprite.py`), and dead space rectangles
derived from the real monitor union rather than the virtual desktop bounding box.

Command line, in case you want to poke at it:

- `/s` run the saver
- `/p <hwnd>` render into a preview pane
- `/c` settings page
- `/w` windowed play mode
- `/t <dir> <seconds>` render test frames to bmp files with no window at all

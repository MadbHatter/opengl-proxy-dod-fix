# Darkest of Days: black screen fix for modern AMD GPUs

On AMD cards from GCN onwards (tested on an RX 7900 XTX, driver 32.0.31041.1004),
Darkest of Days shows the menu fine but turns black with audio as soon as a
level starts. This repository has the root cause and a drop-in fix.

## Install

1. Download `dist/opengl32.dll` (or build it, see below).
2. Copy it next to `darkestofdays.exe`.
3. Play. Nothing in the game is modified, so it should work with any exe
   version (the prebuilt DLL was tested in-game on the unpatched retail 1.0 install with an RX 7900 XTX).

To uninstall, delete `opengl32.dll` from the game folder. The fix writes
`dod_glfix.log` next to the exe so you can confirm it is active.

## Cause

At level start the game renders a lighting pass like this:

```
glPushAttrib(GL_POLYGON_BIT | GL_ENABLE_BIT | GL_SCISSOR_BIT);
glEnable(GL_DEPTH_BOUNDS_TEST_EXT);
glDepthBoundsEXT(0.99944, 0.99948);
... draw ...
glPopAttrib();
```

The [EXT_depth_bounds_test](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_depth_bounds_test.txt)
spec puts the enable in the `enable` and `depth-buffer` attribute groups (and the bounds in
`depth-buffer`), so `glPopAttrib` must switch the test off again. AMD's current
OpenGL driver does not: blend and scissor are restored, the depth bounds test
stays on with the tiny range. The game never disables it explicitly, so every
later fragment is tested against a stored depth of 1.0 (the clear value), fails,
and nothing is drawn for the rest of the level.

The old TeraScale driver branch (HD 2000 to HD 6000) handled this correctly,
which is why the game worked on those cards.

How this was found: an apitrace capture of the black screen replays black on
the same card; all shaders compile and link, state dumps around draws look sane,
but the depth buffer stays at 1.0 even after the colour-masked depth pre-pass.
Bisecting the GL state showed `GL_DEPTH_BOUNDS_TEST_EXT` still enabled after the
matching `glPopAttrib`. A standalone 30-line push, enable, pop test reproduces it
on the driver for `GL_ENABLE_BIT`, `GL_DEPTH_BUFFER_BIT` and `GL_ALL_ATTRIB_BITS`.

## Fix

`glfix/` builds a proxy `opengl32.dll`. All 368 exports forward to the system
`opengl32.dll`. Only `glPushAttrib`/`glPopAttrib` are wrapped: on push it saves
the depth bounds enable and range, and after the real pop it restores them.
`glNewList`/`glEndList` are watched so pushes compiled into display lists don't
desync the shadow stack.

Set `DOD_GLFIX_REAL_OPENGL32` to another `opengl32.dll` path to chain the fix
in front of a different wrapper (for example apitrace).

## Build

```
sudo apt install gcc-mingw-w64-i686   # or MSYS2 mingw-w64-i686-gcc
glfix/build.sh                          # -> glfix/build/opengl32.dll
```

The game is 32-bit, so the DLL must be 32-bit.

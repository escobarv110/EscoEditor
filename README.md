# EscoEditor

A standalone plugin for **GTA V's Rockstar Editor** — the one in FiveM and in
singleplayer. It adds the things the editor is missing when you are actually
shooting something: a free camera you can slow right down, keyframes you can
copy between markers, time of day and weather per clip, and **scene lights you
place in the picture with a 3D gizmo and key over the clip**.

No ScriptHookV, no Rockstar Editor+, no Menyoo. One `.asi`, and it runs
alongside Rockstar Editor+ if you use that too.

<sub>Version 4.10 · GTA V b3258–b3751 and singleplayer b3407 · MIT</sub>

---

## What it does

**Camera**

* **Camera speed** from 5% to 1000% of the stock free camera — the whole point:
  the stock camera is far too fast for a close pass over a car, and there is no
  setting for it. Separate speeds for up/down and for looking, and the
  acceleration ramp scales with the speed so slow moves start slowly.
* **Free Look** (`K`) — fly the camera at a keyframe without changing that
  keyframe's camera, then switch back to the camera the clip will render. For
  scouting an angle, or placing lights from somewhere else in the scene.
* Hold **Shift** for ×3, **Alt** for ×0.25 while flying; `F` / `G` / `0` step
  the speed, and the mouse wheel can too.

**Keyframes**

* **Copy / Cut / Paste** a range of keyframes (markers) — everything on them:
  camera, DOF, time, weather, speed.
* **Copy / Paste / To All** for the whole Depth of Field block of a keyframe.
* **Speed To All** - the speed of the keyframe whose menu is open goes onto
  every keyframe of the clip, so a whole sequence is set from one place.

**ENB depth of field**

* With **ENBSeries** in the game (its `d3d11.dll` exports an SDK) and ENB's
  `EnableDepthOfField` on, the **ENB** switch hands ENB's `enbdepthoffield.fx`
  the Rockstar Editor's own depth of field, keyframe by keyframe: a Custom
  keyframe's Focal Distance is where ENB focuses (in metres — converted to
  NVE's own unit), its Intensity decides how quickly the blur comes in
  (100% = NVE's own aperture), None means no blur, Default leaves ENB's focus
  alone — blended from one keyframe to the next.
* **Every ENB DOF option, per keyframe**, right in the keyframe's own Depth of
  Field menu: one row each for every option the running NVE technique reads
  (blur size, maximum blur, near field, chromatic spread, anamorphic, lens
  distortion, quality…). The list scrolls past the column's 16 rows, and
  holding left/right speeds up like the game's own sliders. Each option
  blends between the keyframes that set it.
* Values go in from ENB's own per-frame callback, only when they change, and
  ENB's own values go back when the switch is off and before ENB saves, so its
  `.ini` keeps yours.

**Scene lights** — the big one

* Place **point and spot lights** in a clip, with a 3D gizmo in the picture:
  move, rotate, scale.
* **Keyframe them** over the clip — colour, intensity, range, falloff, cone,
  volume, position and direction all interpolate between keys.
* **Attach** a light to the camera, or to a **ped, vehicle or prop** in the
  scene — pick it in the picture or from a list of what is around. It follows
  through the whole clip, and holds its place when the replay drops what it
  follows out of the scene.
* **Lens flares and a light leak** per light (through ReShade — see below):
  glow, anamorphic streak, star, ghosts, halo, colour fringe, leak, flicker,
  with presets.
* Switches per light: shadows, sharper shadow, volumetric, no reflections, lit
  in a blackout, and **From the sun** — the engine works the light's colour and
  brightness out from the sun at the clip's hour, so it follows sunrise, noon
  and dusk on its own.
* **Time of day and weather** per clip, in the light editor or on the menu —
  and **keyed over the clip** like a light if you want: the time runs in
  minutes (taking the short way round midnight) and the weather uses the
  game's own old/new/blend packet fields, so it crossfades rather than
  stepping.
* Lights are saved per project and clip in `EscoEditor.lights.txt`, and
  **"Same lights in every clip"** or **"Copy from"** move a rig between clips
  and projects.
* The lights, and the keyed sky, are in an export - the game's own Export
  button and Rockstar Editor Plus's exporter alike. The list is rebuilt by
  the light consumer itself when nothing else has, so it does not depend on
  the replay director ticking between encoded frames.

Everything is on a page of the **EscoEditor** row at the bottom of every
marker's menu (keyboard only, like the rest of the editor), and the light
editor is a window you open with `L`.

## Requirements

* **FiveM** or GTA V singleplayer (an ASI loader — ScriptHookV's `dinput8.dll`
  or any other — for singleplayer).
* For the **light editor window** and the **lens flares**: ReShade 6.3 or newer
  **with add-on support** (the build that loads `.addon64` files). The lights
  themselves work without it; only the window and the flares need it.

## Install

**FiveM** — close FiveM, then run `Install.bat`, or copy
`EscoEditor.asi` and `EscoFlare.fx` into

```
%LOCALAPPDATA%\FiveM\FiveM.app\plugins\
```

**Singleplayer** — copy `EscoEditor.asi` and `EscoFlare.fx` next to `GTA5.exe`.

Then open the Rockstar Editor, open any marker's menu, and the last row is
**EscoEditor**. Settings live in `EscoEditor.ini` next to the `.asi`; what
happened lives in `EscoEditor.log` beside it.

## Alongside Rockstar Editor+

Both can be installed together, and EscoEditor does the work to make that true.

RE+ needs four addresses — `UpdateSmoothing`, the marker-index function, the
marker storage and the replay clock — and installs **nothing** unless it finds
all four, by their first bytes. EscoEditor hooks `UpdateSmoothing`, so a jump
sitting there is enough to keep the whole of RE+ dormant. So: EscoEditor waits
for RE+ when it is already loaded, and when RE+ turns up later it takes **all**
of its own hooks out for twelve seconds — RE+ retries every two seconds, finds
the game's own bytes, and installs in full — then rebuilds each hook on top of
whatever RE+ left behind, so both run. RE+ is recognised however its file has
been renamed.

The free camera belongs to EscoEditor when both are loaded. RE+ rewrites the
camera's response blocks every frame from its own tick, so EscoEditor writes
**Look Speed, Response and the mouse multiplier again after it**, once a frame,
and the movement speed correction is measured against whatever `MaxSpeed`
currently holds — so whatever RE+ puts there is scaled away and the camera moves
at the percentage on EscoEditor's row. RE+'s own camera speed rows and keys will
look like they do nothing; that is the arrangement, not a fault. Everything else
runs side by side, and the scene lights of both plugins are drawn together.

## Build from source

Visual Studio 2026 with the C++ workload (or any MSVC that has
`vcvars64.bat` — edit the path at the top of `build.bat`).

```bat
build.bat                     :: -> dist\EscoEditor.asi
build\selftest\selftest.bat   :: builds the self-test
build\selftest\selftest.exe            :: a cameras.ymt may be passed, but is not needed
```

The self-test is 172 checks over the parts that can run outside the game: the
metadata parser, the light record, keyframe interpolation, the light file
format, the entity pools, the marker clipboard, Free Look's snapshots, MinHook
itself. It prints `SELFTEST: all checks passed` or the first thing that broke.

### How the source is laid out

One translation unit — `src/main.cpp` includes the `.inc` files in order:

| file | what is in it |
| --- | --- |
| `ecs_core.inc` | log, ini, pattern scanning, presets, the camera metadata block |
| `ecs_game.inc` | the marker record and marker storage, quaternions, hooking |
| `ecs_clipboard.inc` | the keyframe clipboard |
| `ecs_camfree.inc` | Free Look |
| `ecs_scene.inc` | time of day and weather |
| `ecs_dof.inc` | depth of field copy / paste / to all |
| `ecs_lights.inc` | the lights themselves: store, keyframes, records, pools |
| `ecs_enbdof.inc` | the ENB option: the editor's DOF drawn by ENB through its SDK |
| `ecs_lightui.inc` | the light editor window and the 3D gizmo |
| `ecs_reshade.inc` | registering as a ReShade add-on; drawing the flares |
| `ecs_menu.inc` | the rows in the marker menu |
| `ecs_director.inc` | the per-frame hook on the replay director |
| `ecs_worker.inc` | the worker thread |

`dist/EscoFlare.fx` is the flare shader. `tools/escodump` is a small read-only
plugin that writes a decrypted copy of the game's module out, for working out
byte patterns on a new build.

Everything the plugin does to the game is a pattern scan plus a MinHook hook;
there are no hard-coded addresses, which is why the same binary works across
several builds.

## Licence

MIT — do what you like with it, including shipping it in your own thing. See
[LICENSE](LICENSE). MinHook (`src/minhook`) is BSD-2 by Tsuda Kageyu.

## Credits

Written by **Escobar**, with Claude (Anthropic) doing the typing and a lot of
the reverse engineering. Thanks to **crosire** for ReShade and its add-on API,
to **Tsuda Kageyu** for MinHook, and to the Rockstar Editor community whose
notes on the replay system saved weeks.

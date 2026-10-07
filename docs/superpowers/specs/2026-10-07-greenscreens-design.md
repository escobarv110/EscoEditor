# Greenscreens in the world - design

Date: 2026-10-07 - EscoEditor 4.25 - status: built (see "As built")

## Goal

Chroma screens placed in the world of a replay clip, the way scene lights are:
they stay where they are put while the camera moves, people and cars in front
of them stay visible, and their colour is flat and exact in every light, time
of day and weather - so the footage keys cleanly in an editor afterwards.
ReShade / ENB "greenscreens" are screen-space and move with the view; these do
not.

They must appear in the editor, in playback, in Rockstar Editor+ renders and
in Extended Video Export (EVE) exports. The game's own export without EVE
captures before ReShade and will not show them - accepted, stated in the
README.

## What the user gets

- A **Screens** section in the light editor beside Lights: add, select,
  delete, rename, enable, the same 3D gizmo (move / rotate / scale), the same
  keyframe list and the same "follows" (world, camera, ped, vehicle, prop).
- Per screen:
  - **Shape**: Flat, Curved, Cyc.
    - Flat: a rectangle, width x height.
    - Curved: the rectangle bent sideways around a vertical axis; Curve =
      the arc it spans in degrees (0 = flat, up to 180).
    - Cyc: a wall of width x height, a floor of the same width reaching
      Floor Depth forward, joined by a quarter-round corner of Corner Radius.
  - **Colour**: Green (0, 177, 64), Blue (0, 71, 187) or any colour. Flat,
    unlit, not touched by the game's light, fog or exposure.
  - **Markers**: Off, Crosses, Dots; Spacing (m), Size (m), and a colour
    that defaults to a slightly darker shade of the screen colour.
- Everything numeric (position, rotation, size, curve, floor, corner, colour)
  can be keyed over the clip and blends between keys, as lights do. Shape,
  markers on/off and "follows" belong to the screen, not to a key.
- Up to 8 screens per clip.

## How it works

### Data (ecs_lights.inc)

`Screen` next to `Light` in each clip's `LightSet`:

```
struct ScreenParams {
    float pos[3];                 // centre of the wall's bottom edge
    float yaw, pitch, roll;       // degrees; forward = the side that is seen
    float width, height;          // metres
    float curve;                  // degrees of arc (Curved)
    float floorDepth, corner;     // metres (Cyc)
    float col[3];                 // 0..1
    float markSpacing, markSize;  // metres
    float markCol[3];             // 0..1
};
struct Screen { id, name, enabled, shape, markers, Attach at, ScreenParams base,
                nkeys, ScreenKey keys[64], runtime: ent, entSlot, parent, ... };
```

Evaluation, attach and keyframe insertion reuse the lights' machinery (the
same blend - angles blend the short way round; colour linear).

### File (EscoEditor.lights.txt)

New lines inside a clip's `scope`, ignored by older versions:

```
scr <id> <enabled> <shape> <markers> <name>
scra <mode> <kind> 0x<model> <x> <y> <z>      (only when it follows something)
scrb <ScreenParams floats>
scrk <t> <ScreenParams floats>
```

"Lights In Every Clip" shares screens like lights.

### Drawing (EscoFlare.fx + ecs_reshade.inc)

The existing EscoFlare pass gains screens before the flares: per pixel it builds
the view ray from the camera basis and field of view (new uniforms), takes the
scene's distance from the depth buffer (EE_Depth), and intersects the ray with
each screen in the screen's own frame:

- Flat: ray against the plane, inside the rectangle.
- Curved: ray against the cylinder whose arc of `curve` degrees has chord
  `width`, inside the arc and the height.
- Cyc: the nearest of wall plane, floor plane and the corner quarter-cylinder,
  each within its part.

A hit nearer than the scene paints the pixel the screen's colour, with markers
from the hit point's surface coordinates. Screens are sent camera-relative (the
CPU subtracts the camera position) so float precision holds anywhere on the
map. The same intersection code exists in C++ for picking with the mouse and
for the self-test.

The pass is drawn where the flares are drawn now (before the game's UI, so the
editor's menus stay on top), whenever a replay is open and a screen or flare is
in view.

### Exports

- RE+ diverts Export to a preview and captures at Present: screens are drawn as
  in playback.
- EVE: the game's Export (BAKE) is running. 4.23 stopped all ReShade work then,
  because EscoFocus (a pass writing its own texture and read back on the CPU)
  turned EVE's video black. Screens draw onto the picture, which is a different
  case; `ScreensInExport=1` (default) lets the EscoFlare pass draw during an
  Export, EscoFocus stays off. `ScreensInExport=0` restores 4.23. The log says
  which happened. The user's first EVE export is the real test.

### Light editor (ecs_lightui.inc)

- A Lights / Screens switch at the top of the editor's list; the gizmo, the
  keyframe list and "follows" work on whatever is selected.
- Rotate: the three rings set yaw (vertical axis), pitch and roll.
- Scale: width and height (and floor depth for a Cyc) by dragging the
  handles; numbers in the panel as well.
- Click in the view selects the screen under the cursor (C++ intersection).

## Errors and limits

- More than 8 screens in view: the 8 nearest are drawn; the log says so once.
- No depth buffer (ReShade's depth not found): screens draw over everything;
  the editor shows a one-line warning.
- EscoFlare.fx is the plugin's own built-in copy (4.17), so the new pass
  cannot be missing.

## Testing

Self-test (build/selftest):
- Flat, Curved and Cyc hit tests: centre, edges, outside, behind, grazing.
- Picking returns the nearest screen.
- Save / load round trip of screens, keys and attach; an old file without
  screens still loads; a file with screens loads in a parser that skips them.
- Key blending: positions linear, angles the short way round, colours linear.

In game (user): place one of each shape, check occlusion by a ped in front,
check colour under night/day, one RE+ render, one EVE export.

## Not doing

Lighting or shadows on screens, spill or reflections, screens in the game's own
export without EVE, more than 8 screens.

## As built (4.25)

A screen became a third kind of light (`T_SCREEN`) instead of a separate
`Screen` struct, so keys, blending, attaching, Duplicate, "Same lights in
every clip" and the gizmo work for it unchanged. It keeps a light's `Params`:
width = intensity, height = range, curve = falloff, floor depth = inner,
corner = outer, marker spacing = volInt, marker size = volSize, colour = col,
facing = dir, Scale sizes everything. Shape and markers sit in its flags
(bits 8-9 and 10-11). Differences from the design above:

- No roll: a screen is turned by yaw and pitch of its facing; up is the
  world's (or what it follows).
- Markers are a fixed darker shade (0.55 x) of the screen's colour.
- One list: screens sit in the light list ("screen" beside "spot"/"point"),
  with an "Add screen" button, not a Lights / Screens switch.
- File: `screen <id> <on> 0x<flags> <name>`, then `sa` / `sb` / `sk` lines in
  the light layout - every line starts with "s", which builds before 4.25
  skip.
- `ScreensInExport=1` lets the whole EscoFlare pass (screens and flares) draw
  during the game's Export; EscoFocus stays off then.

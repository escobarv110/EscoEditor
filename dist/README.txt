EscoEditor.asi 4.2  -  more control for the Rockstar Editor
======================================================================
(formerly EditorCamSpeed.asi - same plugin, new name)

Standalone plugin: no ScriptHookV, no Rockstar Editor+, no Menyoo. It adds
an "EscoEditor" row at the bottom of every marker's menu in the editor,
takes over the Speed row, adds Copy / Paste / To All to the Depth of Field
menu, adds scene lights you place in the picture with a 3D gizmo - fixed in
the world, on the camera, or riding along with a ped, a vehicle or a prop -
and keeps the free camera speed keys. The editor's own camera path,
transitions, shake and depth of field are never touched.

WHAT CHANGED IN 4.2
  - Both plugins now really do work together. Rockstar Editor Plus rewrites
    the free camera's response blocks every frame from its own tick, so the
    Look Speed, Response and mouse settings on EscoEditor's rows were being
    overwritten and did nothing. They are now written again after RE+'s tick,
    once a frame, so the numbers on the rows are the ones the frame uses.
    (The movement speed was already ours: the correction is measured against
    whatever MaxSpeed currently holds, so whatever RE+ writes there is scaled
    away.)
  - The log says what the arrangement is when both are loaded, and warns when
    the marker menu column is full - the game draws 16 rows and no more, and
    two plugins adding rows can reach that.

WHAT CHANGED IN 4.1
  - "Speed To All" on the Copy & Paste page: the speed of the keyframe whose
    menu is open goes onto every keyframe of the clip. Set one keyframe to the
    speed you want, then this row puts it on the whole sequence. The row's
    help line says which speed it will use and what it did.
  - The marker Speed row in 5% steps is gone. It extended the game's nine-value
    speed index and hooked the playback's speed lookup to answer for the extra
    values, and it did not work in practice. The Speed row is the game's own
    again, with its nine steps. ClipSpeed is removed from the ini.

4.0 WAS THE FIRST PUBLIC RELEASE
  Everything below is the history of how it got here, newest first. The source
  is at https://github.com/escobarv110/EscoEditor (MIT).

WHAT CHANGED IN 3.21
  - An attached light no longer flickers from a distance. The replay only
    keeps entities alive around the camera, so the car (or ped, or prop) a
    light follows disappears from the game's pools when the camera is far
    away - and the light, and its lens flare, were dropped on every frame
    that happened. It now holds the last place it saw its parent, so the
    light stays exactly where it was until the car comes back. The log counts
    those frames ("an attached light held its last place in N frame(s)").
  - A light also keeps hold of the same car when the replay moves it to
    another pool slot, instead of re-picking the nearest car of that model
    and jumping between two of them.

WHAT CHANGED IN 3.20
  - The flicker line now counts the frames the game DREW next to the times it
    asked for its lights. If the asks are fewer than the frames, the light is
    simply not in every frame - which is the flicker, and it is then the
    plugin's to fix. If they match, every frame had the light in it and the
    engine is dropping it later, which is a different repair. One line says
    which:
      "the game drew 120 frame(s) and asked for its lights 57 time(s) ...
       FEWER ASKS THAN FRAMES: the light is only in some of the frames"

WHAT CHANGED IN 3.19
  - A vehicle pool layout that was guessed now has to show eight entities
    before it is believed, and whatever is settled on is re-checked every ten
    seconds: if it stops holding vehicles it is thrown away and the search
    starts again. On 3.18 the search ran while a clip was still loading, when
    the world had no cars in it yet, and a two-entity guess (count +0x14,
    bits +0x10 - not a pool at all) was allowed to win and then stuck for the
    session.
  - The log writes one of the game's own scene light records next to one of
    ours, once, for comparing them field by field. Everything of ours goes
    into the game's list every frame - the tally proves that - so a light that
    blinks at a distance is the engine choosing not to draw it, and the
    difference has to be in the record.

WHAT CHANGED IN 3.18
  - Vehicles, properly this time. 3.13 read 256 pool slots looking for cars
    and found 48; when that search moved to the worker thread in 3.14 the
    sample was cut to 16 - 48 slots to keep it cheap, and since the replay
    puts its cars in whatever slots happen to be free, it kept landing in an
    empty stretch and reporting none. It now reads 512 slots of each shape
    the game really uses (and the whole pool before trusting a guessed one),
    and it starts as soon as the replay is running rather than waiting for a
    marker menu.

WHAT CHANGED IN 3.17
  - Vehicles are back. 3.14 moved the pool search onto the plugin's worker
    thread, where it started two seconds after the game did - at the loading
    screen, with no vehicles in the world at all - used up its attempts there
    and gave up for the session. It now searches only while a clip is open in
    the editor, every five seconds, and keeps at it until it finds the pool.
  - "Copy from" in the light editor: a picker of every project and clip
    EscoEditor.lights.txt holds, with its light count, and a Copy here button.
    Lights are kept per project AND clip, so a lights file copied from another
    machine - or a project that has been renamed - loads but shows nothing
    until its rig is brought over. This is how to bring it over, keeping the
    keyframes, flares and what each light follows.

WHAT CHANGED IN 3.16
  - A light that flickers now says why. Every two seconds, while a clip is
    open, the log records how often the game asked for its scene lights, how
    many of ours went in, how many the game's list refused, how many frames
    used a stale snapshot, and how full the game's own list was:
      "lights: in the last two seconds the game asked for its lights 118
       time(s); 118 of 118 went in, 0 refused ..."
    A refusal means the game's light list was full - the usual reason a light
    blinks when the camera is far away and steadies as it comes closer, since
    a distant view has far more of the map's own lights in it. The line is
    only written when the picture changes, so it does not fill the log.

WHAT CHANGED IN 3.15
  - Attaching a light to a vehicle no longer crashes the game. The frame after
    a light was put on a car, the code that checks "is this still the same
    entity?" worked the pool slot out by dividing the pointer by the pool's
    item size - and the vehicle pool, being an array of pointers, has an item
    size of zero. That divide by zero was the crash FiveM reported as
    "escoeditor.asi+18024". The slot an entity was found in is remembered
    instead, which needs no arithmetic and works for every kind of pool.

WHAT CHANGED IN 3.14
  - 3.13 found the vehicle pool and then froze the game: the search walks
    hundreds of candidate layouts, most of its reads land on unmapped
    addresses, and every one of those costs a trip through the kernel. Run
    from inside the replay director's frame that is a freeze, and when a clip
    had no cars nearby it searched again on the very next frame, for ever.
    The search now runs on the plugin's own worker thread, one attempt every
    15 s and eight at most; the game thread only ever uses a layout already
    settled on. The layout GTA V really uses is tried first, so the normal
    case costs a handful of reads, and a winner has to show at least two
    entities standing inside the map.

WHAT CHANGED IN 3.13
  - The vehicle pool's layout is worked out in the game instead of assumed:
    every candidate - one or two pointer hops, the slots read as pointers or
    as objects in a row, and the capacity and in-use bits at each plausible
    offset - is tried once, and the one that yields entities with a readable
    model and world matrix is kept. The log names it:
      "lights: the vehicle pool reads as an array of pointers - 1 hop(s),
       array +0x0, count +0x8, in-use bits +0x30: 14 vehicle(s) in it"
    If none of them works it writes what the global points at, sixteen qwords
    of it, which is what I need to place the fields by hand - send that line.

WHAT CHANGED IN 3.12
  - Vehicles can be attached to. The scene list showed peds and props but
    never a vehicle: GTA V's vehicle pool is an array of pointers, and it was
    being read as a run of entities one after another, so every slot came out
    as rubbish. Both readings are now tried and the one that yields real
    entities is kept - the log says which ("the vehicle pool reads as ...").

WHAT CHANGED IN 3.11
  - Same lights in every clip: tick it in the light editor (or the "Lights In
    Every Clip" row on the Lights page) and this clip's lights become the
    whole project's. Every edit lands in every other clip of that project, and
    a clip opened for the first time starts with them - a light rig that does
    not change from shot to shot. Off, each clip keeps its own, as before.
    LightsAllClips in the ini.
  - Free Look now works from the keyboard. It asked the game for "the marker
    whose menu is open", which is never the case in Edit Camera - where the
    flying happens - so the key found no keyframe and did nothing. It takes
    the editor's own marker index now.

WHAT CHANGED IN 3.10
  - The Free Look key is listed in the light editor's "Keys & mouse" table,
    and KeyFreeLook is written into an EscoEditor.ini that predates it, so
    the key can be seen and changed instead of being an invisible default.
  - Holding the Free Look key no longer flips it nine times a second: a
    switch takes the press only, not the auto-repeat the speed keys want.

WHAT CHANGED IN 3.9
  - Free Look: fly the editor's camera at a keyframe WITHOUT changing that
    keyframe's camera, and switch back and forth between your own view and the
    camera the clip will render. K by default, or the "Free Look" row on the
    Camera Speed page.
      On   your view. The keyframe's camera is remembered; flying no longer
           authors anything. Switching on again returns you to where you left
           off.
      Off  the keyframe's own camera goes back exactly as it was, and the
           editor recomputes the shot from it - that is the "final result".
    It switches itself off when the editor leaves that keyframe or changes
    mode, so a keyframe is never left holding your free camera. It needs a
    keyframe's menu open in the editor; the row's help line says what it did.
    Nothing in the game's camera code is touched: it is the marker record that
    is saved and put back, the same fields the keyframe clipboard copies.
  - The light editor no longer flickers on its key. Its key is watched twice
    over - by the worker and by the window, because ReShade hides keys from
    the game while it blocks input - and on some machines both acted on one
    press, so the window opened and shut within a frame or two and the key
    seemed not to work. One gate now: one open or close per press.

WHAT CHANGED IN 3.8
  - Hooks that could not go in while the game was starting no longer take the
    whole plugin down with them. On some machines (seen on FiveM b3258) the
    game hands over no thread list for a while, which MinHook needs, and every
    hook failed with MH_ERROR_MEMORY_ALLOC: no menu rows, no smooth blend, no
    weather, no scene lights - and the ReShade tab wrongly said the build was
    not mapped. Now the thread snapshot is retried, the worker puts in
    whatever is still missing every five seconds for two minutes, and after a
    minute of that it goes in without pausing the game's threads. The log
    says exactly what happened.
  - If lights or menu rows are missing, look for "hook:" lines in
    EscoEditor.log - "everything that was waiting is in now" means it sorted
    itself out.

WHAT CHANGED IN 3.7
  - "From a list" in the light editor's Transform section: the peds, vehicles
    and props the game has within 150 m of the camera, nearest first, with a
    tick per kind. Click a row and the light follows it - no hunting for a car
    in the picture. Hovering a row boxes that one in the picture.
  - "Jump to it" (on by default): a light lands a metre above the middle of
    the ped, vehicle or prop it starts following, instead of staying where it
    was. The whole clip and every key move by the same amount, so a keyed
    movement is kept - the light as a whole moves. Untick it for the old
    behaviour (stay in place and follow from there). It does not apply to
    following the camera.

WHAT CHANGED IN 3.6
  - The version is on screen: the light editor's window is titled "EscoEditor
    Lights 3.6" and ReShade's EscoEditor tab says it too. If it shows an older
    number, an older .asi is in the plugins folder - an extracted copy of an
    older zip can put one back over a newer one.
  - The log no longer fills with "lens flares drawn before the game's menus" /
    "drawn at the end of the frame": the pre-menu draw finds the game's own
    menu draw on most frames but not every one, and only a state that has
    held for half a second is worth a line.

WHAT CHANGED IN 3.5
  - More to a lens flare: its own colour and how far that replaces the
    light's (Flare colour / Tint), an Angle for the streak and the star, a
    Cross streak, 2 - 8 Star spikes, 1 - 6 Ghosts with their spacing, a Halo
    size, a Squeeze for an oval anamorphic core, and a Flicker with its speed
    that follows the clip's own clock - so it falls on the same frame every
    playback and holds still while you scrub. Two new starting points, Blue
    and Dirty.
  - Time of Day and Weather are in the light editor window as well (and in
    ReShade's EscoEditor tab), under "Time & weather", with Dawn / Noon /
    Golden / Night buttons - the same two overrides as on the EscoEditor menu,
    still session-only and never saved.
  - A flare saved by 3.3 loads unchanged: the numbers 3.3 did not have are
    filled in with what it drew.

WHAT CHANGED IN 3.4
  - Runs alongside Rockstar Editor+ (see WITH ROCKSTAR EDITOR+ below). Until
    now EscoEditor hooked the functions both plugins use two seconds after the
    game started, while RE+ installs much later and looks for those functions
    by their first bytes - so its menu, smooth blend and exporter never got
    off the ground. EscoEditor now waits for RE+ to install first and hooks on
    top of it, and its own searches step over a jump another plugin left.

WHAT CHANGED IN 3.3
  - Lens flares and a light leak, per light: "Flare (lens)" in the light
    editor, with presets (Soft, Anamorphic, Star, Cinematic, Leak) and
    sliders for size, brightness, streak, star, ghosts, halo, colour fringe
    and leak. Drawn by EscoFlare.fx through ReShade, right before the
    editor's own menus, so nothing in the editor goes blurry.
  - A "From the sun" switch on a light: the engine works its colour and
    brightness out from the sun at the clip's hour, so the light follows
    sunrise, noon and dusk by itself.
  - EscoFlare.fx sits next to the .asi. Install.bat copies it into
    the plugins folder and the plugin puts it into ReShade's shader folder
    by itself.

WHAT CHANGED IN 3.2
  - The light editor explains itself: a "Keys & mouse" list at the bottom of
    the window says what every key and mouse action does, and every button,
    switch and value carries a tooltip.

WHAT CHANGED IN 3.1
  - Placing a light while the camera is close to it. The arrows move a light
    by centimetres per screen pixel up close, which is why it was easier from
    far away. Now: Alt+click in the picture puts the light under the cursor
    and keeps dragging it across the screen, the mouse wheel pushes it away
    from the camera or pulls it closer, and while dragging the gizmo Shift
    moves it 6x faster and Alt finer. The Position fields step in proportion
    to how far the light is, and Ctrl+click on a field types an exact number.

WHAT CHANGED IN 3.0
  - A light can follow the camera, or a ped, a vehicle or a prop of the
    scene: "Follows" at the top of the light's Transform (see LIGHTS).
  - The light editor window picks the game window in front when ReShade
    runs more than one window.

WHAT CHANGED IN 2.9
  - EscoDOF (the ReShade bokeh shader and its Depth of Field rows) is gone.
    The game draws its own depth of field again, exactly as each keyframe
    sets it. Copy DOF / Paste DOF / DOF To All stay.
  - New page in the EscoEditor menu: Lights, with Open Light Editor and
    Light Editor Key (change the key that opens the editor, in game). The
    key can also be changed in the light editor window and in ReShade's
    menu (EscoEditor tab).
  - Light editor: a Transform section (Position, Rotation, Scale), a Scale
    gizmo (R), a clean keyframe list with one Delete button per key, and the
    light list and key list can be made taller by dragging their bottom edge.
  - The old EscoDOF settings leave EscoEditor.ini by themselves.
  - Coming from 2.5 - 2.8: EscoDOF.fx is not needed any more.
    Install.bat removes it from the plugins folder and from
    plugins\reshade-shaders\Shaders.

CHECK YOU HAVE THIS VERSION
  The first line of EscoEditor.log (next to the .asi) must say
  "EscoEditor 4.2".

INSTALL
  FiveM:          close FiveM, run Install.bat - or copy EscoEditor.asi
                  AND EscoFlare.fx to %LOCALAPPDATA%\FiveM\FiveM.app\plugins\ and
                  DELETE the old EditorCamSpeed.asi there
  Singleplayer:   copy EscoEditor.asi and EscoFlare.fx next to GTA5.exe, delete
                  EditorCamSpeed.asi
  The light editor WINDOW needs ReShade 6.3 or newer, the build WITH ADD-ON
  SUPPORT (the one that loads .addon64 files). EscoEditor registers with it by
  itself - nothing to set in ReShade. Without it everything else works, and
  lights placed before still draw.
  Your EditorCamSpeed.ini settings are carried over into EscoEditor.ini on the
  first start. Every FiveM game build id is declared (1604 ... 3889), so
  FiveM loads it.

IN THE MENU  (keyboard only - mouse clicks on these rows are ignored)
  Open any marker's menu (Camera, Depth of Field, Effects, Audio, Speed...).
  The last row is "EscoEditor" and shows a page name. Left/right on it
  switches page; the page's rows appear under it. The help line under the
  column explains the row you are on.

  Page: Camera Speed          (the free camera you fly in Edit Camera)
    Camera Speed    5% ... 1000%. 100% = the game's normal speed
    Up/Down Speed   Page Up/Down speed. "Same" follows Camera Speed
    Look Speed      how fast the camera turns with the mouse or stick

  Page: Copy & Paste          (whole keyframes)
    From Keyframe   first keyframe to copy - starts on this marker
    To Keyframe     last keyframe to copy - same as From = just one
    Copy Keyframes  press: the range goes to the clipboard
    Cut Keyframes   press: like Copy, and the originals are removed when you
                    Paste (same clip only)
    Paste Keyframes press: THIS marker becomes the first copied keyframe and
                    the others are added after it with their spacing
    Everything in a keyframe travels: camera, blend, shake, depth of field,
    effects, audio, speed. The clipboard is saved in EscoEditor.ini, so you
    can paste in another clip, another project, or tomorrow. Pasting into
    another clip clears the Attach / Look At / focus targets.

  Page: Time & Weather        (the whole clip, while it plays and when you export)
    Time of Day     As Recorded, or any time in 15-minute steps
    Weather         As Recorded, or Extra Sunny ... Halloween

  Page: Lights
    Open Light Editor   press: opens the EscoEditor Lights window
    Light Editor Key    the key that opens and closes it (L at first).
                        Left/right: the next free key (L K J H U I O P N M B,
                        F1 - F12, Insert, Home, End).
                        Enter, then press any key: that key. Esc or Backspace
                        cancel. A key already in use is refused and the help
                        line says why: W E R Del belong to the light editor,
                        W A S D and Page Up/Down fly the camera, F G 0 Shift
                        Alt are the speed keys, Enter Space Tab and the arrows
                        run the menus, and the key that opens ReShade's menu.

DEPTH OF FIELD  (the game's own Depth of Field submenu of a keyframe)
  The game's rows (Mode, Intensity, Focus Mode, Focal Distance, Target) work
  as always. Three rows are added at the bottom:
    Copy DOF        this keyframe's depth of field to the clipboard
    Paste DOF       onto this keyframe
    DOF To All      this keyframe's depth of field onto every keyframe of the
                    clip

LIGHTS  (the window needs ReShade with add-on support)
  Open the editor with its key (L), with Open Light Editor on the Lights page,
  or from ReShade's menu (the EscoEditor tab). The "EscoEditor Lights" window
  opens and the picture becomes a 3D viewport: every light is a dot with its
  name, and the selected one carries a gizmo.
    Add point / Add spot   a new light in front of the camera; a spot points
                           where the camera looks
    click a dot            select that light
    W / E / R              gizmo: move, rotate (a spot), scale
    drag an arrow          move along that axis: red X, green Y, blue Z
    drag the white square  move across the screen
    drag a ring            turn a spot
    scale gizmo            drag right or up to grow, left or down to shrink
    Alt+click              put the light under the cursor, keeping its distance
    mouse wheel            push the light away from the camera or pull it closer
    hold Shift / Alt       while dragging: 6x faster / finer
    hold Ctrl              snap: 25 cm, 15 degrees, scale in steps of 0.1
    Del                    delete the selected light
    its key or Esc         close the editor
  The window lists all of these under "Keys & mouse", and every button,
  switch and value has a tooltip that says what it does.
  Transform:
    Follows    what the light is fixed to (below)
    Position   X east, Y north, Z up, in metres - or, while it follows
               something, Offset: metres to its right (X), ahead of it (Y)
               and above it (Z)
    Rotation   a spot's X (tilt up / down) and Z (heading) - turned like what
               it follows, when it follows something. A round cone looks the
               same rolled, so there is no Y; a point light shines every way
               and has none
    Scale      the whole light bigger or smaller: it reaches Range x Scale,
               and a spot keeps the angles of its cone
    To camera, 3 m ahead and Aim like the camera place a light from where you
    stand.
  Follows:
    World              the light stays at a fixed place in the world
    Camera             the light moves and turns with the camera - a light on
                       the camera rig
    Pick in the scene  then click one of the boxes in the picture: every ped,
                       vehicle and prop within 150 m of the camera (the nearest
                       64) shows with its kind and distance. The light then
                       rides along with it - put it on a car's bonnet and it
                       stays there while the car drives. Esc cancels.
    Changing what a light follows keeps it where it is right now; its keys
    move with it. The replay rebuilds peds, vehicles and props when you jump
    around the clip, so the light finds its one again by its model, the one
    nearest to where it was last seen (with several of the same model in the
    scene, the nearest wins). Where that entity is not in the scene (not
    there yet, gone), the light is not drawn.
  Follows: World, Camera, "Pick in the scene" (then click a ped, vehicle or
  prop in the picture) or "From a list" (pick it from the list of what the
  game has near the camera, nearest first - the way to get a light onto a
  particular car). With "Jump to it" ticked the light lands on what it
  follows, a metre above its middle; untick it to keep the light where it is
  and follow from there. What it follows is remembered by its model, so the
  replay rebuilding the scene finds it again - and a light on a vehicle stays
  on that model of vehicle.

  Lights are saved in EscoEditor.lights.txt next to the .asi, under the
  project's name and the clip's number. Keep that file and the lights come
  back; copy it to another machine and the rigs are all still in it, but a
  clip only shows the ones saved under ITS project and number - use "Copy
  from" in the light editor to bring a rig over, or tick "Same lights in
  every clip" to spread one across a project.

  Free Look (K, or the row on the Camera Speed page): fly where you like at
  a keyframe without authoring its camera - for looking around, for placing
  lights from a better angle, for checking a shot. Switch it off and the
  keyframe's camera is back exactly as it was; switch it on again and you are
  back where you were flying. It switches itself off when the editor leaves
  that keyframe.

  Light: colour, intensity, range, falloff, the cone of a spot, the volume,
  and the switches Shadows, Sharper shadow, Volumetric, No reflections,
  Lit in a blackout and From the sun (the engine takes this light's colour
  and brightness from the sun at the clip's hour, so it changes with sunrise,
  noon and dusk on its own - Time of Day on the EscoEditor menu drives it).
  Flare (lens):
    Tick "Lens flare" and pick a starting point - Soft, Anamorphic, Star,
    Cinematic, Leak, Blue or Dirty - then set the numbers:
      Size, Brightness      how big and how strong the whole flare is
      Streak                the anamorphic line across the frame
      Star                  crossed spikes, as a stopped-down lens draws them
      Ghosts                the circles marching through the middle
      Halo                  the ring on the far side of the centre
      Colour fringe         how far the ghosts and halo drift in colour
      Light leak            light creeping in from the edge of the frame; it
                            shows even when the light itself is off screen
      Angle                 turns the streak and the star (0 = level)
      Cross streak          a second streak square across the first
      Star spikes           2 - 8, the blades in the iris
      Ghost count/spacing   how many ghosts and how far apart they sit
      Halo size             the ring's radius
      Squeeze               an oval core, 1 round, 2 twice as wide
      Flare colour / Tint   a colour of its own, and how far it replaces the
                            light's colour (blue at about 0.7 = the cine look)
      Flicker / speed       makes it breathe - an arc lamp, a fire, a failing
                            bulb. It follows the clip's own clock, so the same
                            frame always looks the same and it holds still
                            while you scrub.
    The flare follows the light's own Intensity as well, so keying the
    intensity keys the flare. A flare is hidden by whatever is in front of
    the light, and it needs ReShade with add-on support (EscoFlare.fx).
    The game's own video export does NOT contain ReShade effects: record the
    playback (OBS, ShadowPlay) or export with Extended Video Export. The
    lights themselves are in the game's export either way.
  The key at the bottom of the window can be changed there too ("Change",
  then press the new key).

  Lights belong to their clip. Every clip of every project keeps its own,
  saved in EscoEditor.lights.txt next to the .asi, and they are drawn in
  playback, in every menu and in the export. A light is one look for the
  whole clip until you tick "Animate over the clip". Then each edit goes to
  a key at the open marker, or at the playhead when no marker menu is open,
  and the light moves, turns, scales and changes colour smoothly from key to
  key (an attached light animates its offset). The key list shows each key's
  time with a Delete button; the key the edits go to right now is marked
  "editing".

  While the editor is open the game gets no input: close it to fly the
  camera or use the editor's menus. Only 8 lights in view can cast shadows
  at once (the game's limit); tick Sharper shadow on the ones that matter
  most.

WITH ROCKSTAR EDITOR+  (RockstarEditorPlus.asi)
  Both plugins can be installed together. EscoEditor notices RE+ in the log
  ("note: RockstarEditorPlus.asi is loaded") and then:
    - waits for RE+ to install its own hooks before installing its own, and
      hooks on top of them, so RE+'s menu, smooth blend, camera shake and
      exporter keep working. This can delay EscoEditor's own rows and lights
      by up to half a minute after the game starts - the log says when it went
      in ("Rockstar Editor+ has hooked ... installing EscoEditor on top").
    - starts its "EscoEditor" row Closed, because the game's marker menu draws
      only 16 rows and both plugins put rows there. Left/right on the row opens
      a page as usual; MenuPage in the ini still picks the page.
  The free camera belongs to EscoEditor when both are loaded: the speed on
  its row is what the camera moves at, and Look Speed, Response and the mouse
  multiplier are written again after RE+'s own tick every frame. RE+'s camera
  speed rows and keys will look like they do nothing - that is the
  arrangement, not a fault. Everything else runs side by side, and the scene
  lights of both plugins are drawn together.

SPEED ROW (the game's own row in the marker menu)
  Left/right step 5% at a time, from 5% to 1000%. Playback and export use the
  new speed. A project with these speeds opened WITHOUT the plugin plays
  those markers at 100%.

WITH KEYS (while flying in Edit Camera, game window in front)
    F            one step slower
    G            one step faster
    0 (zero)     back to 100%       (the "a grave" key on a French keyboard)
    hold Shift   3x while held
    hold Alt     0.25x while held (precision)
    mouse wheel  one step per notch - only with Hotkeys=3 in the ini

INI (EscoEditor.ini) - the menu writes the same keys
  [EscoEditor]
  Speed, VerticalSpeed, LookSpeed            as above
  Response=1            0 stock ramp, 1 ramp scales with the speed, 2 instant
  Hotkeys=2             0 off, 1 step keys, 2 + hold keys, 3 + mouse wheel
  MenuRows=1            0 = no rows in the editor menu
  MenuPage=1            the page EscoEditor opens on (0 closed .. 4 Lights)
  Scene=1               0 = no Time & Weather page
  Lights=1              0 = no scene lights and no light editor
  KeyLights=0x4C        the key that opens the light editor (L); the Lights
                        page changes it
  KeySlower/KeyFaster/KeyReset/KeyBoost/KeySlow   virtual-key codes
  BoostMul / SlowMul    the hold multipliers
  HotkeysAnywhere=0     1 = keys also work in the editor menus
  Log=1                 0 = no log file
  [Keyframes]           the clipboard, written by Copy / Cut
  [DepthOfField]        the copied DOF, written by Copy DOF

IF SOMETHING LOOKS WRONG - read EscoEditor.log
  "reshade: registered EscoEditor as an add-on ... ImGui 1.90.4 handed over"
                                           = the light editor window can open
  "reshade: not in the process"            = no ReShade: no light editor window
  "lights: installed"                      = scene lights are live
  "reshade: EscoFlare.fx is loaded ..."    = the lens flares can draw
  "reshade: lens flares drawn before the game's menus"  = the editor stays sharp
  "lights: a light can follow the camera, or peds vehicles props"
                                           = the scene's entity lists were found
  "lights: clip 2 of 'Project' - 3 light(s)" = the lights of the clip on screen
  "lights: 3 scene light(s) handed to the game" = they reach the renderer
  "lights: 'Spot 2' follows Vehicle 1A2B3C4D now" = a light was attached
  "lights: 'Spot 2' found its Vehicle 1A2B3C4D again (the replay rebuilt it)"
                                           = it found its vehicle after a jump
  "lights: the light editor draws in the 2560x1440 window" = where it is drawn
  "lights: the light editor key is now K"  = a new key was saved

SOURCE
  src\main.cpp + src\ecs_*.inc + dist\EscoFlare.fx + build.bat in the folder
  above dist\ (MIT;
  MinHook is BSD-2; the ReShade add-on headers in src\reshade are BSD-3, Dear
  ImGui's header in src\imgui is MIT). build\selftest\ runs the offline
  self-test.

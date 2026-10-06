# The player-facing layer

The plan for Phase 6's last item: save states, an SDL2 + Dear ImGui frontend
and controller remapping. Written 4 Oct 2026 against psprecomp 95a2759,
last-raven f5a590b and third-birthday-recomp as they stood that day. Nothing
here is implemented yet. The choices it left open were settled with
Sif the same day; see *Decided*.

The three items look independent and are not. A menu has to pause the game,
a save state has to be taken while the game is paused at a known place, and
rebinding needs the menu to be usable. So the plan starts from one shared
foundation, a **safe point** where the host can stop the guest, and builds
the three on top of it.

All of it ships in **one player**: a launcher, an ISO importer, an on-device
compiler, the runtime and one shared host, living in psprecomp. Every
supported game is a **title pack**, its own code and data compiled on the
player's machine against that host (§1). Last Raven's package already works
this way for the three Armored Core titles. The plan generalises it so that
The 3rd Birthday, and titles after it, plug in without forking the host.

## Where things stand

**Host layer.** `psprecomp_host` (`src/host/`) holds the savedata dialog and
nothing else. Its only consumer is third-birthday-recomp's `test_host`.
Both games compile `save_dialog.c` by path in their own build scripts, and
each game carries its own `present.c`, `render_gl.c`, `launcher.c` and
`settings.c`. Against Last Raven, The 3rd Birthday's copies differ by about
510 lines in `present.c`, 470 in `render_gl.c` and 1,190 in `launcher.c`.

**Threads.** One SDL thread, not the process's main thread, initialises
SDL, creates the window and GL context and runs the only event loop
(last-raven `host/present.c`, `sdl_thread`). It releases the context, and
the **GE thread** claims it. That thread is a guest thread's host thread,
and it composes and swaps. In GL mode the window swaps only when the guest
presents. The one exception is the savedata dialog's throttled redraw, which
the guest's own `sceUtilitySavedataUpdate` drives. In software mode the SDL
thread repaints on every loop iteration.

**Input.** Static tables (`KEYS_CLASSIC`, `KEYS_WASD`, `CLASSIC`, `MODERN`,
`MOUSE_WASD`) are chosen by the `KEYS`/`GAMEPAD`/`INPUT`/`MOUSE` settings.
The host owns one controller at a time. Title-specific buttons travel as
carrier bits: `LR_PAD_*`, which Last Raven's replacements decode, and The
3rd Birthday's `t3b_*`. A weak symbol, `lr_modern_controls_available`, says
whether anything will decode them. Everything meets in `publish_pad` and
then in `ctrl_fill` (`src/hle/misc.c`). The recorder sees the merged pad, so
replays never see host bindings. **No binding can be changed.**

**Settings.** Each game has a typed registry with 23 options in Last Raven,
carrying label, page, help and range, plus INI presets and a launcher drawn
with SDL_Renderer and SDL_ttf. Every consumer caches its value at startup.
Window size is the only setting that can change while the game runs.

**Pause.** None. The savedata dialog blocks input and publishes a neutral
pad while the game keeps running underneath.

**Snapshots.** `PSPRECOMP_RAMSNAP` and GE capture (`.gcap`) save memory and
GE registers for analysis. They are instruments, not save states. Neither
stores threads, kernel objects or the GE queue.

**Build.** There is no C++ in any of the three projects. GL entry points are
loaded through `SDL_GL_GetProcAddress`, with no loader library and no `-lGL`.
This machine runs `sdl2-compat` 2.32.72 over SDL3 3.4.16, which Arch has
shipped in place of `sdl2` since late January 2025. The packages bundle real
SDL2 2.32.10 instead.

**Packaging.** Last Raven's AppImage (last-raven `docs/PACKAGING.md`)
already compiles games on the player's machine:

1. `packaging/linux/games.json` pins each title's disc ID and decrypted
   executable SHA-256, and names its replacements and replace list.
2. `scripts/import_game.py` extracts the executable, decrypts it and
   verifies it.
3. `scripts/compile_game.py` runs the emitter, a title-specific generator
   (`fps-loop.py`) and `emit-split.py`, then compiles with the bundled Zig.
   It links a prebuilt **per-title** host library, `libhost-<slug>.a`, which
   is Last Raven's own `present.c`/`render_gl.c`/`boot.c` with `GAME_TITLE`
   compiled in.

Per-title build fingerprints rebuild only the titles whose inputs changed.
The launcher learns every title's options from Last Raven's `settings.c`,
compiled into the launcher itself. The 3rd Birthday has no packaging. Its
host carries its own features: an audio mixer, a bloom filter, aim modes, an
aspect camera, gait, and its own launcher pages.

## Decisions

1. **One player, many title packs, in psprecomp.** Everything
   game-specific is a pack:
   - a manifest (data) that the importer and the compile step read;
   - a settings schema (C) compiled into both the launcher and the game;
   - C sources compiled on the device with the generated code;
   - one registration call at boot.

   Packs are compiled with the same app build's headers and linked
   statically, so there is no plugin interface to keep stable; build
   fingerprints rebuild a game when its inputs change. Each game's
   repository keeps its pack, and the player build takes packs by path.
2. **One safe point, owned by the runtime.** The host gets one hook where
   the guest is quiescent: at a frame-boundary firmware call, on the GE
   thread, not nested inside a callback. Pause, the menu, quick save and
   quick load all happen there and nowhere else.
3. **Remapping happens in the host, before `publish_pad`.** Bindings become
   data, and the existing layouts become their defaults. Replays and
   recordings are unaffected because they already sit below this layer.
   Title-specific buttons become **named actions** that the title registers,
   replacing hard-coded carrier tables and the weak symbol.
4. **Dear ImGui is vendored, and its C++ stays in a few host files.** UI
   pages are generated from registries (settings, bindings, save slots), so
   a game host writes no UI code. ImGui runs on the SDL thread. In GL mode
   the GE thread draws a copied snapshot of its output. cimgui and
   dear_bindings were considered; both still compile C++ underneath, and a C
   API of our own is smaller.
5. **Save states resume natively and are only taken at the safe point.**
   The emitter gains a resume entry at every call's return site. A restored
   thread re-enters its live guest frames one at a time from the return
   addresses on the guest stack. The interpreter is not needed in the game
   build. A save the runtime cannot resume exactly is **refused with a
   reason**, never approximated. The scope is quick save and load anywhere,
   reached in stages (see *Decided*).
6. **The host merge comes first.** A player shared by different games needs
   one host, one settings core and one input layer. So `present.c`,
   `render_gl.c` and the settings core move into `psprecomp_host` before the
   new features are built, and each title's own behaviour becomes a hook it
   registers (§1). Every feature after that lands for all packs at once. A
   title's remaining work is its registrations, such as safe-point calls,
   input actions and census runs.

## Order of work

| # | Stage | Needs | Gate |
|---|---|---|---|
| 1 | Pack interface; the host and settings core into psprecomp; the Armored Core titles built on it | — | Replay rows, render checks and host fixtures unchanged for all three titles |
| 2 | The player in psprecomp: importer, compiler, packaging; Armored Core packs taken by path | 1 | The player's package and one built by the old pipeline from the same sources prepare all three real ISOs into games that replay identically; an existing install updates intact |
| 3 | The 3rd Birthday as a pack: its features on host hooks, `render_gl.c` reconciled | 1, 2 | TB's own tests and replays unchanged; a packaged TB import plays |
| 4 | Save-state census and resume entries (spike) | — | Park census per title; emitted-code cost measured; resume unit test passes |
| 5 | Safe point and host pause | 1 | A pause is invisible to the guest; paced summary shows guest time excludes it |
| 6 | Input ownership and binding tables | 1 | Defaults reproduce today's layouts exactly; replay rows unchanged |
| 7 | The overlay: pause menu, settings, rebinding, volume | 5, 6 | Render checks unchanged with the overlay closed; open/close checks with held input |
| 8 | Save states, loaded at launch | 4, 5 | Save at poll N, load, run to M: rows identical to the uninterrupted run, every title |
| 9 | Quick save and load in-process, slots in the overlay | 7, 8 | Repeated load loops: no leaked threads, rows still identical |
| 10 | The launcher rebuilt on the overlay's generated pages | 2, 7 | Launcher tests, packaged launcher-to-game path |

Stage 4 touches only the runtime and the emitter, so it can run alongside
stages 1–3. It should, even though its payoff comes last, because it is the
one stage that can change the plan: if the census shows threads parked in
many different kinds of wait, save states cost much more, and the scope gets
decided again before the overlay is built around them.

## 1. The player and title packs

**The player** is one application: launcher, ISO importer, on-device
compiler, runtime and the shared host. It prepares each supported game on
the player's own machine from their own disc. It is Last Raven's AppImage
generalised, and its recipe moved into psprecomp's `player/` on 5 Oct
(`player/README.md`):

- `import_game.py`, `compile_game.py`, `emit-split.py`;
- `game_fingerprints.py`;
- the bootstrap/build scripts, the dependency pins and `AppRun`.

**A title pack** is everything about one game, and it ships as source and
data only, never game bytes.

*The manifest*, data read by the importer and the compile step:

- slug and title;
- accepted disc IDs and executable SHA-256s, today's `games.json` rows;
- module path, replace list and code-generation steps that run after the
  emit, such as Last Raven's `fps-loop.py`;
- source files and header directories;
- **named input actions** with their carrier bits and default bindings;
- the frame-boundary calls for the safe point (§2);
- capabilities such as movie decoding.

*The settings schema*, `psp_title_settings`
(`include/psprecomp/host/settings.h`): a C file with no dependency on
generated code. It holds the option table (key, type, default, range or
choices, labels, page, help, display format), the starter presets, and hooks
for the title's derived values and notes. An apply policy (live, next
launch, restart) joins it in stage 7.

*C sources*, compiled on the device alongside the generated code: the
replacements, and any host features of the title's own.

*One registration call*, `psp_title_register(const psp_title *)`, made from
boot. It replaces the weak symbols (`lr_modern_controls_available`,
`lr_adaptive_aspect_available`) and the compile-time `GAME_TITLE`. Through it
the title registers:

- its input actions;
- the settings it reads, by key;
- pause and resume listeners;
- save-state data and resume ranges (§5);
- the host hooks it uses.

**No plugin interface.** A pack is compiled against the headers of the same
player build and linked statically into its game's executable. When the
player updates, the existing fingerprints (`game-builds.json`) decide which
games to rebuild. A pack's fingerprint covers its manifest, its sources and
their transitive headers, as a title's replacements are covered today.

**One definition of each option.** The launcher has to show a title's
settings before that title has ever been compiled, and the game has to
validate the same values. The player build compiles each pack's settings
schema into the launcher, and the device compiles it into the game, so both
read one table. The launcher's pages are generated from the schema from
stage 2 on. That is what makes it a generic launcher before stage 10
rebuilds it on ImGui.

This plan first put the options in the manifest and generated the C table
from it. Writing the schema in C needs no generator, and the hooks that
derive values from options were C anyway. Stage 1's settings core is built
that way (5 Oct).

**Hooks, added when a pack needs one.** Title behaviour that today is a fork
of `present.c` or `render_gl.c` becomes a hook in the shared host:

- audio output: The 3rd Birthday's mixer, `psp_audio_set_pending` and host
  work;
- a post-processing pass: its bloom filter;
- the aspect scene size and camera, which both games have;
- pause and resume listeners: Last Raven's higher-FPS clock.

Packs can share family code. The Armored Core titles share `ac3_controls.h`
and `stick.h` from a family directory that their packs include.

**Repositories.** psprecomp holds the player. Each game's repository keeps
its pack, beside the research, scenarios and replays that justify it. The
player build takes packs by path, so there is no submodule cycle: the game
repositories include psprecomp, never the reverse. The source archive
shipped with a release includes every pack it was built with.

**Moving the host (stages 1–3), in the order that unblocks the rest:**

1. **Stage 1: the registration call and the settings core.** Last Raven's
   `settings.c` core moves into the toolkit, its options become the title's
   schema, and the three Armored Core titles are built on it. *The settings
   core was done 5 Oct* (`src/host/settings.c`, `tests/test_settings.c`).
   Last Raven's printouts, preferences file, launcher screenshots and garage
   replay are byte-identical across the move.
2. **Stage 1: `present.c`'s shared core.** That is the SDL thread, GL
   handoff, frame conversion, controller ownership, look channel and dialog
   integration. Audio becomes a hook, and the title becomes a runtime value.
   *Moved 5 Oct* (`src/host/present.c`, `tests/test_present.c`), whole and
   with Last Raven's behaviour. The title is `psp_title_info`: a name,
   capability bits replacing the weak flags, and optional control notes.
   The carriers are `PSP_PAD_*`. Garage and both sibling probes replay to
   identical summaries, and the windowed startup lines are unchanged. The
   audio hook waits for The 3rd Birthday's mixer in stage 3, the first
   consumer that needs it.
3. **Stage 2: the importer, compiler and packaging recipe move into
   psprecomp.** Last Raven's three titles become packs. *Done 5 Oct*
   (`player/`). Last Raven is one pack: `pack.json`, plus a CMake file for
   the launcher, `boot.c` and the GL backend, which have not moved yet.
   - The gate was first written against the `deck-test-6` control logs.
     A month of runtime changes separates that build from today's, so the
     gate became an A/B. Last Raven's own pipeline and the player built
     packages from the same sources, and each prepared the three real ISOs
     into an isolated library.
   - The garage and both probe replays gave identical summaries: 928 and
     3,003 lists; 10,980,171, 14,251,977 and 14,242,139 commands; 0 bad
     accesses.
   - Both AppDirs hold the same files, apart from the new `app.json`, and
     pass the same builder checks.
   - An install made by the old package opened in the new one with its
     settings and records untouched. Every title asked for preparation once,
     because its fingerprint changed, and re-preparing one kept its old build
     and the save beside it.
4. **Stage 3: `render_gl.c`.** It moves after The 3rd Birthday's roughly 470
   lines of drift are reconciled.
5. **Stage 3: The 3rd Birthday becomes a pack.** Its features move onto
   hooks, and its launcher pages (Graphics, Gameplay, Controller, Keyboard &
   Mouse, Advanced) become pages of its schema.
   *Settings and presentation done 5 Oct*.
   - Its settings are a schema on the shared mechanism. Its CMake makes the
     schema part of `psprecomp_settings`'s link interface.
   - Its `present.c` is gone. The audio backend, input tables and hooks,
     and scene extent it needs are in `host/t3b_present.c`, given through
     `psp_title_info`.
   - An input harness drove the old and new layers through 16
     configurations and got identical pad words, 896 lines.
   - Its deterministic scenarios gave identical artifacts. The real-time
     windowed ones vary run to run.
   - The packaged launcher is psprecomp's, from Last Raven's, which had
     the library and import flow (The 3rd Birthday's does not).
     *Done 5 Oct* (`src/host/launcher.c`, `tests/test_launcher.c`).
     - Its pages are the schema's page names, in the order they first
       appear. Its rows are their options, except those marked
       `PSP_OPTION_HIDDEN`.
     - An option's `stops` replace Last Raven's FPS-cap list.
     - Each pack's `psp_launcher_info` names the app and carries its About
       text, its title order and what New and Reset start from.
     - Last Raven's launcher tests pass on it, and their 14 screenshots
       are byte-identical.
     - The 3rd Birthday keeps its own richer launcher for development.
   - Decided 5 Oct: The 3rd Birthday is packaged before `render_gl.c` is
     merged. Its pack carries its own `render_gl.c` until then.
   - *The 3rd Birthday is a pack, 5 Oct* (its `pack.json` and
     `packaging/linux/pack.cmake`).
     - Its replacements compile on the player's machine.
       `aspect_camera.c` needs no generated header, so it is a host
       object, and `compile_game.py` is unchanged.
     - Its AppImage passed every builder check.
     - Its importer took the retail ISO and compiled the game in 2 m 17 s.
     - The packaged game replayed the `gameplay` scenario to a GE capture
       and PCM byte-identical to the development build's, with 0 bad
       accesses, and ran it through GL at 1080p.
     - Last Raven's package, rebuilt on the shared launcher, passes the
       same checks. Its builder screenshots match stage 2's, apart from
       one save time.
   - *`render_gl.c` merged, 6 Oct* (`src/host/render_gl.c`; step 4).
     - The 3rd Birthday's backend, a superset of Last Raven's, is the base.
     - What was that game's own is title data:
       - `PSP_TITLE_ASPECT_BY_SOURCE`: screen-space draws placed by what
         they read;
       - `psp_title.bloom`: the glow composite the smooth filter
         recognises;
       - the scene extent, through `present_aspect_extent`.
     - Armored Core keeps its rule: a full-width screen-space draw fills
       the wide scene.
     - Checked by replaying 17 game captures through both backends across
       seven to nine resolution, aspect, window-shape and bloom settings:
       131 cases.
       - The 3rd Birthday: all 54 cases identical, images and reports. Its six GL
         fixtures report identically.
       - Last Raven: classification, native aspect and tall windows
         unchanged. Three general fixes now apply, each reproduced by
         reverting it alone:
         - nearest sampling's texel-boundary tolerance;
         - a whole-pixel HUD origin at window resolution with the wide
           aspect;
         - the exact scene scale at PSP resolution with the wide aspect,
           which was up to 0.3 px too wide.
       - Its render, resolution, preview and savedata checks keep their
         counts.

The launcher stays a separate pre-launch process, and its INI file and
`--config`/`--preset` arguments remain the contract with each game.

**Existing installs.** The app's name and its XDG folders (`last-raven`
today) change once it is no longer only Armored Core. The first launch
migrates the library, settings and saves, following the rule
`PACKAGING.md` already uses for its legacy settings. Saves are never moved
without a copy remaining.

**A disc without a pack is rejected,** as an unknown executable is today.
Each title's correctness rests on its own measurements.

## 2. The safe point and host pause

**Where.** The hook sits in `psp_hle_call` (`src/hle/hle.c`), after the
handler returns and before the post-handler ticks. It fires only when all
three hold:

- the call's NID is one of the frame-boundary calls named in the title's
  manifest;
- the caller is the GE-owner thread;
- `g_call_depth == 1`, so the call is not nested inside a callback or an
  interrupt handler.

At that moment the calling thread has finished its call (v0 is written) and
holds the scheduler token. Every other guest thread is parked inside a
firmware call, because handoff happens only in firmware calls
(`src/hle/sched.h:3-30`). Which calls count as frame boundaries is measured
per title, not assumed; Last Raven presents through `sceDisplaySetFrameBuf`
from `00259820`, and the controller read is another candidate.

**Pause.** On a pause request the hook enters a host loop. Because that loop
keeps the token, no guest thread runs. While it loops:

- **Clock.** The real-time clock stops. On resume it is re-anchored
  (`origin = now - us*1000`, `src/clock.c`), so neither vblank nor the timers
  catch up. Game hosts with clocks of their own subscribe to a resume
  listener; Last Raven's higher-frame-rate deadlines in `host/fps.h` are the
  first.
- **Audio.** The mixer outputs silence without consuming its rings. A
  movie's audio/picture lead is preserved because both are measured in guest
  time.
- **Redraw.** In GL, the loop recomposes the last presented frame plus the
  overlay and swaps at display rate. This generalises the dialog's existing
  `gl_dialog_redraw`. In software the SDL thread already repaints.

**Gates.**

- An unpaced replay with the hook enabled and a scripted pause
  (`PSPRECOMP_PAUSE_AT=<poll>:<ms>`) gives rows identical to the same replay
  without it.
- A paced, windowed run paused for 10 s prints guest elapsed time 10 s below
  wall elapsed time.
- Clock drift is no worse than before.
- Audio gap counts are unchanged.
- A pause in the middle of the intro movie keeps the lead flat.

## 3. Input ownership and remapping

**3a. One input owner.** Lift the dialog's arbitration (last-raven
`host/present.c` around `save_dialog_event`, `g_dialog_block_input`,
`publish_pad`) into a host module with three owners: the game, the dialog
and the overlay.

- **Taking ownership:** release mouse capture, clear held keys, buttons and
  stick, clear the banked mouse motion (`psp_ctrl_clear_mouse`), and publish
  a neutral pad.
- **Giving it back:** wait until all input is released, or for 1 s, so the
  click or button that closed the menu is not turned into a shot.

The save dialog moves onto this module unchanged in behaviour.

**3b. Bindings as data.**

- **Sources:** controller buttons, stick axes (the whole stick, or one
  half), triggers, keyboard scancodes, mouse buttons, mouse motion and the
  wheel.
- **Targets:**
  - PSP buttons;
  - the PSP stick (analog from a stick, or digital from keys, keeping
    today's per-axis keyboard ownership);
  - the look channel;
  - **title actions** (named, registered by the title, carried as today's
    carrier bits);
  - **host actions**: menu, quick save, quick load, slot ±, screenshot,
    mouse capture, fullscreen, quit.

Thresholds and hysteresis for axis-to-button bindings stay at today's
values (50%/25% on the triggers). Stick deadzones stay in the replacements,
which already tune them per mode.

**Defaults reproduce today exactly.** `KEYS_CLASSIC`, `KEYS_WASD`,
`CLASSIC`, `MODERN` and `MOUSE_WASD` become built-in binding sets. A table
test resolves every source through the old tables and the new defaults, for
every layout and every mode, and requires identical output. Only then are
the old tables deleted.

**Title registration.** A title's pack declares its named actions, with
their carrier bits and default bindings, in its manifest (§1): Last Raven's
`LR_PAD_*`, The 3rd Birthday's `t3b_*`, and the AC3 engine's
`extra_for_action` (`host/ac3_controls.h`). Registration replaces
`lr_modern_controls_available`: a title that registers no actions gets the
classic PSP map.

**Storage.** Bindings are `bind.*` keys in the existing preset INI. A preset
without them uses the title's defaults. Keys for PSP buttons are shared
across titles, which matches the decision that presets are shared. A title
ignores title-action keys it did not register.

**Controllers.**

- Load `gamecontrollerdb.txt` from the config directory, when it is there,
  with `SDL_GameControllerAddMappingsFromFile` before any pad is opened.
- Keep a single active pad. Add an option to switch the active pad to
  whichever one was pressed last.
- Packaging already allows Steam's virtual pad
  (`SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=1`).

**3c. Live changes.**

- Bindings live on the SDL thread, so a change applies at once.
- Tuning values that the replacements cache in statics (deadzones,
  sensitivity, expo, camera lag) move behind a settings generation counter.
  An immutable snapshot is published atomically, and the replacements reload
  it at their next poll.
- While a replay is being recorded, gameplay-affecting tuning is either
  locked or written into the recording as a directive, so the recording
  stays reproducible.

**Gates.**

- The table test above.
- Today's virtual-pad tests (`SDL_JoystickAttachVirtual` with the isolation
  hints) for every default.
- New tests for rebinding, hot-plug and the switch to the last-used pad.
- Unchanged replay rows, as a guard: replays bypass the host, so a change
  there means the layer leaked.

## 4. The Dear ImGui overlay

**Dependency.** Pin Dear ImGui v1.92.9b, released 31 Jul 2026, under
`third_party/imgui/`. It is MIT-licensed and not in Arch's official repos,
so vendoring is the norm. Two host files are compiled as C++:

- `imgui*.cpp` plus `imgui_impl_sdl2.cpp`, for input and platform. SDL2's
  backend is still shipped and maintained beside SDL3's
  ([backends](https://github.com/ocornut/imgui/tree/master/backends)).
- one file of our own, which implements a C API, `psp_ui_*`.

CMake enables CXX for `src/host` only. Every build that compiles host files
by path has to change:

- both games' `06-boot.sh`;
- the packaging CMake;
- the test scripts.

The on-device `zig cc` link needs the C++ runtime. Getting the packaged
build to link is part of this stage, not an afterthought.

**Threads.** One ImGui context must not be used from two threads
([FAQ](https://github.com/ocornut/imgui/blob/master/docs/FAQ.md#about-multi-threading)).
SDL2's event pump belongs to the thread that initialised video
([SDL_PumpEvents](https://wiki.libsdl.org/SDL2/SDL_PumpEvents)). So the
whole ImGui frame runs on the **SDL thread**: `ProcessEvent`, `NewFrame`,
widgets and `Render`.

- **Software path:** draw with `imgui_impl_sdlrenderer2` on the same thread,
  before `SDL_RenderPresent`.
- **GL path:** copy the frame into a C snapshot. The snapshot holds
  vertices, indices, commands with clip rectangles and texture IDs, plus the
  1.92 texture create/update/destroy requests with their pixels. The GE
  thread draws it in `render_gl.c`'s compose step, after the dialog overlay
  and before the swap. It saves and restores GL state and sets
  `g.mu.disturbed`, as `gl_dialog_overlay` does.

The GL drawing is our own C code (one shader, scissor per command), not
`imgui_impl_opengl3`:

- `render_gl.c` keeps sole ownership of GL;
- it stays on the `SDL_GL_GetProcAddress` loader;
- it avoids the backend's embedded gl3w loader, which the sdl2-compat
  README warns about under Wayland/EGL.

`IMGUI_IMPL_OPENGL_LOADER_CUSTOM` exists and is the fallback if writing our
own renderer turns out to be the wrong call.

**The risk in this stage** is carrying the 1.92 dynamic font atlas across
threads. Upstream's helper for exactly this, `ImDrawDataSnapshot` plus
`ImTextureQueue` in imgui_club's `imgui_threaded_rendering`, describes
itself as "not well tested". If our copy of the texture requests proves
fragile, the fallback is a fixed atlas built once at the chosen UI scale and
rebuilt when the scale changes.

**What it shows.**

- **Resume.**
- **Save state and Load state:** slots with thumbnails and times, from
  stage 9.
- **Controls:** the bindings, a press-to-bind capture and the tuning values.
- **Graphics:** options that apply live are editable; the rest show
  "applies on restart".
- **Audio:** host master volume and mute, which the settings plan lists as
  missing.
- **Performance:** fps and frame time, reusing the counters behind the
  window title.
- **Quit.**

Each page is generated from a registry. That needs one new field on
settings options: an **apply policy** (live / next launch / restart).

**Navigation and scale.**

- Gamepad navigation (`ImGuiConfigFlags_NavEnableGamepad`) uses the pad the
  input owner already holds, via `ImGui_ImplSDL2_SetGamepadMode` in Manual
  mode, so ImGui never opens controllers of its own.
- `ImGui_ImplSDL2_GetContentScaleForDisplay` returns 1.0 on Wayland, so the
  UI scale comes from the drawable height (the 3440x1440 desktop, a
  1280x800 Deck) plus a user multiplier.

**The menu always pauses the game** (stage 5). Opening it takes input
ownership (3a). On a controller, pressing View+Start opens it, and Quit moves
into the menu. That retires today's quit chord (View+Start held for 2 s);
Ctrl+Shift+Q stays.

**Gates.**

- With the overlay closed, `scripts/12-render-tests.sh` and the fixed-scene
  checks give byte-identical results. The UI stays out of guest memory and
  out of captures.
- Opening and closing while holding move and fire, with focus loss and
  mouse capture, behaves as SETTINGS-PLAN.md specifies.
- Controller-only operation works on the Deck.
- The packaged build links and runs.

## 5. Save states

### Why the obvious approach does not work here

In an emulator, a save state is memory plus registers. Here, a guest call
chain is a chain of C calls on a host stack, and C stacks cannot be written
to a file. What can be written is everything the guest can see:

- All guest registers live in one global struct, `psp_cpu`
  (`include/psprecomp/cpu.h:42-68`).
- No guest value lives in a C local across a call. The emitter's locals
  (`_c`, `_jt`, `_entry`) never span one.
- Every `jal`/`jalr` sets `$ra` to the return site before its C call
  (`tools/allegrexrecomp/emit.c:982`), and prologues store it to the guest
  stack in emulated RAM.

The guest stack therefore holds the true return chain.

### Resuming natively

`jr $ra` compiles to a C `return` that ignores `$ra` (`emit.c:990-998`), and
every function body already takes an entry address, `psp_body_X(_entry)`,
with a switch over its labels. So a function can be re-entered in the
middle. To resume a thread:

1. Restore its register file.
2. Enter the function containing its return site *at* that site.
3. When that function returns, its epilogue has reloaded `$ra` with its own
   caller's return site. Enter that next.

The driver is a loop of the form "dispatch to `$ra` until `$ra` is 0", and
0 is already the thread-entry sentinel (`src/hle/sched.c`, `thread_main`).

The emitter change is to mark each call's return site as an entry of the
function that contains it. Last Raven's emitted file has about 50,000 call
sites and 43,000 interior-label thunks today. Return sites go into a sorted
resume table of `{site, body, entry}` instead of becoming thunks, so the
dispatch table does not double.

Two costs need measuring before this is accepted:

- extra `case` labels in each body's entry switch;
- `-O2` code quality around the new merge points. This is expected to be
  small, because guest state is global memory that is reloaded after every
  call anyway.

The interpreter checks the result rather than shipping in the game: a unit
test interrupts a three-deep call chain, resumes it natively, and compares
the outcome with an interpreted run from the same register file.

### What can be resumed, and what is refused

A thread's state at a save is one of:

1. **Not yet started:** its entry point and arguments.
2. **At the safe point:** the calling thread. Its call has completed, so it
   resumes with the post-handler ticks and then the return site.
3. **Parked in a wait:** the hard case. The rest of the firmware handler
   lives in its C locals. `hle_WaitSema` (`src/hle/threadman.c:1637-1721`)
   keeps `id`, `tmo_ptr`, `deadline`, the block result and the wake reason
   across the park.

   Each kind of wait that a census actually observes is split into a *start*
   half and a *finish(rc)* half. The parked state (wait type, object,
   arguments, deadline) moves into the thread record. A restored thread
   re-blocks on the same queue position and runs *finish* when woken.
4. **Anything else is refused,** with a reason:
   - a thread parked in a callback, the mpeg ring callback or a
     `psp_call_guest`, where host frames sit between guest frames;
   - a movie playing (openh264/libavcodec state);
   - the savedata or message dialog active;
   - a replaced function live on a parked thread's stack without a
     registered resume.

   A quick save that is refused retries at the next safe points for a
   moment, then reports why.

**Replaced functions.** The interior thunks of a replaced function still
enter the *original* body (`emit.c:1091-1104`). A replacement that is live
on the stack therefore needs a resume of its own. Last Raven's
higher-frame-rate mission loop (`fps_native_loop`, replacing `00102018`) is
always live in a mission, and it has a natural resume point at its frame
start (`0x0010209C`). Titles register `{address range → resume function}`.
A counter on replacements that call back into guest code catches any
unregistered one at save time.

### The census (stage 4)

`PSPRECOMP_PARK_CENSUS=<polls>` prints, at the safe point, where every guest
thread is parked (`src/hle/census.c`):

- handler, wait type and object;
- callback nesting;
- live replacements;
- whether a movie or dialog is active.

The safe point is the one stage 5 will formalize: a firmware call from the
GE-owning thread's own guest code has completed. The scheduler records each
park as it happens (block, delay, yield, preemption). Every host frame that
calls back into guest code marks itself per thread: the runtime's guest
calls and interrupt handlers, and every call the emitted code makes into a
function the host replaced (`PSP_REPLACED`, emitted always).
`tools/park_census.py` reduces the logs.

*Results, 6 Oct.* Every census found the safe point on the game's main
thread, right after its controller read.

| Title | Replays | Censuses | Resumable, given split waits | Movie playing | Under host frames |
|---|---|---|---|---|---|
| The 3rd Birthday | gameplay (boot to the street), modern-buttons (street, pause), modern-hub (a save loaded into the hub), movie-audio | 98 | 38 | 60 | 0 |
| Last Raven | pause-look (intro, menus, garage, a mission, pause), mission-effects (fire and boost, Higher FPS on and off) | 37 | 12 | 2 | 23 |

AC3 Portable and Silent Line run slower headless: their first pass covered
the menus only. Its 11 censuses parked the same way as Last Raven's.

The threads were parked in eight firmware calls in all:

| Call | Kind | Where |
|---|---|---|
| `sceKernelWaitSemaCB` | a queue wait | The 3rd Birthday's three stream handlers |
| `sceKernelSleepThreadCB` | a wakeup count | The 3rd Birthday's file reader |
| `sceKernelWaitThreadEnd` | waits for a thread | Armored Core's `user_main`, for the game thread |
| `sceKernelDelayThread`, `sceKernelDelayThreadCB` | a timed delay | both games' sound and movie threads |
| `sceAudioOutputPannedBlocking`, `sceAudioOutput2OutputBlocking` | a timed delay (the runtime's audio pacing) | both games' mixers |
| `sceMpegRingbufferAvailableSize` | a delay inside the call | during movies only, which are refused anyway |

None was a nested call, and no thread was inside a callback when the census
ran. So the split is small: three real waits, whose parked state is a
queue position or a count, and two kinds of timed delay, whose finish half
is only "return". The scope stays as decided, quick save and load anywhere.

Two things refuse a save at the safe point:

- **Last Raven's mission loop.** `0x00102018` is replaced (`fps_native_loop`
  and its Higher FPS loop). From the sortie on (poll 1500 of pause-look;
  play starts near 2050) the safe point sits under it, with Higher FPS on
  or off. Menus and the garage
  are clear. Stage 8 needs the registered resume this section already plans
  for it, at its frame start, `0x0010209C`.
- **A movie playing** (a movie context holding stream data): most of The 3rd
  Birthday's opening. As planned, a quick save waits for the next safe point
  and says why. The savedata dialog was never open at a census; a finished
  one (status 4) is only a status word.

### Resume entries: what they cost (stage 4)

`allegrexrecomp emit --resume` gives every call's return site a case in its
function's entry switch, without a dispatch thunk of its own. It also gives
each function holding one a `psp_resume_<addr>(site)`, and the module a
sorted table that `psp_resume_chain` climbs. `tests/test_resume.c` checks it
end to end on a three-deep chain interrupted inside a firmware call:
scrambled, restored and resumed natively, the chain ends in the
uninterrupted run's registers and memory, and so does the interpreter from
the same snapshot.

Emitted, all four titles:

| Title | Return sites | Switch cases | Dispatch thunks | Generated C |
|---|---|---|---|---|
| The 3rd Birthday | 61,261 | 62,311 → 121,732 | 51,913, unchanged | 107.5 → 115.0 MB |
| Last Raven | 50,323 | 52,333 → 102,817 | 43,324, unchanged | 78.4 → 84.8 MB |
| AC3 Portable | 35,042 | 40,996 → 76,061 | 34,264, unchanged | 61.2 → 65.6 MB |
| Silent Line | 39,895 | 42,862 → 83,030 | 35,476, unchanged | 65.4 → 70.5 MB |

Built at `-O2` the way the games' own scripts split it, and replayed
headless. Instructions are counted with `perf_event_open`; the machine was
busy, so wall time is not used.

| Title | Compile CPU | Code | Resume table | Replay instructions |
|---|---|---|---|---|
| The 3rd Birthday | 466 → 496 s (+6.5%) | 29.6 → 31.5 MB (+6.1%) | 0.98 MB | +0.010% (gameplay, 239.3 billion; three runs each) |
| Last Raven | 314 → 330 s (+5.3%) | 20.2 → 21.9 MB (+8.0%) | 0.81 MB | +0.002% (mission-effects, 2,832 billion; two runs each) |

Every replay ended identically with and without resume entries. The
measured cost is mostly in the build. In play it is within the noise of the
replays themselves. So resume entries can be the default when stage 8 needs
them.

### What a state holds

**Header:**

- magic, format version;
- toolkit commit and **emitted-module hash**: a state loads only into the
  build that wrote it;
- title slug, guest time, poll count;
- a 480x272 thumbnail.

**Chunks**, tagged and versioned per module:

- guest RAM (32 MB), VRAM (2 MB, after a forced synchronous GL readback),
  the module image (6 MB for Last Raven; it shadows the scratchpad);
- every scheduler slot's register file and state;
- thread manager, kernel-object, lock, timer, sysmem and interrupt tables.
  Host pointers (`psp_waitq *`, `ge_queue *`, FPL/TLS heap pointers) are
  written as indices;
- clock, display, controller state including the replay cursor;
- SAS and audio channels;
- ATRAC (plain state; the decoder is reopened from its extradata and
  flushed);
- the GE queue, event ring, timeline and registers. This extends
  `psp_ge_state_save`, which only covers registers today;
- file descriptors as path, base, length and position. A path field has to
  be added; descriptors keep a `FILE*` and nothing else;
- a game-host chunk that titles register, holding Last Raven's replacement
  statics and its fps camera and clock state. Pose history can be reset
  instead of saved.

**Re-created, not saved:** host threads, locks, `FILE*`s, codecs, the dispatch
and firmware registries, host callbacks, the GL context and caches (every
write generation is bumped so textures re-import), audio rings, the clock
anchor.

**Size and compression.** About 40 MB raw before compression. Measure what
zero-page skipping alone achieves before adding a compression dependency.

**Load at launch (stage 8).** `--load-state <file>` boots the module and
registers it as usual, skips `module_start`, restores, spawns each thread in
resume mode and re-anchors the clock.

**The gate is deterministic, and it is the whole argument.** Run a replay
headless (unpaced, synthetic clock) and save at poll N. Then either:

- continue to M, or
- launch fresh, load the state and run the same replay tail to M.

Lists, commands, bad accesses, input logs and the final framebuffer bytes
must be identical. Do this at several N per title: menu, garage, mission,
mid-boost.

**Quick load in-process (stage 9).**

1. Every guest host thread unwinds to the base of its `thread_main` by
   `longjmp`. The thread record already has an `unwind` buffer for
   `sceKernelExitThread`.
2. The loader, which is the GE-owner thread at its safe point, restores
   state.
3. Each slot resumes on a host thread: the existing ones are reused, missing
   ones are spawned, and surplus ones exit.

Because the loader is the GE-owner thread and comes back as the same guest
slot, the GL context never changes thread. That rule is what the GL backend
relies on.

**What players should know.** A state does not roll back memory-stick saves.
States are tied to one build.

**Gates.** The stage 8 rows again, but loaded in-process, 100 times in a
loop with no growth in threads or memory, and in both renderers. In
resolution mode a restored render target is rebuilt from the 1x VRAM copy,
so the first frame after a load may be softer on persistent targets. That
is expected and should be noted, not fixed.

## Testing, all stages

- **Deterministic rows first.** Every stage has a headless, unpaced gate
  that compares against rows recorded before it. Windowed checks come
  second, and are written down as such.
- **The autotest sweep, diffed per test,** after any change to
  `threadman.c`, `kernobj.c`, `kernlock.c` or `sched.c` (stage 4's wait
  splitting). A wait that changes conformance blocks the stage.
- **Packaged checks** follow last-raven's `PACKAGING.md`:
  - relocation, read-only app contents, XDG paths and migration;
  - the ELF/ABI audit;
  - real-ISO imports, kept as private validation outside release staging.
- **Real-controller and Deck checks** follow the save dialog's acceptance
  list (`docs/SAVEDATA-UI.md` in last-raven).

## Decided (4 Oct, with Sif)

1. **Save states: quick save and load anywhere.** This is stages 4, 8 and 9.
   - Suspend-on-quit alone was the narrower option.
   - Deferring save states was the third.

   The staging still applies: if stage 4's census shows the waits cost too
   much, the work stops at load-at-launch and the scope is reopened.
2. **The menu button:** pressing View+Start opens the menu.
   - Quit moves into the menu, retiring the 2 s hold-to-quit chord.
   - Ctrl+Shift+Q stays.
   - Not chosen: Guide (Steam often claims it) and L3+R3.
3. **The launcher: rebuilt on the ImGui pages later** (stage 10), after the
   overlay exists. Keeping its SDL_Renderer UI indefinitely was the other
   option.
4. **The player lives in psprecomp, and each game's repository keeps its
   pack** (§1). Two alternatives were not chosen:
   - an umbrella repository submoduling psprecomp and every game;
   - growing Last Raven's packaging into the umbrella, which would have made
     The 3rd Birthday depend on Last Raven.

## Not doing

- Fast-forward or rewind. The game paces against guest time. Fast-forward
  is the higher-frame-rate problem in another form (last-raven
  `docs/HIGH-FRAMERATE-RESEARCH.md`), and rewind is save states taken every
  frame.
- Save states across builds. They need a format that survives renamed
  emitted functions, and nothing needs that yet.
- Loading a save state while a movie or a system dialog is up. These are
  refused rather than serialised.
- Running ImGui on the GE thread. It would put SDL video calls on a thread
  that did not initialise video.
- Gyro and touchpad bindings. The binding model leaves room for them; no
  title needs them.
- A plugin interface for packs, meaning precompiled title code loaded at run
  time. Packs are source compiled with the player's own headers, so the
  interface can change freely between player builds.

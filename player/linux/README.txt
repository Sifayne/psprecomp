psprecomp — Linux x86-64 test build

psprecomp plays PSP games recompiled to run natively, prepared on your own
computer from your own discs. No game code or assets are included.

Make the AppImage executable, then open it. On Steam Deck, do the first setup
in Desktop Mode.

1. Add a game. Choose Add game, browse to your PSP ISO and choose it. Drives
   opens removable media locations. You can also drop an ISO on the window.
   Keep the app open while preparation runs; then choose Save and play.
2. Add its pack, if it has one. A pack adds what makes a game better than
   the plain recompiled one -- controls, wider views, higher frame rates,
   fixes -- and its own settings. In the launcher, open Packs and choose
   Add pack..., then the pack's .zip file. It is built for this computer as
   it is added. A game played plain before its pack was added becomes the
   pack's, with its saves; prepare it once more.

A game with no pack plays as the plain recompiled game: what the recompiler
makes of the disc, with no additions. It may not work as well as a game with
a pack, or at all. A pack supports exact executable versions; another
version of its game plays plain.

The settings under All games apply to every game; each pack adds its own
pages for its games. In the game, Escape or View + Menu opens the menu: save
states (F5 saves, F9 loads), bindings, settings and Quit.

Moving from a game's own app (such as Armored Core Portable or The 3rd
Birthday): add its pack. Its saves and save states are copied into
psprecomp's folders, and its prepared games, with where their ISOs are,
come along; its settings are brought in the first time the launcher shows
that pack. The earlier app's folders keep their saves and a note of what
came over. Prepare each game once with the new app.

The app includes its preparation tools, C compiler, Python runtime and media
libraries. No compiler, Python or FFmpeg installation is needed. First setup
is CPU intensive and needs at least 2 GB of available library space. Leave the
ISO in an accessible location; the game reads its assets from that file.

Launcher-only updates keep prepared games ready. Changes to a pack's game
fixes rebuild only the affected titles; runtime/compiler changes can require
rebuilding all games. Existing ISO locations, settings and per-game saves are
preserved. If an ISO moves, use Add game to select it again; a verified
prepared game can be reused. Cancellation or a failed import preserves
existing installations and saves.

Default persistent locations (XDG overrides are supported):
  Settings: ~/.config/psprecomp/settings.ini
  Packs:    ~/.local/share/psprecomp/packs
  Games:    ~/.local/share/psprecomp/games
  Saves:    ~/.local/share/psprecomp/saves/<game>/ms/PSP/SAVEDATA
  States:   ~/.local/share/psprecomp/states/<game>
  Logs:     ~/.local/state/psprecomp/logs

To play from more than one computer, keep the Saves folder the same on each
with any folder-sync tool, and play on one machine at a time. Save states are
tied to the build that made them and are not worth syncing.

For Gaming Mode, add the AppImage as a non-Steam game after Desktop Mode setup.
Use the native Linux executable; no Proton compatibility override is needed.
The launcher and game accept Steam Input's virtual gamepad automatically.

Command-line options: --help, --print-paths, --add-pack /path/to/pack.zip,
--remove-pack ID, --list-packs, --import /path/to/game.iso, --list-games,
--check-startup.

This software uses FFmpeg under the LGPL v2.1 or later: https://ffmpeg.org/
Exact source and rebuild instructions accompany the AppImage in the matching
sources archive. Libraries remain separate; compatible replacement libraries
can be supplied using LD_LIBRARY_PATH or in an extracted AppDir. See licenses/.
pspdecrypt is a separate GPLv3 helper; its corresponding source is included.
The icon is the unchanged GNOME Adwaita application icon, CC BY-SA 3.0 US.

This is a test build. A successful launcher startup does not by itself verify
gameplay, audio or save behavior on a particular device.

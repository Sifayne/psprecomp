# Dear ImGui

Vendored from [ocornut/imgui](https://github.com/ocornut/imgui) release
`v1.92.9b` (31 Jul 2026): the tag's tarball,
`codeload.github.com/ocornut/imgui/tar.gz/refs/tags/v1.92.9b`,
SHA-256 `21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99`.
Only the core (`imgui*.cpp`, `imgui*.h`, `imconfig.h`, `imstb_*.h`), the
SDL2 platform backend and the SDL_Renderer2 backend are kept, unmodified.
MIT license, in `LICENSE.txt`.

It draws the player's in-game overlay (docs/PLAYER-LAYER.md §4). The host
reaches it only through `src/host/ui.cpp`'s C API, and compiles it with
`-fno-exceptions -fno-rtti -fno-threadsafe-statics`, which leaves it needing
nothing of the C++ runtime: a plain C link, including the player's
on-device `zig cc`, links it as it is.

# In Libertas - exsurge, two can play fake latin.

A GUI editor for .spc charts and a hook for the game In Falsus by lowiro. This repository does not supply any game assets nor extracted .spc charts from the game itself. In order to play the charts you need to own a copy of the game.

## Getting started

- Install [Melon Loader](https://github.com/LavaGang/MelonLoader/releases) into your game directory, either directly or with GUI.
- Run the game with Melon Loader installed, let it generate the configs necessary.
- Download [latest release](include actual link later) or build yourself both hook and the editor.
- Copy dlls provided into Mods folder created by Melon Loader in the game folder.
- Create a `CustomCharts` folder inside your game folder and put chart folders there, they should appear ingame on your next launch if done correctly.

In order to make custom charts launch the editor and follow [User Manual](docs/MANUAL.md).

## Building

### Requirements

- CMake 3.24 or newer.
- Visual Studio 2022 with the Desktop development with C++ workload and x64 tools.
- The .NET 6 SDK.
- [vcpkg](https://github.com/microsoft/vcpkg) at `C:\vcpkg\vcpkg`, with the Qt 6 Core, Gui, Multimedia, OpenGL, OpenGLWidgets, and Widgets components installed for `x64-windows`.
- An In Falsus installation with MelonLoader, so the hook project can reference its generated IL2CPP assemblies.

Run `tools\build.ps1`. The editor and its runtime files are written to
`build\In Libertas`; the managed hook and native jacket resolver bridge are
written to `build\Mods`.

## Credits

The repository bundles [audiowaveform](https://github.com/bbc/audiowaveform) in `tools\audiowaveform` for waveform generation.

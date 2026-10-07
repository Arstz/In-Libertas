# In Libertas - exsurge, two can play fake latin.

A GUI editor for .spc charts and a hook for the game In Falsus by lowiro. This repository does not supply any game assets nor extracted .spc charts from the game itself. In order to play the charts you need to own a copy of the game. For fast and direct communication use [discord server](https://discord.gg/MrtxtVvYuZ), I read the issues here as well.

## Getting started

- Install [Melon Loader](https://github.com/LavaGang/MelonLoader/releases) into your game directory, either directly or with GUI.
- Run the game with Melon Loader installed, let it generate the configs necessary.
- Download [latest release](https://github.com/Arstz/In-Libertas/releases/) or build yourself both hook and the editor.
- Copy dlls provided into Mods folder created by Melon Loader in the game folder.
- Create a `CustomCharts` folder inside your game folder and put chart folders there, they should appear ingame on your next launch if done correctly.

In order to make custom charts launch the editor and follow [User Manual](docs/MANUAL.md).

## Building

### Requirements

- CMake 3.24 or newer.
- Visual Studio 2022 with the Desktop development with C++ workload and x64 tools.
- The .NET 6 SDK.
- [vcpkg](https://github.com/microsoft/vcpkg), with Qt 6.10 or newer (Core, Gui, Multimedia, OpenGL, OpenGLWidgets, and Widgets) installed for `x64-windows`. Qt Multimedia must use its FFmpeg backend.
- Qt's JPEG and GIF plugins, and `qtimageformats[core,webp]:x64-windows` for WebP. The build also deploys Qt's ICO and TGA plugins; BMP and PNG are built into Qt Gui.
- FFmpeg with the stable libvorbis encoder enabled (`vcpkg install "ffmpeg[vorbis]:x64-windows" --recurse`) for Ogg Vorbis export. No separate `ffmpeg.exe` or audiowaveform executable is needed.
- An In Falsus installation with MelonLoader, so the hook project can reference its generated IL2CPP assemblies.

Run `tools\build.ps1`. The editor and its runtime files are written to
`build\In Libertas`; the managed hook and native jacket resolver bridge are
written to `build\Mods`.

Run `ctest --test-dir build -C Release --output-on-failure` after building to
check media decoding, normalized jacket export, and Ogg Vorbis audio export.

## Credits

The editor uses [Qt](https://www.qt.io/) and its
[FFmpeg](https://ffmpeg.org/) multimedia backend for audio playback, waveform
generation, timing analysis, and export. Ogg Vorbis encoding uses
[libvorbis and libogg](https://xiph.org/). JPEG and WebP support use Qt's plugins
and their [libjpeg-turbo](https://libjpeg-turbo.org/) and
[WebP](https://developers.google.com/speed/webp) codecs.

The repository also retains the legacy bundled
[audiowaveform](https://github.com/bbc/audiowaveform) binary; the editor no longer
requires or uses it.

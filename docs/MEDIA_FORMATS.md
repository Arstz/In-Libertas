# Media formats

Audio playback, waveform generation, timing analysis, and export use Qt
Multimedia's FFmpeg backend. No command-line media tools are required at runtime.
Audio import and folder discovery use the same capability-filtered list of
common formats. Imported audio must pass a decoding check.

Export writes `audio.ogg`. Ogg Vorbis sources are validated and preserved;
other encodings are converted to stereo, 48 kHz Vorbis at a target 320 kb/s.
The encoder requires FFmpeg's stable libvorbis support. Export is cancellable
and commits the audio file atomically. Projects retain their original media.

The jacket picker reflects the deployed Qt image decoders. The build includes
PNG, JPEG, WebP, BMP, GIF, ICO, TGA, and Qt's built-in portable bitmap formats.
Required codec DLLs are bundled with the editor. Additional image codec
modules are not required.

Jackets use a common file and memory decoding path with automatic orientation
and sRGB color conversion. A jacket is a still image, using the first decoded
frame. Export normalizes both jacket files to non-interlaced 8-bit RGBA PNG.

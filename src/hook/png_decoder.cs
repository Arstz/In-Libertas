using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;

namespace InFalsusCustomSongHook;

internal sealed class DecodedPng
{
    internal DecodedPng(int width, int height, byte[] rgbaBottomUp)
    { Width = width; Height = height; RgbaBottomUp = rgbaBottomUp; }
    internal int Width { get; }
    internal int Height { get; }
    internal byte[] RgbaBottomUp { get; }
}

/// <summary>Minimal, strict PNG decoder for non-interlaced 8-bit jacket art.</summary>
internal static class PngDecoder
{
    private static readonly byte[] Signature = { 137, 80, 78, 71, 13, 10, 26, 10 };

    internal static DecodedPng Decode(byte[] source)
    {
        if (source is null || source.Length < Signature.Length + 12)
            throw new InvalidDataException("PNG is empty or truncated.");
        for (int index = 0; index < Signature.Length; index++)
            if (source[index] != Signature[index]) throw new InvalidDataException("Jacket is not a PNG file.");

        int cursor = Signature.Length;
        int width = 0, height = 0, bitDepth = 0, colorType = -1, interlace = -1;
        byte[] palette = null;
        byte[] paletteAlpha = null;
        List<byte> compressed = new();
        while (cursor + 12 <= source.Length)
        {
            int length = ReadInt32(source, cursor); cursor += 4;
            if (length < 0 || cursor + 8L + length > source.Length) throw new InvalidDataException("PNG chunk exceeds file bounds.");
            uint type = ReadUInt32(source, cursor); cursor += 4;
            int payload = cursor; cursor += length;
            cursor += 4; // CRC; Unity's files are trusted after the bounds check.
            switch (type)
            {
                case 0x49484452: // IHDR
                    if (length != 13 || width != 0) throw new InvalidDataException("PNG IHDR is invalid.");
                    width = ReadInt32(source, payload);
                    height = ReadInt32(source, payload + 4);
                    bitDepth = source[payload + 8];
                    colorType = source[payload + 9];
                    if (source[payload + 10] != 0 || source[payload + 11] != 0) throw new InvalidDataException("Unsupported PNG compression or filter method.");
                    interlace = source[payload + 12];
                    break;
                case 0x504C5445: // PLTE
                    palette = Slice(source, payload, length);
                    break;
                case 0x74524E53: // tRNS
                    paletteAlpha = Slice(source, payload, length);
                    break;
                case 0x49444154: // IDAT
                    for (int index = 0; index < length; index++) compressed.Add(source[payload + index]);
                    break;
                case 0x49454E44: // IEND
                    cursor = source.Length;
                    break;
            }
        }

        if (width <= 0 || height <= 0 || bitDepth != 8 || interlace != 0 || compressed.Count == 0)
            throw new InvalidDataException("Only non-interlaced 8-bit PNG jacket art is supported.");
        int channels = colorType switch { 0 => 1, 2 => 3, 3 => 1, 4 => 2, 6 => 4, _ => 0 };
        if (channels == 0 || (colorType == 3 && (palette is null || palette.Length % 3 != 0)))
            throw new InvalidDataException("PNG colour format is unsupported.");
        if ((long)width * height > 67_108_864) throw new InvalidDataException("PNG dimensions are unreasonably large.");

        int stride = checked(width * channels);
        byte[] filtered = Inflate(compressed.ToArray(), checked((stride + 1) * height));
        byte[] rgba = new byte[checked(width * height * 4)];
        byte[] previous = new byte[stride];
        byte[] current = new byte[stride];
        int input = 0;
        for (int row = 0; row < height; row++)
        {
            int filter = filtered[input++];
            Buffer.BlockCopy(filtered, input, current, 0, stride); input += stride;
            Unfilter(current, previous, filter, channels);
            int targetRow = height - 1 - row; // Texture2D raw data starts at the bottom row.
            for (int x = 0; x < width; x++) WritePixel(current, x * channels, rgba, (targetRow * width + x) * 4, colorType, palette, paletteAlpha);
            (previous, current) = (current, previous);
        }
        return new DecodedPng(width, height, rgba);
    }

    private static byte[] Inflate(byte[] compressed, int expectedLength)
    {
        using MemoryStream input = new(compressed, false);
        using ZLibStream zlib = new(input, CompressionMode.Decompress);
        byte[] result = new byte[expectedLength];
        int offset = 0;
        while (offset < result.Length)
        {
            int read = zlib.Read(result, offset, result.Length - offset);
            if (read == 0) throw new InvalidDataException("PNG pixel data ended early.");
            offset += read;
        }
        if (zlib.ReadByte() >= 0) throw new InvalidDataException("PNG pixel data exceeds its declared dimensions.");
        return result;
    }

    private static void Unfilter(byte[] current, byte[] previous, int filter, int bpp)
    {
        for (int index = 0; index < current.Length; index++)
        {
            int left = index >= bpp ? current[index - bpp] : 0;
            int above = previous[index];
            int upperLeft = index >= bpp ? previous[index - bpp] : 0;
            current[index] = filter switch
            {
                0 => current[index],
                1 => (byte)(current[index] + left),
                2 => (byte)(current[index] + above),
                3 => (byte)(current[index] + ((left + above) >> 1)),
                4 => (byte)(current[index] + Paeth(left, above, upperLeft)),
                _ => throw new InvalidDataException("PNG contains an unknown row filter."),
            };
        }
    }

    private static int Paeth(int left, int above, int upperLeft)
    {
        int estimate = left + above - upperLeft;
        int dl = Math.Abs(estimate - left), da = Math.Abs(estimate - above), du = Math.Abs(estimate - upperLeft);
        return dl <= da && dl <= du ? left : da <= du ? above : upperLeft;
    }

    private static void WritePixel(byte[] row, int source, byte[] rgba, int target, int colorType, byte[] palette, byte[] alpha)
    {
        switch (colorType)
        {
            case 0: rgba[target] = rgba[target + 1] = rgba[target + 2] = row[source]; rgba[target + 3] = 255; return;
            case 2: rgba[target] = row[source]; rgba[target + 1] = row[source + 1]; rgba[target + 2] = row[source + 2]; rgba[target + 3] = 255; return;
            case 3:
                int paletteIndex = row[source];
                if (paletteIndex * 3 + 2 >= palette.Length) throw new InvalidDataException("PNG palette index is outside PLTE.");
                rgba[target] = palette[paletteIndex * 3]; rgba[target + 1] = palette[paletteIndex * 3 + 1]; rgba[target + 2] = palette[paletteIndex * 3 + 2];
                rgba[target + 3] = alpha is not null && paletteIndex < alpha.Length ? alpha[paletteIndex] : (byte)255; return;
            case 4: rgba[target] = rgba[target + 1] = rgba[target + 2] = row[source]; rgba[target + 3] = row[source + 1]; return;
            case 6: rgba[target] = row[source]; rgba[target + 1] = row[source + 1]; rgba[target + 2] = row[source + 2]; rgba[target + 3] = row[source + 3]; return;
            default: throw new InvalidDataException("PNG colour format is unsupported.");
        }
    }

    private static int ReadInt32(byte[] bytes, int offset) => unchecked((int)ReadUInt32(bytes, offset));
    private static uint ReadUInt32(byte[] bytes, int offset) => (uint)(bytes[offset] << 24 | bytes[offset + 1] << 16 | bytes[offset + 2] << 8 | bytes[offset + 3]);
    private static byte[] Slice(byte[] bytes, int offset, int length) { byte[] result = new byte[length]; Buffer.BlockCopy(bytes, offset, result, 0, length); return result; }
}

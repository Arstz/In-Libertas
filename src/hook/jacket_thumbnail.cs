using System;
using System.Buffers.Binary;
using System.IO;
using System.IO.Compression;
using System.Text;

namespace InFalsusCustomSongHook;

internal static class JacketThumbnail {
    internal const int kExtent = 256;
    private const int kChannels = 4;
    private const uint kPngPolynomial = 3988292384;
    private static readonly byte[] kSignature = { 137, 80, 78, 71, 13, 10, 26, 10 };

    private readonly struct SampleRange {
        internal readonly int First;
        internal readonly int Last;
        private readonly bool m_reduce;
        private readonly double m_start;
        private readonly double m_end;

        internal SampleRange(int sourceExtent, int targetIndex) {
            m_reduce = sourceExtent > kExtent;
            if (m_reduce) {
                m_start = targetIndex * (double)sourceExtent / kExtent;
                m_end = (targetIndex + 1) * (double)sourceExtent / kExtent;
                First = (int)Math.Floor(m_start);
                Last = Math.Min((int)Math.Ceiling(m_end) - 1, sourceExtent - 1);
            } else {
                m_start = Math.Clamp((targetIndex + 0.5) * sourceExtent / kExtent - 0.5, 0, sourceExtent - 1);
                m_end = m_start;
                First = (int)Math.Floor(m_start);
                Last = Math.Min(First + 1, sourceExtent - 1);
            }
        }

        internal double Weight(int index) {
            return m_reduce
                ? Math.Max(0, Math.Min(m_end, index + 1) - Math.Max(m_start, index)) / (m_end - m_start)
                : Math.Max(0, 1 - Math.Abs(index - m_start));
        }
    }

    internal static byte[] Create(byte[] jacket) {
        DecodedPng source = PngDecoder.Decode(jacket);
        byte[] pixels = new byte[kExtent * kExtent * kChannels];
        SampleRange[] horizontal = new SampleRange[kExtent];
        SampleRange[] vertical = new SampleRange[kExtent];
        for (int index = 0; index < kExtent; index++) {
            horizontal[index] = new SampleRange(source.Width, index);
            vertical[index] = new SampleRange(source.Height, index);
        }
        for (int y = 0; y < kExtent; y++) {
            for (int x = 0; x < kExtent; x++) {
                SampleRange columns = horizontal[x], rows = vertical[y];
                int target = (y * kExtent + x) * kChannels;
                double red = 0, green = 0, blue = 0, alpha = 0;
                for (int row = rows.First; row <= rows.Last; row++) {
                    double rowWeight = rows.Weight(row);
                    for (int column = columns.First; column <= columns.Last; column++) {
                        int offset = (row * source.Width + column) * kChannels;
                        double weightedAlpha = rowWeight * columns.Weight(column) * source.RgbaBottomUp[offset + 3];
                        red += weightedAlpha * source.RgbaBottomUp[offset];
                        green += weightedAlpha * source.RgbaBottomUp[offset + 1];
                        blue += weightedAlpha * source.RgbaBottomUp[offset + 2];
                        alpha += weightedAlpha;
                    }
                }
                if (alpha > 0) {
                    pixels[target] = ToByte(red / alpha);
                    pixels[target + 1] = ToByte(green / alpha);
                    pixels[target + 2] = ToByte(blue / alpha);
                }
                pixels[target + 3] = ToByte(alpha);
            }
        }

        return Encode(new DecodedPng(kExtent, kExtent, pixels));
    }

    private static byte ToByte(double value) {
        return (byte)Math.Clamp(Math.Round(value, MidpointRounding.AwayFromZero), 0, byte.MaxValue);
    }

    internal static byte[] Encode(DecodedPng image) {
        using MemoryStream output = new();
        using MemoryStream compressed = new();
        byte[] header = new byte[13];
        int stride = checked(image.Width * kChannels);
        if (image.Width <= 0 || image.Height <= 0 || image.RgbaBottomUp.Length != checked(stride * image.Height))
            throw new InvalidDataException("Invalid jacket pixel dimensions.");
        BinaryPrimitives.WriteInt32BigEndian(header.AsSpan(0, 4), image.Width);
        BinaryPrimitives.WriteInt32BigEndian(header.AsSpan(4, 4), image.Height);
        header[8] = 8;
        header[9] = 6;
        using (ZLibStream zlib = new(compressed, CompressionLevel.Optimal, true)) {
            for (int row = image.Height - 1; row >= 0; row--) {
                zlib.WriteByte(0);
                zlib.Write(image.RgbaBottomUp, row * stride, stride);
            }
        }
        output.Write(kSignature);
        WriteChunk(output, "IHDR", header);
        WriteChunk(output, "IDAT", compressed.ToArray());
        WriteChunk(output, "IEND", Array.Empty<byte>());

        return output.ToArray();
    }

    private static void WriteChunk(Stream output, string name, byte[] data) {
        byte[] type = Encoding.ASCII.GetBytes(name);
        Span<byte> number = stackalloc byte[4];
        uint crc = uint.MaxValue;
        BinaryPrimitives.WriteInt32BigEndian(number, data.Length);
        output.Write(number);
        output.Write(type);
        output.Write(data);
        foreach (byte value in type) crc = UpdateCrc(crc, value);
        foreach (byte value in data) crc = UpdateCrc(crc, value);
        BinaryPrimitives.WriteUInt32BigEndian(number, ~crc);
        output.Write(number);
    }

    private static uint UpdateCrc(uint crc, byte value) {
        crc ^= value;
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) == 0 ? 0 : kPngPolynomial);

        return crc;
    }
}

using System;

namespace InFalsusCustomSongHook;

internal static class JacketThumbnail {
    internal const int kExtent = 256;
    private const int kChannels = 4;

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
        DecodedJacket source = JacketCodec.Decode(jacket);
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

        return Encode(new DecodedJacket(kExtent, kExtent, pixels));
    }

    private static byte ToByte(double value) {
        return (byte)Math.Clamp(Math.Round(value, MidpointRounding.AwayFromZero), 0, byte.MaxValue);
    }

    internal static byte[] Encode(DecodedJacket image) => JacketCodec.Encode(image);
}

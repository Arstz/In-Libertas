using System;
using System.IO;

namespace InFalsusCustomSongHook;

internal static class PackJacketMask {
    private const int kChannels = 4;
    private const int kAlphaChannel = 3;
    private const int kOpaqueAlpha = 255;

    internal static DecodedJacket Apply(DecodedJacket artwork, DecodedJacket mask) {
        Validate(artwork);
        Validate(mask);
        byte[] pixels = new byte[mask.RgbaBottomUp.Length];
        double scale = Math.Max((double)mask.Width / artwork.Width, (double)mask.Height / artwork.Height);
        double offsetX = (artwork.Width - mask.Width / scale) / 2;
        double offsetY = (artwork.Height - mask.Height / scale) / 2;

        for (int y = 0; y < mask.Height; y++) {
            double sourceY = Math.Clamp(offsetY + (y + 0.5) / scale - 0.5, 0, artwork.Height - 1);
            int firstY = (int)sourceY;
            int secondY = Math.Min(firstY + 1, artwork.Height - 1);
            double blendY = sourceY - firstY;
            for (int x = 0; x < mask.Width; x++) {
                double sourceX = Math.Clamp(offsetX + (x + 0.5) / scale - 0.5, 0, artwork.Width - 1);
                int firstX = (int)sourceX;
                int secondX = Math.Min(firstX + 1, artwork.Width - 1);
                int destination = (y * mask.Width + x) * kChannels;
                int bottomLeft = (firstY * artwork.Width + firstX) * kChannels;
                int bottomRight = (firstY * artwork.Width + secondX) * kChannels;
                int topLeft = (secondY * artwork.Width + firstX) * kChannels;
                int topRight = (secondY * artwork.Width + secondX) * kChannels;
                double blendX = sourceX - firstX;
                for (int channel = 0; channel < kChannels; channel++) {
                    double bottom = artwork.RgbaBottomUp[bottomLeft + channel] * (1 - blendX)
                        + artwork.RgbaBottomUp[bottomRight + channel] * blendX;
                    double top = artwork.RgbaBottomUp[topLeft + channel] * (1 - blendX)
                        + artwork.RgbaBottomUp[topRight + channel] * blendX;
                    double value = bottom * (1 - blendY) + top * blendY;
                    if (channel == kAlphaChannel)
                        value *= (double)mask.RgbaBottomUp[destination + kAlphaChannel] / kOpaqueAlpha;
                    pixels[destination + channel] = (byte)Math.Clamp((int)Math.Round(value), 0, kOpaqueAlpha);
                }
            }
        }

        return new DecodedJacket(mask.Width, mask.Height, pixels);
    }

    private static void Validate(DecodedJacket image) {
        if (image is null || image.Width <= 0 || image.Height <= 0 || image.RgbaBottomUp is null
            || (long)image.Width * image.Height * kChannels != image.RgbaBottomUp.Length)
            throw new InvalidDataException("Pack jacket pixel data is invalid.");
    }
}

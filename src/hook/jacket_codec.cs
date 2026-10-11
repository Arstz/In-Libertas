using System;
using System.IO;
using System.Runtime.InteropServices;

namespace InFalsusCustomSongHook;

internal static class JacketCodec {
    private const string kLibrary = "InLibertasJacketCodec";

    [DllImport(kLibrary, CallingConvention = CallingConvention.Cdecl, EntryPoint = "JacketDecode")]
    private static extern IntPtr DecodeNative(byte[] data, UIntPtr size, out int width, out int height);

    [DllImport(kLibrary, CallingConvention = CallingConvention.Cdecl, EntryPoint = "JacketEncode")]
    private static extern IntPtr EncodeNative(byte[] rgba, int width, int height, int stride, out UIntPtr size);

    [DllImport(kLibrary, CallingConvention = CallingConvention.Cdecl, EntryPoint = "JacketFree")]
    private static extern void FreeNative(IntPtr data);

    internal static DecodedJacket Decode(byte[] data) {
        if (data is null) throw new InvalidDataException("Jacket is empty.");
        if (data.Length >= 8 && data[0] == 137 && data[1] == 80 && data[2] == 78 && data[3] == 71)
            return PngDecoder.Decode(data);
        return DecodeWebP(data);
    }

    internal static DecodedJacket DecodeWebP(byte[] data) {
        IntPtr pixels = DecodeNative(data, (UIntPtr)data.Length, out int width, out int height);
        if (pixels == IntPtr.Zero)
            throw new InvalidDataException("Jacket must be a still lossless WebP within 2048 by 2048 pixels.");
        try {
            int stride = checked(width * 4);
            byte[] rgba = new byte[checked(stride * height)];
            for (int row = 0; row < height; row++)
                Marshal.Copy(IntPtr.Add(pixels, row * stride), rgba, (height - row - 1) * stride, stride);
            return new DecodedJacket(width, height, rgba);
        } finally {
            FreeNative(pixels);
        }
    }

    internal static byte[] Encode(DecodedJacket image) {
        if (image.Width <= 0 || image.Width > 2048 || image.Height <= 0 || image.Height > 2048
            || image.RgbaBottomUp.Length != checked(image.Width * image.Height * 4))
            throw new InvalidDataException("Invalid jacket pixel dimensions.");
        int stride = image.Width * 4;
        byte[] rgba = new byte[image.RgbaBottomUp.Length];
        for (int row = 0; row < image.Height; row++)
            Buffer.BlockCopy(image.RgbaBottomUp, row * stride, rgba, (image.Height - row - 1) * stride, stride);
        IntPtr encoded = EncodeNative(rgba, image.Width, image.Height, stride, out UIntPtr size);
        if (encoded == IntPtr.Zero) throw new InvalidDataException("Could not encode the lossless WebP jacket.");
        try {
            byte[] data = new byte[checked((int)size.ToUInt64())];
            Marshal.Copy(encoded, data, 0, data.Length);
            return data;
        } finally {
            FreeNative(encoded);
        }
    }
}

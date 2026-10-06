using System;
using System.IO;
using HarmonyLib;
using Il2Cpp_b;
using Il2CppInterop.Runtime.InteropTypes.Arrays;
using Il2Cpp_n;

namespace InFalsusCustomSongHook;

/// <summary>
/// Supplies a decoded external ICP1 chart for either the injected chart key or
/// an existing stock carrier key. Carrier mode has no SongData dependency.
/// </summary>
[HarmonyPatch(typeof(_S), nameof(_S._Gab))]
internal static class CustomChartOverride
{
    private static readonly object Gate = new();
    private static readonly System.Collections.Generic.Dictionary<string, Il2CppStructArray<byte>> BytesByChartKey =
        new(StringComparer.OrdinalIgnoreCase);
    private static readonly System.Collections.Generic.HashSet<string> LoadAttempted = new(StringComparer.OrdinalIgnoreCase);
    [HarmonyPrefix]
    private static void Prefix(ref Il2CppSystem.ReadOnlySpan<byte> __0, ref string __1)
    {
        CustomSongConfig config = CustomSongInstaller.FindActiveSongByChartKey(__1);
        if (config is null) return;

        try
        {
            EnsureLoaded(config);
            if (BytesByChartKey.TryGetValue(config.ChartKey, out Il2CppStructArray<byte> bytes))
            {
                // Keep _bytes rooted for the process lifetime: the native parser
                // receives a span into this managed IL2CPP allocation.
                __0 = new Il2CppSystem.ReadOnlySpan<byte>(bytes);
                string parserKey = GetParserKey(config);
                // The protected ICP1 note fields are decoded with rolling state
                // seeded by this argument.  The external test payload is an
                // alamode0 payload, so parsing it as custom0 would make every
                // protected field after the header decode incorrectly.
                __1 = parserKey;
                CustomSongMod.Log.Msg($"[CustomSong] external chart selected: requested={config.ChartKey}; parserKey={parserKey}; file={config.ExternalChartStem}; {__0.Length} bytes");
            }
        }
        catch (Exception exception)
        {
            // Preserve the template source span if an external file is invalid.
            CustomSongMod.Log.Error("[CustomSong] external chart unavailable: " + exception.Message);
        }
    }

    [HarmonyFinalizer]
    private static Exception Finalizer(string __1, Exception __exception)
    {
        CustomSongConfig config = CustomSongInstaller.FindActiveSongByChartKey(__1);
        if (__exception is not null && config is not null)
            CustomSongMod.Log.Error("[CustomSong] external parser threw: " + __exception);
        return __exception;
    }

    private static void EnsureLoaded(CustomSongConfig config)
    {
        lock (Gate)
        {
            if (LoadAttempted.Contains(config.ChartKey)) return;
            LoadAttempted.Add(config.ChartKey);

            if (string.IsNullOrWhiteSpace(config.ContentDirectory))
                throw new InvalidOperationException("Custom chart has no chart-folder content directory.");

            string spcPath = Path.Combine(config.ContentDirectory, config.ExternalChartStem + ".spc");
            string samPath = Path.Combine(config.ContentDirectory, config.ExternalChartStem + ".sam");
            if (!File.Exists(spcPath)) spcPath = null;
            if (!File.Exists(samPath)) samPath = null;
            byte[] payload;
            if (spcPath is not null)
                payload = File.ReadAllBytes(spcPath);
            else if (samPath is not null)
                payload = DecodeSam(File.ReadAllBytes(samPath));
            else
                throw new FileNotFoundException(
                    "Expected " + config.ExternalChartStem + ".spc or .sam in " +
                    config.ContentDirectory);

            ValidateAndTrimIcp1(ref payload);
            BytesByChartKey[config.ChartKey] = new Il2CppStructArray<byte>(payload);
            CustomSongMod.Log.Msg("[CustomSong] external chart payload loaded: " + (spcPath ?? samPath));
        }
    }

    private static byte[] DecodeSam(byte[] sam)
    {
        byte[] inner = new byte[sam.Length];
        const ulong kXorKey = 0xA5C39E7D4B2816F0UL; // XORing your payloads is not a good security measure since like 1970 
        for (int index = 0; index < sam.Length; index++)
            inner[index] = (byte)(sam[index] ^ (byte)(kXorKey >> (8 * (index & 7))));
        return inner;
    }

    private static string GetParserKey(CustomSongConfig config)
    {
        string parserChartId = config.ParserChartId;
        if (string.IsNullOrWhiteSpace(parserChartId))
            return config.ExternalChartStem + ".spc";
        return parserChartId + ".spc";
    }

    private static void ValidateAndTrimIcp1(ref byte[] bytes)
    {
        if (bytes.Length < 28 || bytes[0] != (byte)'I' || bytes[1] != (byte)'C' ||
            bytes[2] != (byte)'P' || bytes[3] != (byte)'1')
            throw new InvalidDataException("Custom chart is not an ICP1 payload.");

        uint noteCount = ReadUInt32LittleEndian(bytes, 12);
        uint eventCount = ReadUInt32LittleEndian(bytes, 16);
        long structuralLength = 28L + 80L * noteCount + 32L * eventCount;
        if (structuralLength < 28 || structuralLength > bytes.Length || structuralLength > int.MaxValue)
            throw new InvalidDataException($"Invalid ICP1 structural length {structuralLength} for {bytes.Length} bytes.");
        if (structuralLength != bytes.Length)
            Array.Resize(ref bytes, (int)structuralLength);
    }

    private static uint ReadUInt32LittleEndian(byte[] bytes, int offset) =>
        (uint)(bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24));
}

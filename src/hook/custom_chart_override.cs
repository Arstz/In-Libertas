using System;
using System.Collections.Generic;
using System.IO;
using HarmonyLib;
#if BEPINEX
using _b;
#else
using Il2Cpp_b;
#endif
using Il2CppInterop.Runtime.InteropTypes.Arrays;
#if BEPINEX
using _n;
#else
using Il2Cpp_n;
#endif

namespace InFalsusCustomSongHook;

/// <summary>
/// Supplies a decoded external ICP1 chart for either the injected chart key or
/// an existing stock carrier key. Carrier mode has no SongData dependency.
/// </summary>
[HarmonyPatch(typeof(_S), nameof(_S._Gab))]
internal static class CustomChartOverride
{
    private static readonly object Gate = new();
    private static Dictionary<string, Il2CppStructArray<byte>> BytesByChartKey =
        new(StringComparer.OrdinalIgnoreCase);
    [HarmonyPrefix]
    private static void Prefix(ref Il2CppSystem.ReadOnlySpan<byte> __0, ref string __1, out Il2CppStructArray<byte> __state)
    {
        __state = null;
        CustomSongConfig config = CustomSongInstaller.FindActiveSongByChartKey(__1);
        if (config is null) return;

        try
        {
            __state = GetPayload(config);
            __0 = new Il2CppSystem.ReadOnlySpan<byte>(__state);
            string parserKey = GetParserKey(config);
            __1 = parserKey;
            CustomSongMod.Log.Msg($"[CustomSong] external chart selected: requested={config.ChartKey}; parserKey={parserKey}; file={config.ExternalChartStem}; {__0.Length} bytes");
        }
        catch (Exception exception)
        {
            CustomSongMod.Log.Error("[CustomSong] external chart unavailable: " + exception.Message);
        }
    }

    [HarmonyFinalizer]
    private static Exception Finalizer(Exception __exception, Il2CppStructArray<byte> __state)
    {
        if (__exception is not null && __state is not null)
            CustomSongMod.Log.Error("[CustomSong] external parser threw: " + __exception);
        GC.KeepAlive(__state);
        return __exception;
    }

    private static Il2CppStructArray<byte> GetPayload(CustomSongConfig config)
    {
        lock (Gate)
        {
            if (!BytesByChartKey.TryGetValue(config.ChartKey, out Il2CppStructArray<byte> payload)) {
                payload = new Il2CppStructArray<byte>(ReadPayload(config));
                BytesByChartKey.Add(config.ChartKey, payload);
            }
            return payload;
        }
    }

    internal static Dictionary<string, Il2CppStructArray<byte>> PrepareLibrary(CustomSongConfig root) {
        Dictionary<string, Il2CppStructArray<byte>> payloads = new(StringComparer.OrdinalIgnoreCase);
        foreach (CustomSongCollection collection in root.Collections)
            foreach (CustomSongConfig song in collection.Songs)
                foreach (CustomSongConfig chart in song.ExpandDifficulties())
                    payloads.Add(chart.ChartKey, new Il2CppStructArray<byte>(ReadPayload(chart)));
        return payloads;
    }

    internal static void CommitLibrary(Dictionary<string, Il2CppStructArray<byte>> payloads) {
        lock (Gate) {
            BytesByChartKey = payloads;
        }
    }

    internal static void Reset() {
        lock (Gate) {
            BytesByChartKey = new(StringComparer.OrdinalIgnoreCase);
        }
    }

    private static byte[] ReadPayload(CustomSongConfig config) {
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
                "Expected " + config.ExternalChartStem + ".spc or .sam in " + config.ContentDirectory);

        ValidateAndTrimIcp1(ref payload);
        CustomSongMod.Log.Msg("[CustomSong] external chart payload loaded: " + (spcPath ?? samPath));
        return payload;
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

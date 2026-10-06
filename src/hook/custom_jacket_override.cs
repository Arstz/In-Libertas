using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using Il2CppInterop.Runtime.InteropTypes.Arrays;
using Il2Cppifapp.Game.Data;
using UnityEngine;
using UnityEngine.AddressableAssets;

namespace InFalsusCustomSongHook;

/// <summary>
/// Creates loose PNG material clones from the game's existing jacket assets.
/// The matching completed operations are returned by the private native jacket
/// builder; no ResourceManager provider, catalog, bundle, or runtime patch is
/// involved.
/// </summary>
internal static class CustomJacketOverride
{
    private static readonly object Gate = new();
    private static readonly Dictionary<string, RuntimeJacketPair> JacketsByPayload = new(StringComparer.OrdinalIgnoreCase);
    private static readonly List<DeferredJacketRecord> DeferredRecords = new();

    internal static bool ApplySelectorReferences(SongJacketEntry entry, CustomSongConfig config)
    {
        if (entry is null || config is null || !config.EnableLoosePngJackets) return false;
        return TryApply(entry.JacketLargeMaterial, entry.JacketSmallMaterial, config,
            (large, small) => { entry.JacketLargeMaterial = large; entry.JacketSmallMaterial = small; });
    }

    internal static bool ApplyChartReferences(SongDataChartJacketEntry entry, CustomSongConfig config)
    {
        if (entry is null || config is null || !config.EnableLoosePngJackets) return false;
        return TryApply(entry.JacketLargeMaterial, entry.JacketSmallMaterial, config,
            (large, small) => { entry.JacketLargeMaterial = large; entry.JacketSmallMaterial = small; });
    }

    internal static void QueueSelector(SongData songData, SongId songId, CustomSongConfig config) =>
        Queue(new DeferredJacketRecord(songData, songId, null, config));

    internal static void QueueChart(SongData songData, string chartId, CustomSongConfig config) =>
        Queue(new DeferredJacketRecord(songData, default, chartId, config));

    internal static void Tick()
    {
        lock (Gate)
        {
            for (int index = DeferredRecords.Count - 1; index >= 0; index--)
            {
                DeferredJacketRecord record = DeferredRecords[index];
                if (TryFinalize(record)) DeferredRecords.RemoveAt(index);
            }
        }
    }

    internal static void Dispose()
    {
        lock (Gate)
        {
            NativeJacketResolverOverride.Dispose();
            foreach (RuntimeJacketPair pair in JacketsByPayload.Values) pair.Dispose();
            JacketsByPayload.Clear();
            DeferredRecords.Clear();
        }
    }

    private static bool TryApply(
        AssetReferenceT<Material> templateLarge,
        AssetReferenceT<Material> templateSmall,
        CustomSongConfig config,
        Action<AssetReferenceT<Material>, AssetReferenceT<Material>> apply)
    {
        lock (Gate)
        {
            string identity = config.JacketLargePath + "\n" + config.JacketSmallPath;
            if (!JacketsByPayload.TryGetValue(identity, out RuntimeJacketPair pair))
            {
                if (!TryLoadTemplateMaterial(templateLarge, "large", out Material templateLargeMaterial) ||
                    !TryLoadTemplateMaterial(templateSmall, "small", out Material templateSmallMaterial))
                    return false;
                pair = new RuntimeJacketPair(
                    LoadMaterial(templateLargeMaterial, config.JacketLargePath),
                    LoadMaterial(templateSmallMaterial, config.JacketSmallPath));
                JacketsByPayload.Add(identity, pair);
            }

            if (!NativeJacketResolverOverride.TryConfigure(pair.Large.Material, pair.Small.Material,
                out AssetReferenceT<Material> largeMarker, out AssetReferenceT<Material> smallMarker))
                return false;
            apply(largeMarker, smallMarker);
            return true;
        }
    }

    private static bool TryLoadTemplateMaterial(AssetReferenceT<Material> reference, string label, out Material material)
    {
        material = null;
        if (reference is null) throw new InvalidOperationException("The template " + label + " jacket reference is null.");
        MethodInfo load = null;
        foreach (MethodInfo candidate in reference.GetType().GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
        {
            if (candidate.Name == "LoadAssetAsync" && candidate.GetParameters().Length == 0)
            {
                load = candidate;
                break;
            }
        }
        if (load is null) throw new MissingMethodException(reference.GetType().FullName, "LoadAssetAsync");
        object operation = load.Invoke(reference, null)
            ?? throw new InvalidOperationException("The template " + label + " jacket load returned null.");
        MethodInfo wait = operation.GetType().GetMethod("WaitForCompletion", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        object result = wait?.Invoke(operation, null);
        if (result is Material resolved && resolved.Pointer != IntPtr.Zero)
        {
            material = resolved;
            return true;
        }
        if (result is null) return false;
        throw new InvalidOperationException("The template " + label + " jacket load returned " + result.GetType().FullName + ", not Material.");
    }

    private static void Queue(DeferredJacketRecord record)
    {
        lock (Gate)
        {
            foreach (DeferredJacketRecord existing in DeferredRecords)
                if (existing.Matches(record)) return;
            DeferredRecords.Add(record);
        }
    }

    private static bool TryFinalize(DeferredJacketRecord record)
    {
        if (record.SongData is null || record.SongData.Pointer == IntPtr.Zero) return true;
        if (record.IsSelector)
        {
            Il2CppReferenceArray<SongJacketEntry> entries = record.SongData.songIdJacketMaterials;
            for (int index = 0; index < entries.Length; index++)
            {
                SongJacketEntry entry = entries[index];
                if (entry is null || entry.SongId.Value != record.SongId.Value) continue;
                if (!ApplySelectorReferences(entry, record.Config)) return false;
                entries[index] = entry;
                NativeSongDataBridge.ResetSongIdJacketCache(record.SongData);
                record.SongData._yOA();
                return true;
            }
            return true;
        }

        Il2CppReferenceArray<SongDataChartJacketEntry> chartEntries = record.SongData.chartIdJacketMaterials;
        for (int index = 0; index < chartEntries.Length; index++)
        {
            SongDataChartJacketEntry entry = chartEntries[index];
            if (entry is null || !string.Equals(entry.ChartId, record.ChartId, StringComparison.OrdinalIgnoreCase)) continue;
            if (!ApplyChartReferences(entry, record.Config)) return false;
            chartEntries[index] = entry;
            NativeSongDataBridge.ResetChartIdJacketCache(record.SongData);
            record.SongData._yOA();
            return true;
        }
        return true;
    }

    private static RuntimeJacket LoadMaterial(Material template, string pngPath)
    {
        if (template is null || template.Pointer == IntPtr.Zero)
            throw new InvalidOperationException("The template jacket material is unavailable.");
        if (string.IsNullOrWhiteSpace(pngPath) || !File.Exists(pngPath))
            throw new FileNotFoundException("Loose jacket PNG is unavailable.", pngPath);

        DecodedPng png = PngDecoder.Decode(File.ReadAllBytes(pngPath));
        Texture2D texture = new(png.Width, png.Height, TextureFormat.RGBA32, false);
        try
        {
            unsafe
            {
                fixed (byte* bytes = png.RgbaBottomUp)
                    texture.LoadRawTextureData((IntPtr)bytes, png.RgbaBottomUp.Length);
            }
            texture.Apply(false, true);
            Material material = new(template);
            material.SetTexture("_MainTex", texture);
            return new RuntimeJacket(material, texture);
        }
        catch
        {
            UnityEngine.Object.Destroy(texture);
            throw;
        }
    }

    private sealed class RuntimeJacket
    {
        internal RuntimeJacket(Material material, Texture2D texture) { Material = material; Texture = texture; }
        internal Material Material { get; }
        private Texture2D Texture { get; }
        internal void Dispose()
        {
            if (Material is not null) UnityEngine.Object.Destroy(Material);
            if (Texture is not null) UnityEngine.Object.Destroy(Texture);
        }
    }

    private sealed class RuntimeJacketPair
    {
        internal RuntimeJacketPair(RuntimeJacket large, RuntimeJacket small) { Large = large; Small = small; }
        internal RuntimeJacket Large { get; }
        internal RuntimeJacket Small { get; }
        internal void Dispose()
        {
            Large?.Dispose();
            Small?.Dispose();
        }
    }

    private sealed class DeferredJacketRecord
    {
        internal DeferredJacketRecord(SongData songData, SongId songId, string chartId, CustomSongConfig config)
        {
            SongData = songData;
            SongId = songId;
            ChartId = chartId;
            Config = config;
        }

        internal SongData SongData { get; }
        internal SongId SongId { get; }
        internal string ChartId { get; }
        internal CustomSongConfig Config { get; }
        internal bool IsSelector => ChartId is null;
        internal bool Matches(DeferredJacketRecord other) =>
            SongData.Pointer == other.SongData.Pointer &&
            IsSelector == other.IsSelector &&
            (IsSelector ? SongId.Value == other.SongId.Value : string.Equals(ChartId, other.ChartId, StringComparison.OrdinalIgnoreCase));
    }
}

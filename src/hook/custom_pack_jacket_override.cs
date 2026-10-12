using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEngine;
using PackStyle = Il2Cppifapp.Game.Data.PackSelectSceneAssets.SongSelectPackAssets;

namespace InFalsusCustomSongHook;

internal static class CustomPackJacketOverride {
    private const string kTextureProperty = "_MainTex";
    private static readonly string[] Extensions = { ".png", ".webp" };
    private static readonly Dictionary<(int PackId, string Path, PackSurface Surface), RuntimePackJacket> Jackets = new();
    private static readonly HashSet<string> ReportedErrors = new(StringComparer.Ordinal);

    private enum PackSurface { Large, Small, SongSelect }

    internal static void Apply(PackStyle style, int templatePackId) {
        if (style is null) return;
        if (TryLoad("packLarge", templatePackId, PackSurface.Large, new[] {
                style.PackSelectLargeNonCompleted, style.PackSelectLargeNonCompletedHover,
                style.PackSelectLargeCompleted, style.PackSelectLargeCompletedHover,
            }, out Material[] large)) {
            style.PackSelectLargeNonCompleted = large[0];
            style.PackSelectLargeNonCompletedHover = large[1];
            style.PackSelectLargeCompleted = large[2];
            style.PackSelectLargeCompletedHover = large[3];
        }
        if (TryLoad("packSmall", templatePackId, PackSurface.Small, new[] {
                style.PackSelectNonCompleted, style.PackSelectNonCompletedHover,
                style.PackSelectCompleted, style.PackSelectCompletedHover,
            }, out Material[] small)) {
            style.PackSelectNonCompleted = small[0];
            style.PackSelectNonCompletedHover = small[1];
            style.PackSelectCompleted = small[2];
            style.PackSelectCompletedHover = small[3];
        }
        if (TryLoad("packLarge", templatePackId, PackSurface.SongSelect, new[] {
                style.SongSelectLargeNonCompleted, style.SongSelectLargeNonCompletedHover,
                style.SongSelectLargeCompleted, style.SongSelectLargeCompletedHover,
            }, out Material[] songSelect)) {
            style.SongSelectLargeNonCompleted = songSelect[0];
            style.SongSelectLargeNonCompletedHover = songSelect[1];
            style.SongSelectLargeCompleted = songSelect[2];
            style.SongSelectLargeCompletedHover = songSelect[3];
        }
    }

    internal static void Dispose() {
        foreach (RuntimePackJacket jacket in Jackets.Values) jacket.Dispose();
        Jackets.Clear();
        ReportedErrors.Clear();
    }

    private static bool TryLoad(string fileStem, int templatePackId, PackSurface surface,
        Material[] templates, out Material[] materials) {
        materials = null;
        string path = FindPath(fileStem);
        if (path is null) return false;
        try {
            var key = (templatePackId, path, surface);
            if (!Jackets.TryGetValue(key, out RuntimePackJacket jacket) || !jacket.IsAvailable) {
                jacket?.Dispose();
                Jackets.Remove(key);
                jacket = LoadJacket(path, templates);
                Jackets.Add(key, jacket);
                CustomSongMod.Log.Msg("[CustomSong] loaded pack card jacket: " + path + "; surface=" + surface);
            }
            materials = jacket.Materials;

            return true;
        } catch (Exception exception) {
            string error = path + " (" + surface + "): " + exception.Message;
            if (ReportedErrors.Add(error))
                CustomSongMod.Log.Warning("[CustomSong] pack card jacket could not be loaded; retaining template: " + error);

            return false;
        }
    }

    private static string FindPath(string fileStem) {
        foreach (string extension in Extensions) {
            string path = Path.Combine(CustomSongConfig.CustomChartsRoot, fileStem + extension);
            if (File.Exists(path)) return path;
        }

        return null;
    }

    private static RuntimePackJacket LoadJacket(string path, Material[] templates) {
        if (templates.Any(material => material == null || !material.HasProperty(kTextureProperty)))
            throw new InvalidOperationException("The template pack card materials are unavailable or incompatible.");

        DecodedJacket image = JacketCodec.Decode(File.ReadAllBytes(path));
        Material[] materials = new Material[templates.Length];
        Texture2D[] textures = new Texture2D[templates.Length];
        try {
            for (int index = 0; index < templates.Length; index++) {
                DecodedJacket mask = ReadTemplateTexture(templates[index].GetTexture(kTextureProperty));
                DecodedJacket maskedImage = PackJacketMask.Apply(image, mask);
                Texture2D texture = new(maskedImage.Width, maskedImage.Height, TextureFormat.RGBA32, false);
                textures[index] = texture;
                texture.name = Path.GetFileName(path);
                texture.hideFlags = HideFlags.DontUnloadUnusedAsset;
                texture.wrapMode = TextureWrapMode.Clamp;
                texture.filterMode = FilterMode.Bilinear;
                unsafe {
                    fixed (byte* pixels = maskedImage.RgbaBottomUp)
                        texture.LoadRawTextureData((IntPtr)pixels, maskedImage.RgbaBottomUp.Length);
                }
                texture.Apply(false, true);
                Material material = new(templates[index]);
                materials[index] = material;
                material.hideFlags = HideFlags.DontUnloadUnusedAsset;
                material.SetTexture(kTextureProperty, texture);
            }

            return new RuntimePackJacket(materials, textures);
        } catch {
            foreach (Material material in materials)
                if (material != null) UnityEngine.Object.Destroy(material);
            foreach (Texture2D texture in textures)
                if (texture != null) UnityEngine.Object.Destroy(texture);
            throw;
        }
    }

    private static DecodedJacket ReadTemplateTexture(Texture source) {
        if (source == null || source.width <= 0 || source.height <= 0)
            throw new InvalidOperationException("The template pack card texture is unavailable.");

        RenderTexture previous = RenderTexture.active;
        RenderTexture temporary = RenderTexture.GetTemporary(source.width, source.height, 0,
            RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear);
        Texture2D readback = null;
        try {
            Graphics.Blit(source, temporary);
            RenderTexture.active = temporary;
            readback = new Texture2D(source.width, source.height, TextureFormat.RGBA32, false, true);
            readback.ReadPixels(new Rect(0, 0, source.width, source.height), 0, 0, false);
            byte[] pixels = readback.GetRawTextureData().AsSpan().ToArray();

            return new DecodedJacket(source.width, source.height, pixels);
        } finally {
            RenderTexture.active = previous;
            RenderTexture.ReleaseTemporary(temporary);
            if (readback != null) UnityEngine.Object.Destroy(readback);
        }
    }

    private sealed class RuntimePackJacket {
        internal RuntimePackJacket(Material[] materials, Texture2D[] textures) {
            Materials = materials;
            Textures = textures;
        }

        internal Material[] Materials { get; }
        private Texture2D[] Textures { get; }
        internal bool IsAvailable => Textures.All(texture => texture != null) && Materials.All(material => material != null);

        internal void Dispose() {
            foreach (Material material in Materials)
                if (material != null) UnityEngine.Object.Destroy(material);
            foreach (Texture2D texture in Textures)
                if (texture != null) UnityEngine.Object.Destroy(texture);
        }
    }
}

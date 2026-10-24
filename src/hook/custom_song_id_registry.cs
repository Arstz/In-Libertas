using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace InFalsusCustomSongHook;

internal sealed class CustomSongIdRegistry : IDisposable
{
    internal const int DefaultFirstSongId = 4096;
    private const int VanillaHeadroom = 1024;
    private const int FormatVersion = 1;
    private static readonly JsonSerializerOptions JsonOptions = new() {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
    };

    private readonly string m_path;
    private readonly FileStream m_lock;
    private readonly RegistryDocument m_document;
    private readonly Dictionary<string, ushort> m_songIds = new(StringComparer.Ordinal);
    private int m_nextSongId;
    private bool m_changed;

    private CustomSongIdRegistry(string path, int stockSongCount, FileStream registryLock) {
        m_path = path;
        m_lock = registryLock;
        if (File.Exists(path)) {
            m_document = JsonSerializer.Deserialize<RegistryDocument>(File.ReadAllBytes(path), JsonOptions)
                ?? throw new InvalidDataException("The custom SongId registry is empty: " + path);
        } else {
            if (File.Exists(path + ".bak"))
                throw new InvalidDataException("The custom SongId registry is missing but its backup exists; restore the registry before starting the game: " + path);
            int firstSongId = Math.Max(DefaultFirstSongId,
                ((stockSongCount + VanillaHeadroom + VanillaHeadroom - 1) / VanillaHeadroom) * VanillaHeadroom);
            m_document = new RegistryDocument {
                Version = FormatVersion,
                FirstSongId = firstSongId,
                Songs = new List<RegistryEntry>(),
            };
            m_changed = true;
        }

        if (m_document.Version != FormatVersion || m_document.FirstSongId < DefaultFirstSongId
            || m_document.FirstSongId > ushort.MaxValue || m_document.Songs is null)
            throw new InvalidDataException("The custom SongId registry has an unsupported format or ID range: " + path);

        m_nextSongId = m_document.FirstSongId;
        HashSet<ushort> assigned = new();
        foreach (RegistryEntry entry in m_document.Songs) {
            if (entry is null || string.IsNullOrWhiteSpace(entry.BaseName)
                || entry.SongId < m_document.FirstSongId || entry.SongId > ushort.MaxValue)
                throw new InvalidDataException("The custom SongId registry contains an invalid assignment: " + path);
            ushort songId = (ushort)entry.SongId;
            if (!assigned.Add(songId) || !m_songIds.TryAdd(entry.BaseName, songId))
                throw new InvalidDataException("The custom SongId registry contains duplicate assignments: " + path);
            if (songId < stockSongCount)
                throw new InvalidDataException($"Vanilla SongData now occupies reserved custom SongId {songId} for '{entry.BaseName}'. Custom installation stopped to preserve the registry: {path}");
            m_nextSongId = Math.Max(m_nextSongId, entry.SongId + 1);
        }
        m_nextSongId = Math.Max(m_nextSongId, stockSongCount);
    }

    internal static CustomSongIdRegistry Open(string path, int stockSongCount) {
        if (stockSongCount < 1 || stockSongCount > ushort.MaxValue + 1)
            throw new ArgumentOutOfRangeException(nameof(stockSongCount));
        string fullPath = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(fullPath));
        FileStream registryLock = new(fullPath + ".lock", FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        try {
            return new CustomSongIdRegistry(fullPath, stockSongCount, registryLock);
        } catch {
            registryLock.Dispose();
            throw;
        }
    }

    internal int SongTableLength => m_nextSongId;

    internal ushort GetOrAllocate(string baseName) {
        if (string.IsNullOrWhiteSpace(baseName)) throw new ArgumentException("A song base name is required.", nameof(baseName));
        if (m_songIds.TryGetValue(baseName, out ushort existing)) return existing;
        if (m_nextSongId > ushort.MaxValue)
            throw new InvalidOperationException("The custom SongId registry has exhausted the 16-bit SongId range.");
        ushort songId = (ushort)m_nextSongId++;
        m_songIds.Add(baseName, songId);
        m_document.Songs.Add(new RegistryEntry { BaseName = baseName, SongId = songId });
        m_changed = true;
        return songId;
    }

    internal void Commit() {
        if (!m_changed) return;
        string temporaryPath = m_path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try {
            byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(m_document, JsonOptions);
            using (FileStream file = new(temporaryPath, FileMode.CreateNew, FileAccess.Write, FileShare.None)) {
                file.Write(bytes);
                file.Flush(true);
            }
            if (File.Exists(m_path)) File.Replace(temporaryPath, m_path, m_path + ".bak");
            else File.Move(temporaryPath, m_path);
            m_changed = false;
        } finally {
            if (File.Exists(temporaryPath)) File.Delete(temporaryPath);
        }
    }

    public void Dispose() => m_lock.Dispose();

    private sealed class RegistryDocument
    {
        public int Version { get; set; }
        public int FirstSongId { get; set; }
        public List<RegistryEntry> Songs { get; set; }
    }

    private sealed class RegistryEntry
    {
        public string BaseName { get; set; }
        public int SongId { get; set; }
    }
}

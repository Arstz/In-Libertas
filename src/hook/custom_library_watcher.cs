using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;

namespace InFalsusCustomSongHook;

internal sealed class CustomLibraryWatcher : IDisposable {
    private readonly string m_root;
    private string m_accepted;
    private string m_candidate;
    private FileSystemWatcher m_watcher;
    private int m_revision;
    private int m_candidateRevision;

    internal CustomLibraryWatcher(string root, Action<string> warning) {
        m_root = root;
        try {
            m_watcher = new FileSystemWatcher(root) {
                IncludeSubdirectories = true,
                NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite | NotifyFilters.Size,
            };
            m_watcher.Changed += Changed;
            m_watcher.Created += Changed;
            m_watcher.Deleted += Changed;
            m_watcher.Renamed += Changed;
            m_watcher.Error += WatcherError;
            m_watcher.EnableRaisingEvents = true;
            AcceptChanges(Capture());
        }
        catch (Exception exception) { warning("library change tracking could not start: " + exception.Message); }
    }

    internal bool HasChanges() {
        m_candidate = Capture();
        return !string.Equals(m_candidate, m_accepted, StringComparison.Ordinal);
    }

    internal void Checkpoint() {
        m_candidateRevision = Volatile.Read(ref m_revision);
        m_candidate = Capture();
    }
    internal bool IsCurrentCheckpoint() => string.Equals(m_candidate, Capture(), StringComparison.Ordinal)
        && m_candidateRevision == Volatile.Read(ref m_revision);
    internal void AcceptChanges() => AcceptChanges(m_candidate);
    private void AcceptChanges(string fingerprint) => m_accepted = fingerprint;
    public void Dispose() => m_watcher?.Dispose();

    private void Changed(object sender, FileSystemEventArgs args) {
        string relative = Path.GetRelativePath(m_root, args.FullPath);
        foreach (string part in relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar))
            if (part.StartsWith(".inlibertas-", StringComparison.OrdinalIgnoreCase)) return;
        if (Relevant(args.FullPath)) Interlocked.Increment(ref m_revision);
    }
    private void WatcherError(object sender, ErrorEventArgs args) => Interlocked.Increment(ref m_revision);

    private string Capture() {
        if (!Directory.Exists(m_root)) return "missing";
        StringBuilder fingerprint = new();
        foreach (string path in ContentFiles(m_root).OrderBy(path => path, StringComparer.OrdinalIgnoreCase)) {
            FileInfo file = new(path);
            fingerprint.Append(Path.GetRelativePath(m_root, path)).Append('|').Append(file.Length)
                .Append('|').Append(file.LastWriteTimeUtc.Ticks).Append('\n');
        }
        return fingerprint.ToString();
    }

    private static IEnumerable<string> ContentFiles(string directory) {
        ProjectExport.EnsureOrdinary(directory);
        foreach (string file in Directory.EnumerateFiles(directory)) {
            if (Relevant(file)) {
                ProjectExport.EnsureOrdinary(file);
                yield return file;
            }
        }
        foreach (string child in Directory.EnumerateDirectories(directory)) {
            if (Path.GetFileName(child).StartsWith(".inlibertas-", StringComparison.OrdinalIgnoreCase)) continue;
            foreach (string file in ContentFiles(child)) yield return file;
        }
    }

    private static bool Relevant(string path) =>
        Path.GetFileName(path).Equals("config.json", StringComparison.OrdinalIgnoreCase)
        || Path.GetExtension(path).ToLowerInvariant() is ".10no" or ".spc" or ".sam" or ".ogg" or ".wav" or ".mp3" or ".png" or ".webp";
}

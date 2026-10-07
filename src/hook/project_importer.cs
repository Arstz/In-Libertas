using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;

namespace InFalsusCustomSongHook;

internal sealed class ImportJournal {
    public int Version { get; set; } = 1;
    public string ChartId { get; set; }
    public string SourceFile { get; set; }
    public string SourceSha256 { get; set; }
}

internal sealed class ProjectImporter {
    private const string kTransactionsDirectory = ".inlibertas-imports";
    private const string kBackupsDirectory = ".inlibertas-backups";
    private readonly string m_root;
    private readonly Action<string> m_log;
    private readonly Func<string, bool> m_recycle;
    private readonly Action<string> m_checkpoint;
    private readonly HashSet<string> m_recoveryBlocked = new(StringComparer.OrdinalIgnoreCase);
    private readonly Queue<(string SourcePath, string SourceSha256)> m_pendingRecycling = new();
    private readonly bool m_deferRecycling;

    internal ProjectImporter(string root, Action<string> log, Func<string, bool> recycle, Action<string> checkpoint = null, bool deferRecycling = false) {
        m_root = Path.GetFullPath(root);
        m_log = log;
        m_recycle = recycle;
        m_checkpoint = checkpoint ?? (_ => { });
        m_deferRecycling = deferRecycling;
    }

    internal void Prepare() {
        m_recoveryBlocked.Clear();
        m_pendingRecycling.Clear();
        Directory.CreateDirectory(m_root);
        ProjectExport.EnsureOrdinary(m_root);
        Recover();
        List<(string Path, string ChartId)> candidates = new();
        foreach (string path in Directory.EnumerateFiles(m_root).Where(path => Path.GetExtension(path).Equals(".10no", StringComparison.OrdinalIgnoreCase))
            .OrderBy(path => path, StringComparer.OrdinalIgnoreCase)) {
            try { candidates.Add((path, ReadIdentity(path))); }
            catch (Exception exception) { m_log("project rejected: " + Path.GetFileName(path) + ": " + exception.Message); }
        }
        foreach (var group in candidates.GroupBy(candidate => candidate.ChartId, StringComparer.OrdinalIgnoreCase)) {
            if (m_recoveryBlocked.Contains(group.Key)) {
                m_log("project retained pending recovery: " + group.Key);
                continue;
            }
            if (group.Count() != 1) {
                m_log("duplicate dropped Chart ID '" + group.Key + "'; all conflicting projects retained.");
                continue;
            }
            var candidate = group.First();
            try { Import(candidate.Path, candidate.ChartId); }
            catch (Exception exception) { m_log("project import failed: " + Path.GetFileName(candidate.Path) + ": " + exception.Message); }
        }
    }

    private string ReadIdentity(string path) {
        ProjectExport.EnsureOrdinary(path);
        using FileStream file = new(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        using BinaryReader reader = new(file);
        if (file.Length < 26 || Encoding.ASCII.GetString(reader.ReadBytes(4)) != "10NO") throw new InvalidDataException("Not a .10no project.");
        int version = reader.ReadUInt16();
        if (version == 2) throw new InvalidDataException("Open this legacy project in the updated editor to migrate it.");
        if (version != ProjectReader.kVersion) throw new InvalidDataException("Unsupported project version.");
        uint length = reader.ReadUInt32();
        reader.ReadUInt64(); reader.ReadUInt64();
        if (length > ProjectReader.kMaximumManifestBytes || length > file.Length - file.Position) throw new InvalidDataException("Invalid manifest length.");
        using JsonDocument document = JsonDocument.Parse(ProjectReader.ReadExactly(reader, (int)length));
        string chartId = ProjectReader.Text(document.RootElement, "chart_id");
        if (!ProjectReader.ValidChartId(chartId)) throw new InvalidDataException("Invalid Chart ID.");

        return chartId;
    }

    private void Import(string sourcePath, string expectedChartId) {
        string target = Child(m_root, expectedChartId);
        string token = Guid.NewGuid().ToString("N");
        string transaction = Child(OwnedDirectory(kTransactionsDirectory), token);
        string stage = Child(transaction, "stage");
        string backup = Child(OwnedDirectory(kBackupsDirectory), expectedChartId + "-" + token);
        string sourceHash;
        bool published = false;
        using (FileStream source = new(sourcePath, FileMode.Open, FileAccess.Read, FileShare.Read)) {
            sourceHash = ProjectExport.HashStream(source);
            source.Position = 0;
            ProjectPackage package = ProjectReader.Read(source);
            if (package.ChartId != expectedChartId) throw new InvalidDataException("Project identity changed during discovery.");
            if (Directory.Exists(target)) {
                ExportOwnership existing = ProjectExport.Verify(target, package.ChartId);
                if (existing.ConverterRevision == ProjectExport.kConverterRevision && existing.SourceSha256 == sourceHash) {
                    m_log("unchanged project export verified: " + package.ChartId);
                    published = true;
                }
            }
            if (!published) {
                Directory.CreateDirectory(transaction);
                WriteJournal(transaction, new ImportJournal { ChartId = package.ChartId, SourceFile = Path.GetFileName(sourcePath), SourceSha256 = sourceHash });
                ProjectExport.Write(stage, package, sourceHash);
                m_checkpoint("prepared");
                try {
                    if (Directory.Exists(target)) {
                        ProjectExport.Verify(target, package.ChartId);
                        Directory.Move(target, backup);
                    }
                    m_checkpoint("backed-up");
                    Directory.Move(stage, target);
                    m_checkpoint("published");
                    ProjectExport.Verify(target, package.ChartId);
                    published = true;
                    m_log("project export committed: " + package.ChartId + (Directory.Exists(backup) ? "; previous export: " + backup : ""));
                }
                catch {
                    if (!Directory.Exists(target) && Directory.Exists(backup)) {
                        Directory.Move(backup, target);
                        m_log("previous export restored: " + package.ChartId);
                    }
                    throw;
                }
            }
        }
        if (published) {
            m_checkpoint("source-released");
            RecycleSource(sourcePath, sourceHash);
            if (File.Exists(Child(transaction, "journal.json"))) File.Delete(Child(transaction, "journal.json"));
        }
    }

    private void Recover() {
        string imports = OwnedDirectory(kTransactionsDirectory);
        foreach (string transaction in Directory.EnumerateDirectories(imports)) {
            string recoveringChartId = null;
            try {
                ProjectExport.EnsureOrdinary(transaction);
                string token = Path.GetFileName(transaction);
                if (!Guid.TryParseExact(token, "N", out _)) continue;
                string journalPath = Child(transaction, "journal.json");
                if (!File.Exists(journalPath)) continue;
                ProjectExport.EnsureOrdinary(journalPath);
                if (new FileInfo(journalPath).Length > 4096) throw new InvalidDataException("Oversized import journal.");
                ImportJournal journal = JsonSerializer.Deserialize<ImportJournal>(File.ReadAllBytes(journalPath), ProjectExport.JsonOptions);
                if (journal is null || journal.Version != 1 || !ProjectReader.ValidChartId(journal.ChartId)
                    || journal.SourceSha256 is not { Length: 64 } || Path.GetFileName(journal.SourceFile) != journal.SourceFile
                    || !Path.GetExtension(journal.SourceFile).Equals(".10no", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Invalid recovery journal.");
                recoveringChartId = journal.ChartId;
                string target = Child(m_root, journal.ChartId);
                string backup = Child(OwnedDirectory(kBackupsDirectory), journal.ChartId + "-" + token);
                if (Directory.Exists(target)) {
                    ExportOwnership existing = ProjectExport.Verify(target, journal.ChartId);
                    if (existing.SourceSha256 == journal.SourceSha256) {
                        m_log("recovered committed project export: " + journal.ChartId);
                    } else if (!Directory.Exists(backup)) {
                        m_log("recovered unpublished project export: " + journal.ChartId);
                    } else {
                        throw new IOException("Recovery target differs from the transaction; files retained.");
                    }
                }
                else if (Directory.Exists(backup)) {
                    ProjectExport.Verify(backup, journal.ChartId);
                    Directory.Move(backup, target);
                    m_log("recovered previous project export: " + journal.ChartId);
                }
                File.Delete(journalPath);
            }
            catch (Exception exception) {
                if (recoveringChartId != null) m_recoveryBlocked.Add(recoveringChartId);
                m_log("import recovery warning: " + exception.Message);
            }
        }
    }

    internal void RecyclePending() {
        while (m_pendingRecycling.TryDequeue(out var source)) {
            RecycleSource(source.SourcePath, source.SourceSha256, allowDeferral: false);
        }
    }

    private void RecycleSource(string sourcePath, string expectedHash, bool allowDeferral = true) {
        try {
            ProjectExport.EnsureOrdinary(sourcePath);
            if (ProjectExport.HashFile(sourcePath) != expectedHash) throw new IOException("Source changed; project retained.");
            if (allowDeferral && m_deferRecycling) {
                m_pendingRecycling.Enqueue((sourcePath, expectedHash));
                m_log("source recycling deferred until library installation: " + Path.GetFileName(sourcePath));

                return;
            }
            if (!m_recycle(sourcePath)) throw new IOException("Recycle Bin operation failed; project retained.");
            m_log("project moved to Recycle Bin: " + Path.GetFileName(sourcePath));
        }
        catch (Exception exception) { m_log("source disposal warning: " + exception.Message); }
    }

    private string OwnedDirectory(string name) {
        string directory = Child(m_root, name);
        Directory.CreateDirectory(directory);
        ProjectExport.EnsureOrdinary(directory);

        return directory;
    }

    private static string Child(string directory, string name) {
        string parent = Path.GetFullPath(directory).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        string path = Path.GetFullPath(Path.Combine(parent, name));
        if (Path.GetFileName(name) != name || !path.StartsWith(parent, StringComparison.OrdinalIgnoreCase))
            throw new IOException("Unsafe import path.");

        return path;
    }

    private static void WriteJournal(string directory, ImportJournal journal) {
        string path = Child(directory, "journal.json");
        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(journal, ProjectExport.JsonOptions);
        using FileStream file = new(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        file.Write(bytes);
        file.Flush(true);
    }
}

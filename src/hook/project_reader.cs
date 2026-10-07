using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace InFalsusCustomSongHook;

internal sealed class ProjectPackage {
    internal string ChartId { get; init; }
    internal JsonElement Metadata { get; init; }
    internal JsonElement[] Difficulties { get; init; }
    internal byte[] Audio { get; init; }
    internal byte[] Jacket { get; init; }
}

internal static class ProjectReader {
    internal const int kVersion = 3;
    internal const int kMaximumManifestBytes = 32 * 1024 * 1024;
    internal const long kMaximumJacketPixels = 67108864;
    internal static readonly string[] DifficultyNames = { "minimal", "evolved", "ultimate", "forbidden" };

    internal static bool ValidChartId(string value) => value is { Length: > 0 and <= 200 }
        && Regex.IsMatch(value, "\\A[A-Za-z0-9_-]+\\z")
        && !Regex.IsMatch(value, "\\A(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])\\z", RegexOptions.IgnoreCase);

    internal static ProjectPackage Read(Stream source) {
        using BinaryReader reader = new(source, Encoding.UTF8, true);
        if (source.Length < 26 || Encoding.ASCII.GetString(reader.ReadBytes(4)) != "10NO")
            throw new InvalidDataException("Not a .10no project.");
        ushort version = reader.ReadUInt16();
        if (version == 2)
            throw new InvalidDataException("Legacy .10no: open it in the updated In Libertas editor to migrate it first.");
        if (version != kVersion) throw new InvalidDataException("Unsupported .10no version: " + version);
        uint manifestLength = reader.ReadUInt32();
        ulong audioLength = reader.ReadUInt64();
        ulong jacketLength = reader.ReadUInt64();
        ulong remaining = (ulong)(source.Length - source.Position);
        if (manifestLength > kMaximumManifestBytes || manifestLength > remaining
            || audioLength == 0 || audioLength > int.MaxValue || audioLength > remaining - manifestLength
            || jacketLength == 0 || jacketLength > int.MaxValue || jacketLength != remaining - manifestLength - audioLength)
            throw new InvalidDataException("Invalid project payload lengths.");
        using JsonDocument document = JsonDocument.Parse(ReadExactly(reader, (int)manifestLength));
        JsonElement manifest = document.RootElement;
        if (Text(manifest, "format") != "in_libertas_project" || manifest.GetProperty("version").GetInt32() != version)
            throw new InvalidDataException("Invalid project manifest format or version.");
        string chartId = Text(manifest, "chart_id");
        if (!ValidChartId(chartId)) throw new InvalidDataException("A valid user-entered Chart ID is required.");
        JsonElement assets = manifest.GetProperty("assets");
        if (Text(assets, "song_file_name") != "audio.ogg" || Text(assets, "jacket_file_name") != "jacket.png")
            throw new InvalidDataException("Version-3 projects require audio.ogg and jacket.png assets.");
        byte[] audio = ReadExactly(reader, (int)audioLength);
        byte[] jacket = ReadExactly(reader, (int)jacketLength);
        ProjectMedia.Validate(audio, jacket);
        JsonElement[] difficulties = DifficultyNames.Select(name => manifest.GetProperty("difficulties").GetProperty(name).Clone()).ToArray();
        JsonElement metadata = manifest.GetProperty("metadata").Clone();
        double previewStart = Number(metadata, "preview_start_seconds");
        double previewEnd = Number(metadata, "preview_end_seconds");
        if (previewStart < 0 || previewStart >= previewEnd)
            throw new InvalidDataException("Preview start must be nonnegative and earlier than preview end.");
        if (!difficulties.Any(chart => chart.GetProperty("notes").GetArrayLength() > 0))
            throw new InvalidDataException("Project has no populated difficulties.");

        return new ProjectPackage { ChartId = chartId, Metadata = metadata, Difficulties = difficulties, Audio = audio, Jacket = jacket };
    }

    internal static byte[] ReadExactly(BinaryReader reader, int count) {
        byte[] bytes = reader.ReadBytes(count);
        if (bytes.Length != count) throw new InvalidDataException("Truncated project.");

        return bytes;
    }

    internal static string Text(JsonElement element, string name) => element.GetProperty(name).GetString()
        ?? throw new InvalidDataException("Missing string: " + name);

    internal static double Number(JsonElement element, string name) {
        double value = element.GetProperty(name).GetDouble();
        if (!double.IsFinite(value)) throw new InvalidDataException("Non-finite value: " + name);

        return value;
    }

    internal static uint Time(JsonElement element, string name) {
        long value = element.GetProperty(name).GetInt64();
        if (value < 0 || value > uint.MaxValue) throw new InvalidDataException("Time is outside SPC range: " + name);

        return (uint)value;
    }
}

internal static class ProjectMedia {
    private const uint kOggPolynomial = 79764919;
    private const uint kPngPolynomial = 3988292384;

    internal static void Validate(byte[] audio, byte[] jacket) {
        ValidateVorbis(audio);
        ValidatePng(jacket);
        PngDecoder.Decode(jacket);
    }

    internal static void ValidateVorbis(byte[] audio) {
        using MemoryStream packet = new();
        int cursor = 0, packetIndex = 0;
        uint serial = 0, sequence = 0;
        bool ended = false;
        while (cursor < audio.Length) {
            if (ended || audio.Length - cursor < 27 || Encoding.ASCII.GetString(audio, cursor, 4) != "OggS" || audio[cursor + 4] != 0)
                throw new InvalidDataException("Invalid or incomplete Ogg Vorbis stream.");
            int flags = audio[cursor + 5], segments = audio[cursor + 26];
            int pageLength = 27 + segments;
            if (audio.Length - cursor < pageLength || (flags & ~7) != 0 || ((flags & 1) != 0) != (packet.Length > 0))
                throw new InvalidDataException("Invalid Ogg page continuation.");
            uint pageSerial = BitConverter.ToUInt32(audio, cursor + 14), pageSequence = BitConverter.ToUInt32(audio, cursor + 18);
            if (cursor == 0) {
                serial = pageSerial;
                if ((flags & 2) == 0 || pageSequence != 0) throw new InvalidDataException("Missing Ogg beginning.");
            }
            else if ((flags & 2) != 0) throw new InvalidDataException("Chained Ogg streams require editor normalization.");
            if (pageSerial != serial || pageSequence != sequence++) throw new InvalidDataException("Invalid Ogg page sequence.");
            for (int index = 0; index < segments; index++) pageLength += audio[cursor + 27 + index];
            if (audio.Length - cursor < pageLength) throw new InvalidDataException("Truncated Ogg page.");
            uint checksum = 0;
            for (int index = 0; index < pageLength; index++) {
                checksum ^= (uint)(index >= 22 && index < 26 ? 0 : audio[cursor + index]) << 24;
                for (int bit = 0; bit < 8; bit++) checksum = (checksum << 1) ^ ((checksum & 2147483648U) != 0 ? kOggPolynomial : 0);
            }
            if (checksum != BitConverter.ToUInt32(audio, cursor + 22)) throw new InvalidDataException("Ogg checksum mismatch.");
            int payload = cursor + 27 + segments;
            for (int index = 0; index < segments; index++) {
                int length = audio[cursor + 27 + index];
                if (packetIndex < 3) packet.Write(audio, payload, length);
                else if (length == 255 && packet.Length == 0) packet.WriteByte(0);
                payload += length;
                if (length == 255) continue;
                if (packetIndex < 3) {
                    byte[] bytes = packet.ToArray();
                    int type = packetIndex == 0 ? 1 : packetIndex == 1 ? 3 : 5;
                    if (bytes.Length < 7 || bytes[0] != type || Encoding.ASCII.GetString(bytes, 1, 6) != "vorbis")
                        throw new InvalidDataException("Project audio must use Vorbis, not Opus or another codec.");
                    if (packetIndex == 0 && (bytes.Length != 30 || BitConverter.ToUInt32(bytes, 7) != 0 || bytes[11] == 0
                        || BitConverter.ToUInt32(bytes, 12) == 0 || (bytes[29] & 1) == 0))
                        throw new InvalidDataException("Invalid Vorbis identification header.");
                }
                packetIndex++;
                packet.SetLength(0);
                packet.Position = 0;
            }
            ended = (flags & 4) != 0;
            cursor += pageLength;
        }
        if (!ended || packet.Length != 0 || packetIndex <= 3) throw new InvalidDataException("Incomplete Vorbis audio.");
    }

    internal static void ValidatePng(byte[] bytes) {
        byte[] signature = { 137, 80, 78, 71, 13, 10, 26, 10 };
        if (bytes.Length < 33 || !bytes.AsSpan(0, 8).SequenceEqual(signature) || Encoding.ASCII.GetString(bytes, 12, 4) != "IHDR"
            || bytes[24] != 8 || bytes[25] != 6 || bytes[26] != 0 || bytes[27] != 0 || bytes[28] != 0)
            throw new InvalidDataException("Jacket must be a non-interlaced 8-bit RGBA PNG.");
        bool ended = false;
        for (int cursor = 8; cursor < bytes.Length;) {
            if (ended || bytes.Length - cursor < 12) throw new InvalidDataException("Truncated PNG chunk.");
            uint length = BigEndian(bytes, cursor);
            if (length > int.MaxValue || length > bytes.Length - cursor - 12) throw new InvalidDataException("Invalid PNG chunk length.");
            uint checksum = uint.MaxValue;
            for (int index = cursor + 4; index < cursor + 8 + (int)length; index++) {
                checksum ^= bytes[index];
                for (int bit = 0; bit < 8; bit++) checksum = (checksum >> 1) ^ ((checksum & 1) != 0 ? kPngPolynomial : 0);
            }
            if ((checksum ^ uint.MaxValue) != BigEndian(bytes, cursor + 8 + (int)length))
                throw new InvalidDataException("PNG checksum mismatch.");
            ended = Encoding.ASCII.GetString(bytes, cursor + 4, 4) == "IEND";
            if (ended && length != 0) throw new InvalidDataException("Invalid PNG ending.");
            cursor += 12 + (int)length;
        }
        if (!ended) throw new InvalidDataException("Missing PNG ending.");
    }

    private static uint BigEndian(byte[] bytes, int offset) => (uint)bytes[offset] << 24 | (uint)bytes[offset + 1] << 16
        | (uint)bytes[offset + 2] << 8 | bytes[offset + 3];
}

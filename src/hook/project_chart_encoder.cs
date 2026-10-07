using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Text.Json;

namespace InFalsusCustomSongHook;

internal static class ProjectChartEncoder {
    private const ulong kSeedConstantA = 4265267296055464877UL;
    private const ulong kSeedConstantB = 11400714785074694791UL;
    private const ulong kSeedConstantC = 3335678366873096957UL;
    private const ulong kSeedConstantD = 10723151780598845931UL;
    private const uint kIcp1Magic = 827343689;
    private const int kSkyCoordinateDenominator = 1000000;

    private sealed class EncoderState {
        internal ulong First, Second, Third, Fourth;
        internal uint Counter;

        internal void Advance(ulong first, ulong second, ulong third) {
            unchecked {
                switch (Counter & 3) {
                    case 0:
                        First += first + Rotate(Fourth, 7);
                        Second ^= Rotate(second + First, 13);
                        Third -= Second ^ third;
                        Fourth = First ^ Rotate(Third + Fourth, 27);
                        break;
                    case 1:
                        Second += third + Rotate(First, 9);
                        Third ^= Rotate(first + Second, 19);
                        Fourth -= Third ^ second;
                        First = Second ^ Rotate(First + Fourth, 31);
                        break;
                    case 2:
                        Third += second + Rotate(Second, 15);
                        Fourth ^= Rotate(third + Third, 23);
                        First -= Fourth ^ first;
                        Second = Third ^ Rotate(First + Second, 39);
                        break;
                    default:
                        ulong oldThird = Third;
                        Fourth += first + Rotate(Third, 21);
                        First ^= Rotate(second + Fourth, 29);
                        Second -= third ^ First;
                        Third = Fourth ^ Rotate(Second + oldThird, 43);
                        break;
                }
                Counter++;
            }
        }

        internal ulong Mask(uint index, uint field) {
            ulong packed = (ulong)index << 32 | field;
            unchecked {
                return (field & 3) switch {
                    0 => Fourth ^ (First + Rotate(Third ^ packed, 11)),
                    1 => First ^ (Second - Rotate(packed + Fourth, 17)),
                    2 => Second ^ (Third + Rotate(First ^ packed, 29)),
                    _ => Fourth ^ Rotate(packed + Second + Third, 37),
                };
            }
        }

        internal void Finalize(uint index, uint[] divisors) {
            unchecked {
                ulong repeatedIndex = index | (ulong)index << 32;
                ulong folded = repeatedIndex + ((First + Rotate(Second, 9)) ^ (Fourth + Rotate(Third, 23)));
                uint fold = (uint)(folded ^ folded >> 32);
                ulong fourth = ((ulong)divisors[3] - 1) ^ fold >> 24;
                ulong third = Rotate(((ulong)divisors[2] - 1) ^ ((fold >> 16) & 255), 29) ^ Rotate(fourth, 47);
                ulong combined = ((ulong)divisors[0] - 1) ^ (fold & 255)
                    ^ Rotate(((ulong)divisors[1] - 1) ^ ((fold >> 8) & 255), 13) ^ third;
                uint mix = (uint)combined ^ (uint)(combined >> 32);
                ulong repeated = mix | (ulong)mix << 32;
                First ^= repeated;
                Second += Rotate(repeated, 17);
                Third -= Rotate(repeated, 31);
                Fourth ^= Rotate(repeated, 47);
            }
        }
    }

    private readonly record struct Event(uint Time, uint Kind, ulong First, ulong Second);
    private readonly record struct Ratio(uint Scale, uint Numerator, uint Denominator);
    private readonly record struct Note(ulong GroupId, uint Auxiliary, uint Start, uint End,
        double StartX, double EndX, double StartWidth, double EndWidth, int Kind, int Side);

    internal static byte[] Encode(JsonElement chart, string fileName) {
        Note[] notes = chart.GetProperty("notes").EnumerateArray().Select(ReadNote).OrderBy(note => note.Start).ThenBy(note => note.End).ToArray();
        JsonElement[] timing = chart.GetProperty("timing_events").EnumerateArray().OrderBy(point => ProjectReader.Time(point, "time_ms")).ToArray();
        List<Event> events = new();
        HashSet<ulong> usedGroups = notes.Where(note => note.Kind == 3).Select(note => note.GroupId).ToHashSet();
        ulong nextGroup = 0;
        double initialBpm = timing.Length == 0 ? 120 : ValidBpm(timing[0]);
        int initialMeter = timing.Length == 0 ? 4 : ValidMeter(timing[0]);
        foreach (JsonElement speed in chart.GetProperty("speed_events").EnumerateArray())
            events.Add(new Event(ProjectReader.Time(speed, "time_ms"), 0, (ulong)BitConverter.DoubleToInt64Bits(ProjectReader.Number(speed, "speed")), 0));
        foreach (JsonElement lane in chart.GetProperty("lane_events").EnumerateArray()) {
            int index = lane.GetProperty("lane").GetInt32();
            if (index < 0 || index > 5) throw new InvalidDataException("Invalid lane event.");
            events.Add(new Event(ProjectReader.Time(lane, "time_ms"), 3, (uint)index | (lane.GetProperty("enabled").GetBoolean() ? 256UL : 0), 0));
        }
        if (timing.Length == 0 || ProjectReader.Time(timing[0], "time_ms") > 0)
            events.Add(new Event(0, 1, (ulong)BitConverter.DoubleToInt64Bits(initialBpm), (uint)BitConverter.SingleToInt32Bits(initialMeter)));
        foreach (JsonElement point in timing)
            events.Add(new Event(ProjectReader.Time(point, "time_ms"), 1, (ulong)BitConverter.DoubleToInt64Bits(ValidBpm(point)),
                (uint)BitConverter.SingleToInt32Bits(ValidMeter(point))));
        using MemoryStream stream = new();
        using BinaryWriter writer = new(stream);
        writer.Write(kIcp1Magic);
        writer.Write((ushort)1); writer.Write((ushort)28); writer.Write((ushort)80); writer.Write((ushort)32);
        writer.Write((uint)notes.Length); writer.Write((uint)events.Count);
        writer.Write((float)initialBpm); writer.Write((float)initialMeter);
        EncoderState state = Seed(fileName, (uint)notes.Length);
        unchecked {
            for (uint index = 0; index < notes.Length; index++) {
                Note note = notes[index];
                bool outer = note.Side == 1 || note.Side == 2;
                long denominator = note.Side == 3 ? kSkyCoordinateDenominator : 4;
                Ratio[] ratios = { MakeRatio(note.Side == 2 ? 1.25 : note.StartX, denominator),
                    MakeRatio(note.Side == 2 ? 1.25 : note.EndX, denominator), MakeRatio(outer ? 0.25 : note.StartWidth, denominator),
                    MakeRatio(outer ? 0.25 : note.EndWidth, denominator) };
                ulong groupId = note.GroupId;
                if (note.Kind != 3) {
                    while (usedGroups.Contains(nextGroup)) nextGroup++;
                    groupId = nextGroup;
                    usedGroups.Add(nextGroup++);
                }
                uint type = note.Kind switch { 0 => 1, 1 => 2, 2 => 4, 3 => 5, _ => 0U };
                ulong[] fields = { index, groupId, note.Start, note.End, (uint)(note.Side + 1) | type << 8, note.Auxiliary,
                    ratios[0].Scale, ratios[1].Scale, ratios[2].Scale, ratios[3].Scale };
                writer.Write(fields[0] + state.Mask(index, 0)); writer.Write(fields[1] + state.Mask(index, 1));
                for (uint field = 2; field < fields.Length; field++) writer.Write((uint)fields[field] + (uint)state.Mask(index, field));
                foreach (Ratio ratio in ratios) { writer.Write(ratio.Numerator); writer.Write(ratio.Denominator); }
                state.Advance(fields[0], fields[1], Pack(note.Start, note.End));
                state.Advance(fields[4], fields[5], Pack(ratios[0].Numerator, ratios[0].Denominator));
                state.Advance(Pack(ratios[1].Numerator, ratios[1].Denominator), Pack(ratios[2].Numerator, ratios[2].Denominator),
                    Pack(ratios[3].Numerator, ratios[3].Denominator));
                state.Finalize(index, ratios.Select(ratio => (uint)Gcd((int)ratio.Numerator, (int)ratio.Denominator)).ToArray());
            }
        }
        uint eventIndex = 0;
        foreach (Event entry in events.OrderBy(entry => entry.Time)) {
            writer.Write((ulong)eventIndex++); writer.Write(entry.Time); writer.Write(entry.Kind); writer.Write(entry.First); writer.Write(entry.Second);
        }

        return stream.ToArray();
    }

    private static Note ReadNote(JsonElement element) {
        int kind = element.GetProperty("kind").GetInt32(), side = element.GetProperty("side").GetInt32();
        uint start = ProjectReader.Time(element, "start_ms"), end = ProjectReader.Time(element, "end_ms");
        double startX = ProjectReader.Number(element, "start_x"), endX = ProjectReader.Number(element, "end_x");
        double startWidth = ProjectReader.Number(element, "start_width"), endWidth = ProjectReader.Number(element, "end_width");
        if (kind < 0 || kind > 3 || side < 0 || side > 3 || (kind >= 2) != (side == 3)
            || end < start || ((kind == 1 || kind == 3) && end == start)
            || startWidth <= 0 || endWidth <= 0 || Math.Abs(startX) > 2147 || Math.Abs(endX) > 2147
            || startWidth > 1.25 || endWidth > 1.25)
            throw new InvalidDataException("Invalid project note geometry or type.");
        if (side == 0 && (!ValidFloorSpan(startX, startWidth) || !ValidFloorSpan(endX, endWidth)))
            throw new InvalidDataException("Invalid central lane span.");

        return new Note(ulong.Parse(ProjectReader.Text(element, "group_id")), element.GetProperty("auxiliary").GetUInt32(),
            start, end, startX, endX, startWidth, endWidth, kind, side);
    }

    private static bool ValidFloorSpan(double coordinate, double width) => coordinate >= 0.2499 && coordinate <= 1.0001
        && width >= 0.2499 && coordinate + width <= 1.2501;

    private static double ValidBpm(JsonElement point) {
        double bpm = ProjectReader.Number(point, "bpm");
        if (bpm <= 0 || bpm > float.MaxValue) throw new InvalidDataException("Invalid BPM.");

        return bpm;
    }

    private static int ValidMeter(JsonElement point) {
        int numerator = point.GetProperty("numerator").GetInt32(), denominator = point.GetProperty("denominator").GetInt32();
        if (numerator < 1 || numerator > 32 || denominator < 1 || denominator > 32) throw new InvalidDataException("Invalid time signature.");

        return numerator;
    }

    private static EncoderState Seed(string name, uint count) {
        unchecked {
            EncoderState state = new() { First = count - kSeedConstantA, Second = (ulong)count << 32 ^ kSeedConstantB,
                Third = 1UL - kSeedConstantC, Fourth = 1UL ^ kSeedConstantD };
            for (int index = 0; index < name.Length; index++) {
                ulong character = (ulong)index << 32 | name[index];
                state.Advance(character, character + count, character ^ 1);
            }
            state.Advance(count, 1, (ulong)name.Length);

            return state;
        }
    }

    private static Ratio MakeRatio(double value, long denominator) {
        long numerator = (long)Math.Clamp(Math.Round(value * denominator, MidpointRounding.AwayFromZero), int.MinValue, int.MaxValue);

        return new Ratio((uint)Gcd(numerator, denominator), unchecked((uint)numerator), (uint)denominator);
    }

    private static long Gcd(long first, long second) {
        first = Math.Abs(first); second = Math.Abs(second);
        if (first == 0 && second == 0) return 1;
        while (second != 0) (first, second) = (second, first % second);

        return first;
    }

    private static ulong Rotate(ulong value, int shift) => BitOperations.RotateLeft(value, shift);
    private static ulong Pack(uint low, uint high) => low | (ulong)high << 32;
}

using System.Text.Json;
using System.Text.Json.Serialization;

namespace InFalsusCustomSongHook;

internal sealed class LocalizedSongTitles
{
    [JsonPropertyName("ja")]
    public string Japanese { get; set; } = string.Empty;
    [JsonPropertyName("ko")]
    public string Korean { get; set; } = string.Empty;
    [JsonPropertyName("zh-Hans")]
    public string SimplifiedChinese { get; set; } = string.Empty;
    [JsonPropertyName("zh-Hant")]
    public string TraditionalChinese { get; set; } = string.Empty;

    internal static LocalizedSongTitles FromMetadata(JsonElement metadata) =>
        metadata.TryGetProperty("song_name_localizations", out JsonElement titles)
            ? JsonSerializer.Deserialize<LocalizedSongTitles>(titles.GetRawText()) ?? new()
            : new();
}

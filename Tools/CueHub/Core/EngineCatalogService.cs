using System.Text.Json;

namespace Cue.Hub;

/// <summary>公開済みの Engine だけを列挙する。HttpClient は呼出側が Service より長く所有する</summary>
public sealed class EngineCatalogService(HttpClient a_http, HubStateStore a_store)
{
    public const string RepositoryUrl = "https://github.com/tochouseito/CueEngine";
    private readonly GitHubReleaseClient m_github = new(a_http);
    // 旧 Cache は browser_download_url しか持たないため保全し、API 形式はオンラインで再取得する
    private readonly string m_cache = Path.Combine(a_store.DirectoryPath, "engine-catalog-v2.json");
    public string? LastWarning { get; private set; }

    public async Task<IReadOnlyList<PublishedRelease>> refresh_async(CancellationToken a_cancel)
    {
        LastWarning = null;
        try
        {
            var releases = new List<PublishedRelease>();
            var completed = false;
            for (var page = 1; page <= 100; ++page)
            {
                using var response = await m_github.get_releases_async(page, a_cancel);
                var bytes = await read_bounded_async(response.Content, 4 * 1024 * 1024, a_cancel);
                using var document = JsonDocument.Parse(bytes);
                if (document.RootElement.ValueKind != JsonValueKind.Array) throw new InvalidDataException("Release 一覧が配列ではありません");
                foreach (var item in document.RootElement.EnumerateArray())
                {
                    var release = parse_release(item, "engine-v", true);
                    if (release != null) releases.Add(release);
                }
                if (document.RootElement.GetArrayLength() < 100) { completed = true; break; }
            }
            if (!completed) throw new InvalidDataException("Release 一覧の取得上限を超えました");
            var result = releases.GroupBy(item => item.Tag, StringComparer.Ordinal).Select(group => group.First())
                .OrderByDescending(item => item.PublishedUtc).ThenBy(item => item.Tag, StringComparer.Ordinal).Take(10).ToArray();
            HubStateStore.atomic_write(m_cache, JsonSerializer.SerializeToUtf8Bytes(result, HubStateStore.JsonOptions));
            return result;
        }
        catch (Exception error) when (error is HttpRequestException or IOException or JsonException or InvalidDataException)
        {
            LastWarning = "オンライン取得に失敗しました。保存済み一覧を表示します: " + error.Message;
            if (!File.Exists(m_cache)) throw;
            SafePaths.regular_file(m_cache);
            if (new FileInfo(m_cache).Length > 1024 * 1024) throw new InvalidDataException("カタログ Cache が大きすぎます");
            var cached = JsonSerializer.Deserialize<PublishedRelease[]>(File.ReadAllBytes(m_cache), HubStateStore.JsonOptions) ?? [];
            if (cached.Length > 10) throw new InvalidDataException("カタログ Cache の件数が不正です");
            // Cache は署名検証の代わりに使わず、URL も再検証する
            foreach (var item in cached)
                if (!SafePaths.is_version(item.Version) || item.Tag != "engine-v" + item.Version || item.Assets.Any(asset => !GitHubReleaseClient.is_asset_url(asset.Value)))
                    throw new InvalidDataException("カタログ Cache が不正です");
            return cached;
        }
    }

    public static PublishedRelease? parse_release(JsonElement a_item, string a_prefix, bool a_engine)
    {
        if (a_item.GetProperty("draft").GetBoolean()) return null;
        var tag = a_item.GetProperty("tag_name").GetString() ?? "";
        if (!tag.StartsWith(a_prefix, StringComparison.Ordinal)) return null;
        var version = tag[a_prefix.Length..];
        if (!SafePaths.is_version(version)) return null;
        var preview = a_item.GetProperty("prerelease").GetBoolean();
        if (preview != version.Split('+')[0].Contains('-')) return null;
        if (!a_item.TryGetProperty("published_at", out var published) || !DateTimeOffset.TryParse(published.GetString(), out var date)) return null;
        var assets = new Dictionary<string, string>(StringComparer.Ordinal);
        var identifiers = new HashSet<long>();
        foreach (var asset in a_item.GetProperty("assets").EnumerateArray())
        {
            var name = asset.GetProperty("name").GetString() ?? "";
            if (!asset.TryGetProperty("id", out var identifier) || identifier.ValueKind != JsonValueKind.Number ||
                !identifier.TryGetInt64(out var id) || id <= 0 || !identifiers.Add(id)) return null;
            if (!asset.TryGetProperty("url", out var api) || api.ValueKind != JsonValueKind.String) return null;
            var url = api.GetString() ?? "";
            var browser = asset.GetProperty("browser_download_url").GetString() ?? "";
            // 一覧内の ID / API と Tag / File 名の対応を確認してから、API URL だけを保存する
            if (url != GitHubReleaseClient.asset_url(id) || string.IsNullOrWhiteSpace(name) ||
                browser != RepositoryUrl + "/releases/download/" + Uri.EscapeDataString(tag) + "/" + Uri.EscapeDataString(name) ||
                !assets.TryAdd(name, url)) return null;
        }
        var expected = a_engine ? $"CueEngine-{version}-windows-x64.zip" : "releases.win.json";
        if (!assets.ContainsKey(expected) || (a_engine && (!assets.ContainsKey("engine-release.json") || !assets.ContainsKey("engine-release.json.sig")))) return null;
        return new PublishedRelease { Version = version, Tag = tag, IsPreview = preview, PublishedUtc = date, PageUrl = RepositoryUrl + "/releases/tag/" + Uri.EscapeDataString(tag), Assets = assets };
    }

    public static async Task<byte[]> read_bounded_async(HttpContent a_content, int a_limit, CancellationToken a_cancel)
    {
        if (a_content.Headers.ContentLength > a_limit) throw new InvalidDataException("応答が大きすぎます");
        await using var stream = await a_content.ReadAsStreamAsync(a_cancel);
        using var output = new MemoryStream();
        var buffer = new byte[16384];
        for (int count; (count = await stream.ReadAsync(buffer, a_cancel)) != 0;)
        {
            if (output.Length + count > a_limit) throw new InvalidDataException("応答の上限を超えました");
            output.Write(buffer, 0, count);
        }
        return output.ToArray();
    }
}

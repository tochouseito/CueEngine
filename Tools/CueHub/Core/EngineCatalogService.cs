using System.Net.Http.Headers;
using System.Text.Json;

namespace Cue.Hub;

/// <summary>公開済みの Engine だけを列挙する。HttpClient は呼出側が Service より長く所有する</summary>
public sealed class EngineCatalogService(HttpClient a_http, HubStateStore a_store)
{
    public const string RepositoryUrl = "https://github.com/tochouseito/CueEngine";
    private readonly HttpClient m_http = a_http;
    private readonly string m_cache = Path.Combine(a_store.DirectoryPath, "engine-catalog.json");
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
                using var request = new HttpRequestMessage(HttpMethod.Get, $"https://api.github.com/repos/tochouseito/CueEngine/releases?per_page=100&page={page}");
                request.Headers.UserAgent.ParseAdd("CueHub/0.1");
                request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
                using var response = await m_http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, a_cancel);
                response.EnsureSuccessStatusCode();
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
                if (!SafePaths.is_version(item.Version) || item.Tag != "engine-v" + item.Version || item.Assets.Any(asset => !is_asset_url(asset.Value, item.Tag)))
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
        foreach (var asset in a_item.GetProperty("assets").EnumerateArray())
        {
            var name = asset.GetProperty("name").GetString() ?? "";
            var url = asset.GetProperty("browser_download_url").GetString() ?? "";
            if (!is_asset_url(url, tag) || !assets.TryAdd(name, url)) return null;
        }
        var expected = a_engine ? $"CueEngine-{version}-windows-x64.zip" : "releases.win.json";
        if (!assets.ContainsKey(expected) || (a_engine && (!assets.ContainsKey("engine-release.json") || !assets.ContainsKey("engine-release.json.sig")))) return null;
        return new PublishedRelease { Version = version, Tag = tag, IsPreview = preview, PublishedUtc = date, PageUrl = RepositoryUrl + "/releases/tag/" + Uri.EscapeDataString(tag), Assets = assets };
    }

    public static bool is_asset_url(string a_url, string a_tag)
    {
        if (!Uri.TryCreate(a_url, UriKind.Absolute, out var uri) || uri.Scheme != Uri.UriSchemeHttps || uri.Host != "github.com" || !uri.IsDefaultPort || uri.UserInfo.Length != 0 || uri.Query.Length != 0 || uri.Fragment.Length != 0) return false;
        return uri.AbsolutePath.StartsWith("/tochouseito/CueEngine/releases/download/" + Uri.EscapeDataString(a_tag) + "/", StringComparison.Ordinal);
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

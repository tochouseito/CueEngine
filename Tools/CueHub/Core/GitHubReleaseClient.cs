using System.Globalization;

namespace Cue.Hub;

/// <summary>公開 Release の取得を API に限定する。HttpClient は呼出側が所有し、応答は呼出側が破棄する</summary>
public sealed class GitHubReleaseClient(HttpClient a_http)
{
    public const string RepositoryApiUrl = "https://api.github.com/repos/tochouseito/CueEngine";
    private const string k_assetPrefix = RepositoryApiUrl + "/releases/assets/";
    private readonly HttpClient m_http = a_http;

    /// <summary>一覧の JSON と Asset 本体で Accept を分け、同じ HttpClient の既定 Header を変更しない</summary>
    public Task<HttpResponseMessage> get_releases_async(int a_page, CancellationToken a_cancel)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(a_page);
        return get_async($"{RepositoryApiUrl}/releases?per_page=100&page={a_page}", false, a_cancel);
    }

    /// <summary>署名検証前の取得先を自 Repository の Asset API に限定する。標準 Handler が 302 を追跡する</summary>
    public Task<HttpResponseMessage> get_asset_async(string a_url, CancellationToken a_cancel)
    {
        if (!is_asset_url(a_url)) throw new InvalidDataException("Asset API の URL が不正です");
        return get_async(a_url, true, a_cancel);
    }

    /// <summary>Asset ID から正規形の API URL を生成し、Query や任意 Path を受け入れない</summary>
    public static string asset_url(long a_id)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(a_id);
        return k_assetPrefix + a_id.ToString(CultureInfo.InvariantCulture);
    }

    /// <summary>Cache 由来の URL も Host / Repository / 正の ID が完全一致するものだけを許可する</summary>
    public static bool is_asset_url(string? a_url)
    {
        return a_url != null && a_url.StartsWith(k_assetPrefix, StringComparison.Ordinal) &&
            long.TryParse(a_url.AsSpan(k_assetPrefix.Length), NumberStyles.None, CultureInfo.InvariantCulture, out var id) &&
            id > 0 && a_url == asset_url(id);
    }

    private async Task<HttpResponseMessage> get_async(string a_url, bool a_isAsset, CancellationToken a_cancel)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, a_url);
        request.Headers.UserAgent.ParseAdd("CueHub/0.1");
        request.Headers.Accept.ParseAdd(a_isAsset ? "application/octet-stream" : "application/vnd.github+json");
        var response = await m_http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, a_cancel);
        if (!response.IsSuccessStatusCode)
        {
            var status = response.StatusCode;
            response.Dispose();
            throw new HttpRequestException($"GitHub からの取得に失敗しました (HTTP {(int)status}): {a_url}", null, status);
        }
        return response;
    }
}

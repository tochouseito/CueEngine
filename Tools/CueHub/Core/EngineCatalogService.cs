namespace Cue.Hub;

/// <summary>署名検証済み Catalog の Engine だけを列挙する。HttpClient は呼出側が所有する</summary>
public sealed class EngineCatalogService(HttpClient a_http, HubStateStore a_store, PackageVerifier a_verifier)
{
    public const string RepositoryUrl = DistributionClient.RepositoryUrl;
    private readonly DistributionCatalogService m_catalog = new(a_http, a_store, a_verifier);
    public string? LastWarning => m_catalog.LastWarning;

    /// <summary>正式版と Preview を含む最新 10 世代を取得する。取消時は保存済み一覧へ切り替えない</summary>
    public async Task<IReadOnlyList<PublishedRelease>> refresh_async(CancellationToken a_cancel, bool a_forceRefresh = false)
    {
        var catalog = await m_catalog.load_async(a_cancel, a_forceRefresh);
        return catalog.Engines.OrderByDescending(item => item.PublishedUtc)
            .ThenBy(item => item.Tag, StringComparer.Ordinal).ToArray();
    }

    /// <summary>既存の小規模応答取得用に容量制限を維持する。取消は呼出側へ伝播する</summary>
    public static async Task<byte[]> read_bounded_async(HttpContent a_content, int a_limit, CancellationToken a_cancel)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(a_limit);
        if (a_content.Headers.ContentLength > a_limit) throw new InvalidDataException("応答が大きすぎます");
        await using var stream = await a_content.ReadAsStreamAsync(a_cancel);
        using var output = new MemoryStream();
        var buffer = new byte[Math.Min(16384, a_limit)];
        for (int count; (count = await stream.ReadAsync(buffer, a_cancel)) != 0;)
        {
            if (output.Length + count > a_limit) throw new InvalidDataException("応答の上限を超えました");
            output.Write(buffer, 0, count);
        }
        a_cancel.ThrowIfCancellationRequested();
        return output.ToArray();
    }
}

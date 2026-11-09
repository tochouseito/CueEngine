using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Cue.Hub;

/// <summary>署名付き配布一覧と検証済み Cache を所有する。取得と Cache 更新を直列化し取消を伝播する</summary>
public sealed class DistributionCatalogService(HttpClient a_http, HubStateStore a_store, PackageVerifier a_verifier)
{
    private const int k_cacheBytes = ((DistributionLimits.CatalogBytes + 2) / 3 * 4) + DistributionLimits.SignatureBytes * 6 + 1024;
    private static readonly TimeSpan k_cacheTtl = TimeSpan.FromMinutes(30);
    private readonly DistributionClient m_distribution = new(a_http);
    private readonly PackageVerifier m_verifier = a_verifier;
    private readonly string m_cache = Path.Combine(a_store.DirectoryPath, "distribution-catalog-v1.cache.json");
    private readonly SemaphoreSlim m_operation = new(1, 1);
    private VerifiedCatalog? m_current;
    public string? LastWarning { get; private set; }

    /// <summary>未加工の署名対象 Byte 列を保持する。返却した可変 List を次回の信頼根拠にしない</summary>
    private sealed record VerifiedCatalog(DistributionCatalog Catalog, byte[] Payload, string Signature, DateTimeOffset FetchedUtc);

    /// <summary>30 分以内の検証済み一覧を再利用し、取得失敗時は検証済み Cache だけを返す</summary>
    public async Task<DistributionCatalog> load_async(CancellationToken a_cancel, bool a_forceRefresh = false)
    {
        await m_operation.WaitAsync(a_cancel);
        try
        {
            a_cancel.ThrowIfCancellationRequested();
            LastWarning = null;
            if (!a_forceRefresh && m_current != null && is_fresh(m_current.FetchedUtc))
                return read_catalog(m_current.Payload, m_current.Signature);

            VerifiedCatalog? cached = null;
            try { cached = await read_cache_async(a_cancel); }
            catch (Exception error) when (is_recoverable(error, a_cancel))
            {
                LastWarning = "保存済み配布一覧を検証できませんでした。オンラインで再取得します";
            }
            var fallback = newest(m_current, cached);
            if (!a_forceRefresh && fallback != null && is_fresh(fallback.FetchedUtc))
            {
                m_current = fallback;
                return read_catalog(fallback.Payload, fallback.Signature);
            }

            try
            {
                var payload = await m_distribution.get_bytes_async(DistributionClient.CatalogUrl,
                    DistributionLimits.CatalogBytes, a_cancel);
                var signatureBytes = await m_distribution.get_bytes_async(DistributionClient.CatalogUrl + ".sig",
                    DistributionLimits.SignatureBytes, a_cancel);
                if (signatureBytes.Any(value => value > 127)) throw new InvalidDataException("配布一覧の署名が ASCII ではありません");
                var signature = Encoding.ASCII.GetString(signatureBytes);
                var catalog = read_catalog(payload, signature);
                reject_rollback(catalog, fallback);
                a_cancel.ThrowIfCancellationRequested();
                var verified = new VerifiedCatalog(catalog, payload, signature, DateTimeOffset.UtcNow);
                await save_cache_async(verified, a_cancel);
                m_current = verified;
                LastWarning = null;
                a_cancel.ThrowIfCancellationRequested();
                return read_catalog(payload, signature);
            }
            catch (Exception error) when (is_recoverable(error, a_cancel))
            {
                // 他の Process が保存した新しい検証済み一覧も、失敗時に再確認する
                try { fallback = newest(fallback, await read_cache_async(a_cancel)); }
                catch (Exception cacheError) when (is_recoverable(cacheError, a_cancel)) { }
                LastWarning = "オンラインの配布一覧を確認できません: " + error.Message;
                if (fallback == null) throw;
                LastWarning = "検証済みの保存済み一覧を表示します: " + error.Message;
                a_cancel.ThrowIfCancellationRequested();
                m_current = fallback;
                return read_catalog(fallback.Payload, fallback.Signature);
            }
        }
        finally { m_operation.Release(); }
    }

    /// <summary>署名と厳密な Schema を確認した後にだけ公開モデルを生成する</summary>
    private DistributionCatalog read_catalog(byte[] a_payload, string a_signature)
    {
        if (a_payload.Length is 0 or > DistributionLimits.CatalogBytes || string.IsNullOrWhiteSpace(a_signature) ||
            a_signature.Length > DistributionLimits.SignatureBytes || a_signature.Any(value => value > 127))
            throw new InvalidDataException("配布一覧または署名の容量が不正です");
        m_verifier.verify_signature(a_payload, a_signature, DistributionLimits.CatalogBytes);
        using var document = JsonDocument.Parse(a_payload);
        var root = document.RootElement;
        require_unique(root);
        require_fields(root, ["schemaVersion", "generatedAt", "engines", "hubs"]);
        if (root.GetProperty("schemaVersion").ValueKind != JsonValueKind.Number ||
            !root.GetProperty("schemaVersion").TryGetInt32(out var schema) || schema != 1)
            throw new InvalidDataException("未対応の配布一覧です");
        var generatedAt = read_utc(root.GetProperty("generatedAt"));
        if (generatedAt > DateTimeOffset.UtcNow.AddMinutes(10))
            throw new InvalidDataException("配布一覧の発行時刻が未来を指しています");
        return new DistributionCatalog
        {
            SchemaVersion = schema,
            GeneratedAt = generatedAt,
            Engines = read_releases(root.GetProperty("engines"), "engine"),
            Hubs = read_releases(root.GetProperty("hubs"), "hub"),
        };
    }

    /// <summary>製品、Channel、配布先と必須 Asset を確認し、候補を黙って取り除かない</summary>
    private static List<PublishedRelease> read_releases(JsonElement a_items, string a_product)
    {
        if (a_items.ValueKind != JsonValueKind.Array || a_items.GetArrayLength() > 10)
            throw new InvalidDataException("配布一覧の世代数が不正です");
        var result = new List<PublishedRelease>();
        var tags = new HashSet<string>(StringComparer.Ordinal);
        foreach (var item in a_items.EnumerateArray())
        {
            require_fields(item, ["version", "tag", "isPreview", "publishedUtc", "pageUrl", "assets"]);
            var version = read_string(item.GetProperty("version"));
            var tag = read_string(item.GetProperty("tag"));
            var previewValue = item.GetProperty("isPreview");
            var pageUrl = read_string(item.GetProperty("pageUrl"));
            if (!SafePaths.is_version(version) || tag != a_product + "-v" + version || !tags.Add(tag) ||
                previewValue.ValueKind is not (JsonValueKind.True or JsonValueKind.False) ||
                previewValue.GetBoolean() != version.Split('+')[0].Contains('-') ||
                pageUrl != DistributionClient.RepositoryUrl + "/releases/tag/" + Uri.EscapeDataString(tag))
                throw new InvalidDataException("配布一覧の製品、Tag または Channel が不正です");
            var assetsValue = item.GetProperty("assets");
            if (assetsValue.ValueKind != JsonValueKind.Object || assetsValue.EnumerateObject().Count() > 1000)
                throw new InvalidDataException("配布一覧の Asset が不正です");
            var assets = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var asset in assetsValue.EnumerateObject())
            {
                var url = read_string(asset.Value);
                if (!DistributionClient.is_asset_url(url, tag, asset.Name) || !assets.TryAdd(asset.Name, url))
                    throw new InvalidDataException("配布一覧に許可されない Asset URL があります");
            }
            require_assets(assets, a_product, version);
            result.Add(new PublishedRelease
            {
                Version = version, Tag = tag, IsPreview = previewValue.GetBoolean(),
                PublishedUtc = read_utc(item.GetProperty("publishedUtc")), PageUrl = pageUrl, Assets = assets,
            });
        }
        return result;
    }

    /// <summary>Engine の単体 ZIP と連番 Part を区別し、Hub の更新 File と Setup を要求する</summary>
    private static void require_assets(Dictionary<string, string> a_assets, string a_product, string a_version)
    {
        var manifest = a_product + "-release.json";
        if (!a_assets.ContainsKey(manifest) || !a_assets.ContainsKey(manifest + ".sig"))
            throw new InvalidDataException("配布一覧に署名付き Manifest がありません");
        if (a_product == "hub")
        {
            if (!a_assets.ContainsKey($"CueEngine.Hub-{a_version}-full.nupkg") ||
                !a_assets.ContainsKey("releases.win.json") || !a_assets.ContainsKey("CueEngine.Hub-win-Setup.exe"))
                throw new InvalidDataException("Hub の更新 Package、Feed または Setup がありません");
            return;
        }
        var package = $"CueEngine-{a_version}-windows-x64.zip";
        var parts = a_assets.Keys.Where(name => name.StartsWith(package + ".", StringComparison.Ordinal)).ToArray();
        if (a_assets.ContainsKey(package))
        {
            if (parts.Length != 0) throw new InvalidDataException("単体 ZIP と分割 Package が混在しています");
            return;
        }
        if (parts.Length is 0 or > DistributionLimits.Parts)
            throw new InvalidDataException("Engine Package の Part 数が不正です");
        for (var index = 1; index <= parts.Length; ++index)
            if (!a_assets.ContainsKey(package + "." + index.ToString("D3", System.Globalization.CultureInfo.InvariantCulture)))
                throw new InvalidDataException("Engine Package の Part が連続していません");
    }

    /// <summary>型と UTC Offset を確認し、Local 時刻へ解釈し直さない</summary>
    private static DateTimeOffset read_utc(JsonElement a_value)
    {
        if (a_value.ValueKind != JsonValueKind.String) throw new InvalidDataException("配布一覧の UTC 時刻が不正です");
        var text = a_value.GetString()!;
        if (text.Length < 20 || text[10] != 'T' ||
            !(text.EndsWith('Z') || text.EndsWith("+00:00", StringComparison.Ordinal)) ||
            !a_value.TryGetDateTimeOffset(out var date) ||
            date.Offset != TimeSpan.Zero || date < DateTimeOffset.UnixEpoch)
            throw new InvalidDataException("配布一覧の UTC 時刻が不正です");
        return date;
    }

    /// <summary>必須文字列の Null と空値を拒否する</summary>
    private static string read_string(JsonElement a_value)
    {
        if (a_value.ValueKind != JsonValueKind.String || string.IsNullOrWhiteSpace(a_value.GetString()))
            throw new InvalidDataException("配布一覧の必須文字列が不正です");
        return a_value.GetString()!;
    }

    /// <summary>列挙した項目以外を受け入れず、大文字小文字の揺れも拒否する</summary>
    private static void require_fields(JsonElement a_value, string[] a_fields)
    {
        if (a_value.ValueKind != JsonValueKind.Object || a_value.EnumerateObject().Count() != a_fields.Length ||
            a_value.EnumerateObject().Any(item => !a_fields.Contains(item.Name, StringComparer.Ordinal)))
            throw new InvalidDataException("配布一覧の項目が不正です");
    }

    /// <summary>すべての階層で曖昧な重複キーを拒否する</summary>
    private static void require_unique(JsonElement a_value)
    {
        if (a_value.ValueKind == JsonValueKind.Object)
        {
            var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var item in a_value.EnumerateObject())
            {
                if (!names.Add(item.Name)) throw new InvalidDataException("配布一覧の項目が重複しています");
                require_unique(item.Value);
            }
        }
        else if (a_value.ValueKind == JsonValueKind.Array)
            foreach (var item in a_value.EnumerateArray()) require_unique(item);
    }

    /// <summary>Cache の Payload を再署名せず読み戻し、利用するたびに署名と Schema を確認する</summary>
    private async Task<VerifiedCatalog?> read_cache_async(CancellationToken a_cancel)
    {
        a_cancel.ThrowIfCancellationRequested();
        SafePaths.no_links(m_cache);
        if (!File.Exists(m_cache)) return null;
        SafePaths.regular_file(m_cache);
        await using var file = new FileStream(m_cache, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete,
            16384, FileOptions.Asynchronous | FileOptions.SequentialScan);
        if (file.Length > k_cacheBytes) throw new InvalidDataException("配布一覧 Cache が大きすぎます");
        using var output = new MemoryStream();
        var buffer = new byte[16384];
        for (int count; (count = await file.ReadAsync(buffer, a_cancel)) != 0;)
        {
            if (output.Length + count > k_cacheBytes) throw new InvalidDataException("配布一覧 Cache が大きすぎます");
            output.Write(buffer, 0, count);
        }
        using var document = JsonDocument.Parse(output.ToArray());
        var root = document.RootElement;
        require_unique(root);
        require_fields(root, ["schemaVersion", "fetchedUtc", "payload", "signature"]);
        if (root.GetProperty("schemaVersion").ValueKind != JsonValueKind.Number ||
            !root.GetProperty("schemaVersion").TryGetInt32(out var schema) || schema != 1)
            throw new InvalidDataException("未対応の配布一覧 Cache です");
        var payload = Convert.FromBase64String(read_string(root.GetProperty("payload")));
        var signature = read_string(root.GetProperty("signature"));
        var fetchedUtc = read_utc(root.GetProperty("fetchedUtc"));
        var catalog = read_catalog(payload, signature);
        a_cancel.ThrowIfCancellationRequested();
        return new VerifiedCatalog(catalog, payload, signature, fetchedUtc);
    }

    /// <summary>同一 Cache を書く別 Process と排他し、より新しい検証結果の上書きを防ぐ</summary>
    private async Task save_cache_async(VerifiedCatalog a_value, CancellationToken a_cancel)
    {
        var lockFile = m_cache + ".lock";
        SafePaths.no_links(lockFile);
        using var held = await acquire_cache_lock_async(lockFile, a_cancel);
        VerifiedCatalog? previous = null;
        try { previous = await read_cache_async(a_cancel); }
        catch (Exception error) when (is_recoverable(error, a_cancel)) { }
        reject_rollback(a_value.Catalog, newest(m_current, previous));
        var envelope = JsonSerializer.SerializeToUtf8Bytes(new
        {
            schemaVersion = 1, fetchedUtc = a_value.FetchedUtc, payload = a_value.Payload, signature = a_value.Signature,
        }, HubStateStore.JsonOptions);
        if (envelope.Length > k_cacheBytes) throw new InvalidDataException("配布一覧 Cache が大きすぎます");
        a_cancel.ThrowIfCancellationRequested();
        HubStateStore.atomic_write(m_cache, envelope);
    }

    /// <summary>短時間だけ排他を再試行し、取消を待ち時間へ反映する</summary>
    private static async Task<FileStream> acquire_cache_lock_async(string a_file, CancellationToken a_cancel)
    {
        for (var attempt = 0; ; ++attempt)
        {
            a_cancel.ThrowIfCancellationRequested();
            SafePaths.no_links(a_file);
            try { return new FileStream(a_file, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
            catch (IOException) when (attempt < 19) { await Task.Delay(50, a_cancel); }
        }
    }

    /// <summary>Local Cache の更新時刻が未来を指す場合は TTL を延長しない</summary>
    private static bool is_fresh(DateTimeOffset a_fetchedUtc)
    {
        var elapsed = DateTimeOffset.UtcNow - a_fetchedUtc;
        return elapsed >= TimeSpan.Zero && elapsed < k_cacheTtl;
    }

    /// <summary>発行時刻を信頼根拠にし、再取得日時で古い Catalog を優先しない</summary>
    private static VerifiedCatalog? newest(VerifiedCatalog? a_left, VerifiedCatalog? a_right)
    {
        if (a_left == null) return a_right;
        if (a_right == null) return a_left;
        var order = a_left.Catalog.GeneratedAt.CompareTo(a_right.Catalog.GeneratedAt);
        return order > 0 || order == 0 && a_left.FetchedUtc >= a_right.FetchedUtc ? a_left : a_right;
    }

    /// <summary>オンライン応答と Cache の公開順序を逆転させない</summary>
    private static void reject_rollback(DistributionCatalog a_catalog, VerifiedCatalog? a_previous)
    {
        if (a_previous != null && a_catalog.GeneratedAt < a_previous.Catalog.GeneratedAt)
            throw new InvalidDataException("配布一覧の発行時刻が保存済み一覧より古くなっています");
    }

    /// <summary>利用者の取消を除き、検証済み Cache へ切り替えられる失敗だけを判定する</summary>
    private static bool is_recoverable(Exception a_error, CancellationToken a_cancel) => !a_cancel.IsCancellationRequested &&
        a_error is HttpRequestException or IOException or InvalidDataException or JsonException or CryptographicException or FormatException
            or UnauthorizedAccessException or OperationCanceledException;
}

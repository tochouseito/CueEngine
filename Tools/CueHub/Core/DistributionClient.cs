using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;

namespace Cue.Hub;

/// <summary>公開配布 URL から有界取得する。HttpClient と Cache の配置は呼出側が所有する</summary>
public sealed class DistributionClient(HttpClient a_http)
{
    public const string CatalogUrl = "https://tochouseito.github.io/CueEngine/distribution/catalog-v1.json";
    public const string RepositoryUrl = "https://github.com/tochouseito/CueEngine";
    private const string k_assetPrefix = RepositoryUrl + "/releases/download/";
    private const int k_bufferSize = 131072;
    private const int k_attempts = 3;
    private static readonly TimeSpan k_idleTimeout = TimeSpan.FromSeconds(60);
    private readonly HttpClient m_http = a_http;

    /// <summary>Tag / File 名を検証し、Query や別 Repository を含まない正規形だけを許可する</summary>
    public static bool is_asset_url(string? a_url, string a_tag, string a_name)
    {
        if (a_url == null || !is_tag(a_tag) || !is_file_name(a_name)) return false;
        return a_url == k_assetPrefix + Uri.EscapeDataString(a_tag) + "/" + Uri.EscapeDataString(a_name);
    }

    /// <summary>Manifest / 署名などの小さい応答を上限内で読む。取消は再試行しない</summary>
    public async Task<byte[]> get_bytes_async(string a_url, int a_limit, CancellationToken a_cancel)
    {
        validate_url(a_url);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(a_limit);
        for (var attempt = 0; ; ++attempt)
        {
            a_cancel.ThrowIfCancellationRequested();
            try
            {
                using var request = make_request(a_url);
                using var response = await m_http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, a_cancel);
                require_status(response, HttpStatusCode.OK);
                if (response.Content.Headers.ContentLength > a_limit)
                    throw new InvalidDataException("配布メタデータの容量上限を超えています");
                await using var input = await response.Content.ReadAsStreamAsync(a_cancel);
                using var idle = CancellationTokenSource.CreateLinkedTokenSource(a_cancel);
                using var output = new MemoryStream(Math.Min(a_limit, 65536));
                var buffer = new byte[Math.Min(a_limit, k_bufferSize)];
                while (true)
                {
                    idle.CancelAfter(k_idleTimeout);
                    var count = await input.ReadAsync(buffer, idle.Token);
                    idle.CancelAfter(Timeout.InfiniteTimeSpan);
                    if (count == 0) break;
                    if (count > a_limit - output.Length)
                        throw new InvalidDataException("配布メタデータの容量上限を超えています");
                    output.Write(buffer, 0, count);
                }
                if (response.Content.Headers.ContentLength is long expected && output.Length != expected)
                    throw new IOException("配布メタデータの応答が途中で切れました");
                a_cancel.ThrowIfCancellationRequested();
                return output.ToArray();
            }
            catch (Exception error) when (can_retry(error, a_cancel) && attempt + 1 < k_attempts)
            {
                await Task.Delay(TimeSpan.FromMilliseconds(250 * (attempt + 1)), a_cancel);
            }
            catch (HttpRequestException error)
            {
                a_cancel.ThrowIfCancellationRequested();
                throw sanitize_http(error);
            }
            catch (OperationCanceledException) when (!a_cancel.IsCancellationRequested)
            {
                throw new HttpRequestException("配布 Server から一定時間応答がありません。接続を確認して再実行してください");
            }
        }
    }

    /// <summary>排他的 Cache に再開可能な途中 File を保持し、署名済み Size / SHA256 と一致した時だけ公開する</summary>
    public async Task download_async(string a_url, string a_file, long a_size, string a_sha256,
        IProgress<InstallProgress>? a_progress, CancellationToken a_cancel)
    {
        validate_url(a_url);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(a_size);
        validate_hash(a_sha256);
        a_cancel.ThrowIfCancellationRequested();
        var file = SafePaths.full(a_file);
        var partial = file + ".partial";
        var lockFile = file + ".lock";
        SafePaths.no_links(file);
        SafePaths.no_links(partial);
        SafePaths.no_links(lockFile);
        var parent = Path.GetDirectoryName(file) ?? throw new InvalidDataException("Cache の配置が不正です");
        Directory.CreateDirectory(parent);
        // Lock File は残し、解放と削除の間に別 Process が割り込む競合を避ける
        using var ownership = new FileStream(lockFile, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        if (File.Exists(file))
        {
            SafePaths.regular_file(file);
            await using var existing = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read,
                k_bufferSize, FileOptions.Asynchronous | FileOptions.SequentialScan);
            if (existing.Length == a_size && await matches_hash(existing, a_sha256, a_cancel)) return;
        }
        var hasInvalidPartial = false;
        try
        {
            await using (var output = new FileStream(partial, FileMode.OpenOrCreate, FileAccess.ReadWrite,
                FileShare.None, k_bufferSize, FileOptions.Asynchronous | FileOptions.SequentialScan))
            {
                if (output.Length > a_size)
                {
                    hasInvalidPartial = true;
                    throw new InvalidDataException("途中 Cache の容量が署名済み容量を超えています");
                }
                for (var attempt = 0; output.Length < a_size; ++attempt)
                {
                    a_cancel.ThrowIfCancellationRequested();
                    try
                    {
                        await transfer_async(a_url, output, a_size, a_progress, a_cancel);
                        break;
                    }
                    catch (Exception error) when (can_retry(error, a_cancel) && attempt + 1 < k_attempts)
                    {
                        a_progress?.Report(new("取得が中断されました。途中から再試行します"));
                        await Task.Delay(TimeSpan.FromMilliseconds(250 * (attempt + 1)), a_cancel);
                    }
                    catch (HttpRequestException error)
                    {
                        a_cancel.ThrowIfCancellationRequested();
                        throw sanitize_http(error);
                    }
                    catch (OperationCanceledException) when (!a_cancel.IsCancellationRequested)
                    {
                        throw new HttpRequestException("配布 Server から一定時間応答がありません。途中 Cache を保持して中断しました");
                    }
                }
                await output.FlushAsync(a_cancel);
                output.Position = 0;
                if (output.Length != a_size || !await matches_hash(output, a_sha256, a_cancel))
                {
                    hasInvalidPartial = true;
                    throw new InvalidDataException("取得した配布 File の SHA256 / 容量が一致しません");
                }
            }
            a_cancel.ThrowIfCancellationRequested();
            SafePaths.no_links(file);
            SafePaths.regular_file(partial);
            // 既存の不一致 File も、完全に検証できた File が用意できるまでは保持する
            File.Move(partial, file, true);
            a_progress?.Report(new("取得と検証が完了しました", 100));
        }
        finally
        {
            if (hasInvalidPartial)
            {
                SafePaths.no_links(partial);
                File.Delete(partial);
            }
        }
    }

    private async Task transfer_async(string a_url, FileStream a_output, long a_size,
        IProgress<InstallProgress>? a_progress, CancellationToken a_cancel)
    {
        var start = a_output.Length;
        using var request = make_request(a_url);
        if (start > 0) request.Headers.Range = new RangeHeaderValue(start, null);
        using var response = await m_http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, a_cancel);
        if (response.StatusCode == HttpStatusCode.PartialContent)
        {
            var range = response.Content.Headers.ContentRange;
            if (range == null || range.Unit != "bytes" || range.From != start || range.To != a_size - 1 || range.Length != a_size)
                throw new InvalidDataException("再開応答の Content-Range が署名済み容量と一致しません");
        }
        else
        {
            require_status(response, HttpStatusCode.OK);
            // Range を無視する Server は先頭から返すため、既存の途中 Byte に連結しない
            start = 0;
            a_output.SetLength(0);
        }
        var expected = a_size - start;
        if (response.Content.Headers.ContentLength is long length && length != expected)
            throw new InvalidDataException("応答の Content-Length が署名済み容量と一致しません");
        a_output.Position = start;
        await using var input = await response.Content.ReadAsStreamAsync(a_cancel);
        using var idle = CancellationTokenSource.CreateLinkedTokenSource(a_cancel);
        var buffer = new byte[k_bufferSize];
        var progressTimer = Stopwatch.StartNew();
        var lastPercent = -1.0;
        long received = 0;
        while (true)
        {
            // HeadersRead 後は HttpClient.Timeout が効かないため、Body の無応答も有界にする
            idle.CancelAfter(k_idleTimeout);
            var count = await input.ReadAsync(buffer, idle.Token);
            idle.CancelAfter(Timeout.InfiniteTimeSpan);
            if (count == 0) break;
            if (count > expected - received)
                throw new InvalidDataException("配布 File の応答が署名済み容量を超えています");
            await a_output.WriteAsync(buffer.AsMemory(0, count), a_cancel);
            received += count;
            var percent = (start + received) * 100.0 / a_size;
            if (percent - lastPercent >= 1.0 || progressTimer.ElapsedMilliseconds >= 100)
            {
                a_progress?.Report(new("配布 File を取得しています", percent));
                lastPercent = percent;
                progressTimer.Restart();
            }
        }
        if (received != expected) throw new IOException("配布 File の応答が途中で切れました");
    }

    private static HttpRequestMessage make_request(string a_url)
    {
        var request = new HttpRequestMessage(HttpMethod.Get, a_url);
        request.Headers.UserAgent.ParseAdd("CueHub/0.1");
        request.Headers.Accept.ParseAdd("application/octet-stream");
        return request;
    }

    private static void require_status(HttpResponseMessage a_response, HttpStatusCode a_expected)
    {
        if (a_response.StatusCode == a_expected) return;
        var status = a_response.StatusCode;
        var message = $"配布 Server からの取得に失敗しました (HTTP {(int)status})";
        if (status == HttpStatusCode.TooManyRequests)
        {
            var retry = a_response.Headers.RetryAfter;
            message += retry?.Delta is TimeSpan delay
                ? $"。Server の制限が解除されるまで約 {Math.Ceiling(delay.TotalSeconds)} 秒待って再実行してください"
                : retry?.Date is DateTimeOffset date
                    ? $"。{date:O} 以降まで待って再実行してください"
                    : "。Server の制限が解除されるまで待って再実行してください";
        }
        throw new HttpRequestException(message, null, status);
    }

    private static bool can_retry(Exception a_error, CancellationToken a_cancel)
    {
        if (a_cancel.IsCancellationRequested) return false;
        return a_error is HttpRequestException http
            ? http.StatusCode is null or HttpStatusCode.InternalServerError or HttpStatusCode.BadGateway
                or HttpStatusCode.ServiceUnavailable or HttpStatusCode.GatewayTimeout
            : a_error is IOException or OperationCanceledException;
    }

    private static HttpRequestException sanitize_http(HttpRequestException a_error)
    {
        // HttpClient の診断に署名付き Redirect URL が含まれても UI / Log へ流さない
        if (a_error.StatusCode is HttpStatusCode status)
            return new HttpRequestException(a_error.Message.StartsWith("配布 Server", StringComparison.Ordinal)
                ? a_error.Message : $"配布 Server からの取得に失敗しました (HTTP {(int)status})", null, status);
        return new HttpRequestException("配布 Server との通信に失敗しました。接続を確認して再実行してください");
    }

    private static void validate_url(string a_url)
    {
        if (a_url == CatalogUrl || a_url == CatalogUrl + ".sig") return;
        if (a_url.StartsWith(k_assetPrefix, StringComparison.Ordinal))
        {
            var parts = a_url[k_assetPrefix.Length..].Split('/');
            if (parts.Length == 2 && is_asset_url(a_url, Uri.UnescapeDataString(parts[0]), Uri.UnescapeDataString(parts[1]))) return;
        }
        throw new InvalidDataException("許可された公開配布 URL ではありません");
    }

    private static bool is_tag(string a_tag) => !string.IsNullOrEmpty(a_tag) && a_tag.Length <= 136 &&
        char.IsAsciiLetterOrDigit(a_tag[0]) && a_tag.All(value => char.IsAsciiLetterOrDigit(value) || value is '-' or '_' or '.' or '+');

    private static bool is_file_name(string a_name)
    {
        if (string.IsNullOrEmpty(a_name) || a_name.Contains('/')) return false;
        try { SafePaths.validate_relative(a_name); return true; }
        catch (InvalidDataException) { return false; }
    }

    private static void validate_hash(string a_hash)
    {
        if (a_hash.Length != 64 || !a_hash.All(char.IsAsciiHexDigit))
            throw new InvalidDataException("配布 File の SHA256 が不正です");
    }

    private static async Task<bool> matches_hash(Stream a_input, string a_hash, CancellationToken a_cancel)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[k_bufferSize];
        for (int count; (count = await a_input.ReadAsync(buffer, a_cancel)) != 0;)
            hash.AppendData(buffer, 0, count);
        a_cancel.ThrowIfCancellationRequested();
        return Convert.ToHexString(hash.GetHashAndReset()).Equals(a_hash, StringComparison.OrdinalIgnoreCase);
    }
}

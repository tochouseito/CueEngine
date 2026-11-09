using System.IO.Compression;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using Cue.Hub;

namespace Cue.Hub.Tests;

internal static partial class Program
{
    private const string k_transferTag = "engine-v1.0.0";
    private const string k_transferName = "CueEngine-1.0.0-windows-x64.zip";
    private const string k_transferUrl = DistributionClient.RepositoryUrl + "/releases/download/" + k_transferTag + "/" + k_transferName;

    private static async Task test_distribution_transfers(string a_root)
    {
        var root = Path.Combine(a_root, "DistributionTransfers");
        Directory.CreateDirectory(root);
        await test_distribution_path_links(root);
        var bytes = Enumerable.Range(0, 251).Select(value => (byte)value).ToArray();
        var hash = transfer_hash(bytes);
        var longVersion = "1.0.0-" + new string('a', 122);
        var longTag = "engine-v" + longVersion;
        check(SafePaths.is_version(longVersion) && DistributionClient.is_asset_url(
            DistributionClient.RepositoryUrl + "/releases/download/" + longTag + "/a.zip", longTag, "a.zip"),
            "128-character version and product tag prefix remain compatible");
        using (var handler = new DistributionFixtureHandler((_, _) => Task.FromResult(transfer_response(bytes))))
        using (var http = new HttpClient(handler))
        {
            http.DefaultRequestHeaders.Accept.ParseAdd("application/json");
            var client = new DistributionClient(http);
            check((await client.get_bytes_async(DistributionClient.CatalogUrl, bytes.Length, CancellationToken.None)).SequenceEqual(bytes), "bounded distribution metadata");
            check(handler.HasUserAgent && handler.Urls.All(url => !url.Contains("api.github.com", StringComparison.Ordinal)), "distribution UA and no API request");
            check(http.DefaultRequestHeaders.Accept.Single().MediaType == "application/json" && !http.DefaultRequestHeaders.UserAgent.Any(), "distribution leaves client defaults unchanged");
            await rejected(() => client.get_bytes_async(DistributionClient.CatalogUrl, bytes.Length - 1, CancellationToken.None), "metadata advertised length limit");
            foreach (var url in new[]
            {
                "https://api.github.com/repos/tochouseito/CueEngine/releases/assets/1",
                "http://github.com/tochouseito/CueEngine/releases/download/engine-v1.0.0/a.zip",
                k_transferUrl + "?token=secret", k_transferUrl + "#fragment",
                "https://github.com/other/CueEngine/releases/download/engine-v1.0.0/a.zip",
                DistributionClient.RepositoryUrl + "/releases/download/../a.zip",
                DistributionClient.RepositoryUrl + "/releases/download/engine-v1.0.0/%2Foutside.zip",
                DistributionClient.RepositoryUrl + "/releases/download/engine-v1.0.0/CON.zip"
            })
                await rejected(() => client.get_bytes_async(url, 1024, CancellationToken.None), "foreign or noncanonical distribution URL");
            check(handler.Calls == 2, "invalid URLs never reach HTTP handler");
            check(DistributionClient.is_asset_url(k_transferUrl, k_transferTag, k_transferName) &&
                !DistributionClient.is_asset_url(k_transferUrl, k_transferTag, "../" + k_transferName) &&
                !DistributionClient.is_asset_url(k_transferUrl, "../" + k_transferTag, k_transferName), "release tag and filename exact binding");
        }
        using (var handler = new DistributionFixtureHandler((_, _) => Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StreamContent(new DistributionReadStream(bytes))
        })))
        using (var http = new HttpClient(handler))
            await rejected(() => new DistributionClient(http).get_bytes_async(DistributionClient.CatalogUrl, 20, CancellationToken.None), "metadata streamed length limit");

        var resumed = Path.Combine(root, "resumed.zip");
        File.WriteAllBytes(resumed + ".partial", bytes[..79]);
        using (var handler = new DistributionFixtureHandler((request, _) =>
        {
            check(request.Headers.Range?.Ranges.Single().From == 79, "range starts at retained partial length");
            return Task.FromResult(transfer_response(bytes[79..], 79, bytes.Length));
        }))
        using (var http = new HttpClient(handler))
        {
            await new DistributionClient(http).download_async(k_transferUrl, resumed, bytes.Length, hash, null, CancellationToken.None);
            check(File.ReadAllBytes(resumed).SequenceEqual(bytes) && !File.Exists(resumed + ".partial"), "206 resume publishes verified cache");
            await new DistributionClient(http).download_async(k_transferUrl, resumed, bytes.Length, hash, null, CancellationToken.None);
            check(handler.Calls == 1, "verified complete cache avoids download");
        }
        var ignored = Path.Combine(root, "ignored-range.zip");
        File.WriteAllBytes(ignored + ".partial", bytes[..79]);
        using (var handler = new DistributionFixtureHandler((_, _) => Task.FromResult(transfer_response(bytes))))
        using (var http = new HttpClient(handler))
        {
            await new DistributionClient(http).download_async(k_transferUrl, ignored, bytes.Length, hash, null, CancellationToken.None);
            check(File.ReadAllBytes(ignored).SequenceEqual(bytes), "200 ignoring Range restarts without append");
        }
        foreach (var rangeError in new[] { "from", "to", "total", "missing" })
        {
            var path = Path.Combine(root, "range-error-" + rangeError + ".zip");
            File.WriteAllBytes(path + ".partial", bytes[..79]);
            using var handler = new DistributionFixtureHandler((_, _) =>
            {
                var response = transfer_response(bytes[79..], 79, bytes.Length);
                response.Content.Headers.ContentRange = rangeError switch
                {
                    "from" => new ContentRangeHeaderValue(80, bytes.Length - 1, bytes.Length),
                    "to" => new ContentRangeHeaderValue(79, bytes.Length - 2, bytes.Length),
                    "total" => new ContentRangeHeaderValue(79, bytes.Length - 1, bytes.Length + 1),
                    _ => null
                };
                return Task.FromResult(response);
            });
            using var http = new HttpClient(handler);
            await rejected(() => new DistributionClient(http).download_async(k_transferUrl, path, bytes.Length, hash, null, CancellationToken.None), "reject wrong Content-Range " + rangeError);
            check(handler.Calls == 1 && !File.Exists(path) && File.ReadAllBytes(path + ".partial").SequenceEqual(bytes[..79]), "bad range preserves previous partial");
        }
        foreach (var interruption in new[] { "eof", "io", "timeout" })
        {
            var path = Path.Combine(root, interruption + ".zip");
            var calls = 0;
            using var handler = new DistributionFixtureHandler((request, _) =>
            {
                if (++calls == 1)
                {
                    Action? atEnd = interruption switch
                    {
                        "io" => () => throw new IOException("fixture disconnect"),
                        "timeout" => () => throw new OperationCanceledException("fixture idle timeout"),
                        _ => null
                    };
                    var content = new StreamContent(new DistributionReadStream(bytes[..51], atEnd));
                    content.Headers.ContentLength = bytes.Length;
                    return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = content });
                }
                check(request.Headers.Range?.Ranges.Single().From == 51, "retry reuses bytes received before EOF / I/O / timeout");
                return Task.FromResult(transfer_response(bytes[51..], 51, bytes.Length));
            });
            using var http = new HttpClient(handler);
            await new DistributionClient(http).download_async(k_transferUrl, path, bytes.Length, hash, null, CancellationToken.None);
            check(calls == 2 && File.ReadAllBytes(path).SequenceEqual(bytes), "interrupted stream resumes within retry limit");
        }
        var progressBytes = new byte[20000];
        var progressPath = Path.Combine(root, "progress.zip");
        using (var handler = new DistributionFixtureHandler((_, _) => Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StreamContent(new DistributionReadStream(progressBytes, null, 1))
        })))
        using (var http = new HttpClient(handler))
        {
            var progress = new DistributionProgress();
            await new DistributionClient(http).download_async(k_transferUrl, progressPath, progressBytes.Length, transfer_hash(progressBytes), progress, CancellationToken.None);
            check(progress.Values.Count < 300 && progress.Values[^1].Percent == 100 &&
                progress.Values[^1].Message.Contains("検証", StringComparison.Ordinal), "small reads throttle UI updates and always report verified completion");
        }
        var cancelledPath = Path.Combine(root, "cancelled.zip");
        using (var cancel = new CancellationTokenSource())
        using (var handler = new DistributionFixtureHandler((_, _) =>
        {
            var content = new StreamContent(new DistributionReadStream(bytes[..51], () => cancel.Cancel()));
            content.Headers.ContentLength = bytes.Length;
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = content });
        }))
        using (var http = new HttpClient(handler))
        {
            await rejected(() => new DistributionClient(http).download_async(k_transferUrl, cancelledPath, bytes.Length, hash, null, cancel.Token), "cancel interrupted distribution transfer");
            check(cancel.IsCancellationRequested && handler.Calls == 1 && !File.Exists(cancelledPath) &&
                File.ReadAllBytes(cancelledPath + ".partial").SequenceEqual(bytes[..51]), "cancellation retains partial and does not retry");
        }
        var corrupt = Path.Combine(root, "corrupt.zip");
        var original = Encoding.ASCII.GetBytes("previous cache is preserved");
        File.WriteAllBytes(corrupt, original);
        using (var handler = new DistributionFixtureHandler((_, _) => Task.FromResult(transfer_response(bytes))))
        using (var http = new HttpClient(handler))
        {
            await rejected(() => new DistributionClient(http).download_async(k_transferUrl, corrupt, bytes.Length, new string('0', 64), null, CancellationToken.None), "completed download hash mismatch");
            check(handler.Calls == 1 && !File.Exists(corrupt + ".partial") && File.ReadAllBytes(corrupt).SequenceEqual(original), "bad hash discards partial and preserves original cache");
        }
        var fullPartial = Path.Combine(root, "complete-partial.zip");
        File.WriteAllBytes(fullPartial + ".partial", bytes);
        using (var handler = new DistributionFixtureHandler((_, _) => throw new InvalidOperationException("HTTP not expected")))
        using (var http = new HttpClient(handler))
        {
            await new DistributionClient(http).download_async(k_transferUrl, fullPartial, bytes.Length, hash, null, CancellationToken.None);
            check(handler.Calls == 0 && File.ReadAllBytes(fullPartial).SequenceEqual(bytes), "complete partial can be verified without network");
        }
        await test_distribution_failures(root, bytes);
        await test_package_parts_stream(root);
    }

    private static async Task test_distribution_failures(string a_root, byte[] a_bytes)
    {
        foreach (var status in new[] { HttpStatusCode.Forbidden, HttpStatusCode.TooManyRequests, HttpStatusCode.ServiceUnavailable })
        {
            using var handler = new DistributionFixtureHandler((_, _) =>
            {
                var response = new HttpResponseMessage(status);
                if (status == HttpStatusCode.TooManyRequests) response.Headers.RetryAfter = new RetryConditionHeaderValue(TimeSpan.FromHours(1));
                return Task.FromResult(response);
            });
            using var http = new HttpClient(handler);
            try
            {
                await new DistributionClient(http).get_bytes_async(DistributionClient.CatalogUrl, 1024, CancellationToken.None);
                throw new InvalidOperationException("Expected HTTP rejection");
            }
            catch (HttpRequestException error)
            {
                check(error.StatusCode == status, "HTTP failure retains status");
                check(handler.Calls == (status == HttpStatusCode.ServiceUnavailable ? 3 : 1), "transient retry bounded and 403 / 429 never repeated");
                if (status == HttpStatusCode.TooManyRequests) check(error.Message.Contains("待って", StringComparison.Ordinal), "long Retry-After returns waiting guidance");
            }
        }
        using (var handler = new DistributionFixtureHandler((_, _) => throw new HttpRequestException("https://release-assets.githubusercontent.com/a?secret=secret", null, HttpStatusCode.Forbidden)))
        using (var http = new HttpClient(handler))
        {
            try { await new DistributionClient(http).get_bytes_async(DistributionClient.CatalogUrl, 1024, CancellationToken.None); }
            catch (HttpRequestException error) { check(!error.ToString().Contains("secret", StringComparison.Ordinal), "redirect secret is excluded from diagnostics"); }
        }
        var lockedPath = Path.Combine(a_root, "locked.zip");
        using (var owner = new FileStream(lockedPath + ".lock", FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
        using (var handler = new DistributionFixtureHandler((_, _) => throw new InvalidOperationException("HTTP not expected")))
        using (var http = new HttpClient(handler))
        {
            await rejected(() => new DistributionClient(http).download_async(k_transferUrl, lockedPath, a_bytes.Length, transfer_hash(a_bytes), null, CancellationToken.None), "other process cache ownership is respected");
            check(handler.Calls == 0, "cache lock excludes concurrent download");
        }
    }

    private static async Task test_package_parts_stream(string a_root)
    {
        byte[] archive;
        using (var memory = new MemoryStream())
        {
            using (var zip = new ZipArchive(memory, ZipArchiveMode.Create, true))
            {
                for (var index = 0; index < 8; ++index)
                {
                    using var writer = new StreamWriter(zip.CreateEntry($"folder/entry-{index}.txt").Open(), Encoding.UTF8);
                    writer.Write("fixture entry " + index);
                }
            }
            archive = memory.ToArray();
        }
        var files = new List<string>();
        var parts = new List<ReleasePackagePart>();
        // 小さい Part に分け、ZIP の中央 Directory 自体も複数の File 境界を跨がせる
        for (var offset = 0; offset < archive.Length; offset += 37)
        {
            var file = Path.Combine(a_root, $"archive.zip.{files.Count + 1:D3}");
            var bytes = archive[offset..Math.Min(offset + 37, archive.Length)];
            File.WriteAllBytes(file, bytes);
            files.Add(file);
            parts.Add(new ReleasePackagePart { FileName = Path.GetFileName(file), Size = bytes.Length, Sha256 = transfer_hash(bytes) });
        }
        using (var stream = new PackagePartsStream(files))
        {
            await stream.verify_async(parts, transfer_hash(archive), CancellationToken.None);
            check(stream.Position == 0 && stream.Length == archive.Length, "part hashes and concatenated hash verified");
            var buffer = new byte[93];
            stream.Seek(31, SeekOrigin.Begin);
            check(stream.Read(buffer, 0, buffer.Length) == buffer.Length && buffer.SequenceEqual(archive[31..124]), "synchronous read crosses several part boundaries");
            stream.Seek(37, SeekOrigin.Begin);
            check(await stream.ReadAsync(buffer) == buffer.Length && buffer.SequenceEqual(archive[37..130]), "async read beginning at exact boundary");
            stream.Seek(-12, SeekOrigin.End);
            check(stream.Read(buffer) == 12 && buffer[..12].SequenceEqual(archive[^12..]), "end-relative read crosses boundary and clips at EOF");
            check(stream.Seek(5, SeekOrigin.Current) == archive.Length + 5 && stream.ReadByte() == -1, "seek beyond EOF follows Stream contract");
            await rejected(() => { stream.Seek(-1, SeekOrigin.Begin); return Task.CompletedTask; }, "negative seek rejected");
            await rejected(() => { stream.Seek(long.MaxValue, SeekOrigin.Current); return Task.CompletedTask; }, "overflow seek rejected");
            using (var zip = new ZipArchive(stream, ZipArchiveMode.Read, true))
            {
                check(zip.Entries.Count == 8, "ZIP central directory can span parts");
                using var reader = new StreamReader(zip.GetEntry("folder/entry-6.txt")!.Open(), Encoding.UTF8);
                check(await reader.ReadToEndAsync() == "fixture entry 6", "ZIP payload reads through combined stream");
            }
            await rejected(() => { using var writer = new FileStream(files[0], FileMode.Open, FileAccess.Write, FileShare.ReadWrite); return Task.CompletedTask; }, "open parts exclude writes during extraction");
            await rejected(() => { File.Delete(files[0]); return Task.CompletedTask; }, "open parts exclude delete during extraction");
            await rejected(() => stream.verify_async(parts, new string('0', 64), CancellationToken.None), "combined archive hash mismatch");
            check(stream.Position == 0, "failed verification resets stream position");
            await rejected(() => stream.verify_async(parts.Select((part, index) => index == 0 ? part with { Size = part.Size + 1 } : part).ToArray(), transfer_hash(archive), CancellationToken.None), "signed part size mismatch");
            using var cancel = new CancellationTokenSource();
            cancel.Cancel();
            await rejected(() => stream.verify_async(parts, transfer_hash(archive), cancel.Token), "part verification cancellation");
        }
        File.WriteAllBytes(files[0], Enumerable.Repeat((byte)42, (int)parts[0].Size).ToArray());
        await using (var changed = new PackagePartsStream(files))
            await rejected(() => changed.verify_async(parts, transfer_hash(archive), CancellationToken.None), "modified part rejected");

        var singlePath = Path.Combine(a_root, "renamed-v1-package.zip");
        File.WriteAllBytes(singlePath, archive);
        using (var single = new PackagePartsStream([singlePath]))
        {
            await single.verify_async([new ReleasePackagePart { FileName = "original-v1-name.zip", Size = archive.Length, Sha256 = transfer_hash(archive) }], transfer_hash(archive), CancellationToken.None);
            check(single.Position == 0, "single-file v1 archive keeps renamed package compatibility");
        }
        await rejected(() => { using var tooMany = new PackagePartsStream(Enumerable.Repeat(singlePath, 513).ToArray()); return Task.CompletedTask; }, "part count is bounded");
    }

    private static string transfer_hash(byte[] a_bytes) => Convert.ToHexString(SHA256.HashData(a_bytes));

    private static HttpResponseMessage transfer_response(byte[] a_bytes, long? a_start = null, long? a_total = null)
    {
        var response = new HttpResponseMessage(a_start.HasValue ? HttpStatusCode.PartialContent : HttpStatusCode.OK)
        {
            Content = new ByteArrayContent(a_bytes)
        };
        if (a_start.HasValue)
            response.Content.Headers.ContentRange = new ContentRangeHeaderValue(a_start.Value, a_total!.Value - 1, a_total.Value);
        return response;
    }

    private sealed class DistributionFixtureHandler(Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> a_response) : HttpMessageHandler
    {
        public int Calls { get; private set; }
        public bool HasUserAgent { get; private set; }
        public List<string> Urls { get; } = [];

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage a_request, CancellationToken a_cancel)
        {
            a_cancel.ThrowIfCancellationRequested();
            ++Calls;
            HasUserAgent |= a_request.Headers.UserAgent.Any();
            Urls.Add(a_request.RequestUri!.AbsoluteUri);
            return a_response(a_request, a_cancel);
        }
    }

    /// <summary>Content-Length のない応答、途中 EOF、切断、取消を実 Network に依存せず再現する</summary>
    private sealed class DistributionProgress : IProgress<InstallProgress>
    {
        public List<InstallProgress> Values { get; } = [];
        public void Report(InstallProgress a_progress) => Values.Add(a_progress);
    }

    /// <summary>Cache が参照先のない Link を通常 File として作り直さないことを確認する</summary>
    private static async Task test_distribution_path_links(string a_root)
    {
        var link = Path.Combine(a_root, "dangling-cache-link");
        foreach (var directory in new[] { false, true })
        {
            FileSystemInfo created;
            try
            {
                var target = Path.Combine(a_root, "missing-link-target");
                created = directory ? Directory.CreateSymbolicLink(link, target) : File.CreateSymbolicLink(link, target);
            }
            catch (UnauthorizedAccessException)
            {
                Console.WriteLine("SKIP: symbolic link creation requires Windows developer mode or privilege");
                return;
            }
            try
            {
                await rejected(() => { SafePaths.no_links(link); return Task.CompletedTask; }, "dangling cache link rejected");
                var target = Path.Combine(a_root, "missing-link-target");
                check(!File.Exists(target) && !Directory.Exists(target), "dangling link target remains absent");
            }
            finally { created.Delete(); }
        }
    }

    private sealed class DistributionReadStream(byte[] a_bytes, Action? a_atEnd = null, int a_chunkSize = int.MaxValue) : Stream
    {
        private int m_position;
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => m_position; set => throw new NotSupportedException(); }
        public override int Read(byte[] a_buffer, int a_offset, int a_count)
        {
            if (m_position == a_bytes.Length) { a_atEnd?.Invoke(); return 0; }
            var count = Math.Min(a_chunkSize, Math.Min(a_count, a_bytes.Length - m_position));
            a_bytes.AsSpan(m_position, count).CopyTo(a_buffer.AsSpan(a_offset, count));
            m_position += count;
            return count;
        }
        public override ValueTask<int> ReadAsync(Memory<byte> a_buffer, CancellationToken a_cancel = default)
        {
            a_cancel.ThrowIfCancellationRequested();
            if (m_position == a_bytes.Length) { a_atEnd?.Invoke(); a_cancel.ThrowIfCancellationRequested(); return ValueTask.FromResult(0); }
            var count = Math.Min(a_chunkSize, Math.Min(a_buffer.Length, a_bytes.Length - m_position));
            a_bytes.AsMemory(m_position, count).CopyTo(a_buffer);
            m_position += count;
            return ValueTask.FromResult(count);
        }
        public override void Flush() => throw new NotSupportedException();
        public override long Seek(long a_offset, SeekOrigin a_origin) => throw new NotSupportedException();
        public override void SetLength(long a_value) => throw new NotSupportedException();
        public override void Write(byte[] a_buffer, int a_offset, int a_count) => throw new NotSupportedException();
    }
}

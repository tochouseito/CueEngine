using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Cue.Hub;

/// <summary>Manifest の元 Byte 列を固定した発行者鍵で検証し、JSON 自己申告の鍵を信用しない</summary>
public sealed class PackageVerifier
{
    private readonly string m_publicKey;
    public PackageVerifier(string a_publicKeyPem)
    {
        if (string.IsNullOrWhiteSpace(a_publicKeyPem) || a_publicKeyPem.Contains("PRIVATE KEY", StringComparison.Ordinal))
            throw new InvalidDataException("発行者の公開鍵が未設定です");
        using var key = ECDsa.Create();
        key.ImportFromPem(a_publicKeyPem);
        if (key.KeySize != 256 || key.ExportParameters(false).Curve.Oid.Value != "1.2.840.10045.3.1.7") throw new InvalidDataException("P-256 公開鍵が必要です");
        m_publicKey = key.ExportSubjectPublicKeyInfoPem();
    }

    public ReleasePackageManifest verify(ReadOnlySpan<byte> a_bytes, string a_signature)
    {
        verify_signature(a_bytes, a_signature);
        using var json = JsonDocument.Parse(a_bytes.ToArray());
        if (json.RootElement.ValueKind != JsonValueKind.Object || json.RootElement.EnumerateObject().Select(item => item.Name).Distinct(StringComparer.OrdinalIgnoreCase).Count() != json.RootElement.EnumerateObject().Count())
            throw new InvalidDataException("Manifest 項目が重複しています");
        var result = JsonSerializer.Deserialize<ReleasePackageManifest>(a_bytes, HubStateStore.JsonOptions) ?? throw new InvalidDataException("Manifest が空です");
        if (new[] { result.Product, result.Version, result.Tag, result.Platform, result.Architecture, result.Configuration, result.BuildId, result.PackageFileName, result.PackageSha256 }.Any(string.IsNullOrWhiteSpace))
            throw new InvalidDataException("Manifest の必須文字列がありません");
        if (result.SchemaVersion != 1 || result.Product != "engine" || !SafePaths.is_version(result.Version) || result.Tag != "engine-v" + result.Version ||
            result.Platform != "Windows" || result.Architecture != "x64" || result.Configuration != "Development" ||
            string.IsNullOrWhiteSpace(result.BuildId) || result.BuildId.Length > 256 || result.BuildId.Any(char.IsControl) ||
            result.PackageFileName != $"CueEngine-{result.Version}-windows-x64.zip" ||
            result.PackageSha256.Length != 64 || result.PackageSha256.Any(c => !char.IsAsciiHexDigit(c)) ||
            result.PackageSize is <= 0 or > 8L * 1024 * 1024 * 1024 || result.ExpandedSize is <= 0 or > 24L * 1024 * 1024 * 1024)
            throw new InvalidDataException("未対応または不正な配布 Manifest です");
        return result;
    }

    public void verify_signature(ReadOnlySpan<byte> a_bytes, string a_signature)
    {
        if (a_bytes.Length is 0 or > 65536 || a_signature.Length > 4096) throw new InvalidDataException("Manifest の上限を超えました");
        var signature = Convert.FromBase64String(a_signature.Trim());
        using var key = ECDsa.Create();
        key.ImportFromPem(m_publicKey);
        if (signature.Length != 64 || !key.VerifyData(a_bytes, signature, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation))
            throw new CryptographicException("配布 Manifest の署名を確認できません");
    }
}

/// <summary>配布元 / 展開処理と独立して、Hub が作った導入先だけを識別する</summary>
internal sealed record InstallReceipt(string ReceiptId, string EngineId, string Version, string BuildId);

/// <summary>一つの所有 Staging を検証して公開する。Engine の既存版と Project を上書きしない</summary>
public sealed class EngineInstallService(HubStateStore a_store, HttpClient a_http, PackageVerifier a_verifier, string? a_leaseDirectory = null)
{
    private readonly HubStateStore m_store = a_store;
    private readonly HttpClient m_http = a_http;
    private readonly PackageVerifier m_verifier = a_verifier;
    private readonly string? m_leaseDirectory = a_leaseDirectory;
    private readonly SemaphoreSlim m_operation = new(1, 1);
    private const string k_receiptFile = ".cue-install-receipt.json";

    public async Task<EngineRegistration> install_release_async(PublishedRelease a_release, IProgress<InstallProgress>? a_progress, CancellationToken a_cancel, EngineRegistration? a_repair = null)
    {
        foreach (var pair in a_release.Assets)
            if (!EngineCatalogService.is_asset_url(pair.Value, a_release.Tag)) throw new InvalidDataException("配布 URL が不正です");
        var manifestBytes = await download_small_async(a_release.Assets["engine-release.json"], 65536, a_cancel);
        var signature = Encoding.ASCII.GetString(await download_small_async(a_release.Assets["engine-release.json.sig"], 4096, a_cancel));
        var manifest = m_verifier.verify(manifestBytes, signature);
        if (a_release.Tag != manifest.Tag || a_release.Version != manifest.Version || !a_release.Assets.TryGetValue(manifest.PackageFileName, out var url))
            throw new InvalidDataException("Release と Manifest が一致しません");
        var basePath = prepare_base();
        var downloadRoot = Path.Combine(basePath, ".download-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(downloadRoot);
        try
        {
            var archive = Path.Combine(downloadRoot, manifest.PackageFileName);
            await download_package_async(url, archive, manifest.PackageSize, a_progress, a_cancel);
            return await install_local_async(archive, manifestBytes, signature, a_progress, a_cancel, a_repair);
        }
        finally { SafePaths.delete_owned_tree(downloadRoot, basePath); }
    }

    public async Task<EngineRegistration> install_local_async(string a_archive, byte[] a_manifest, string a_signature, IProgress<InstallProgress>? a_progress, CancellationToken a_cancel, EngineRegistration? a_repair = null)
    {
        var basePath = prepare_base();
        if (!await m_operation.WaitAsync(0, a_cancel)) throw new InvalidOperationException("Engine 操作が進行中です");
        string? staging = null;
        string? backup = null;
        string? target = null;
        FileStream? lease = null;
        try
        {
            var manifest = m_verifier.verify(a_manifest, a_signature);
            target = Path.Combine(basePath, "engine-" + manifest.Version + "-windows-x64");
            if (a_repair != null)
            {
                validate_managed(a_repair, basePath);
                if (!SafePaths.same(target, a_repair.Root) || manifest.Version != a_repair.Version || manifest.BuildId != a_repair.BuildId)
                    throw new InvalidDataException("修復対象の Version が違います");
                lease = EngineLease.acquire(a_repair.LeaseId, true, m_leaseDirectory);
            }
            else if (Directory.Exists(target) || File.Exists(target)) throw new IOException("この Version は既に導入されています");
            SafePaths.no_links(target);
            using (var archive = new FileStream(SafePaths.regular_file(a_archive), FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                if (archive.Length != manifest.PackageSize) throw new InvalidDataException("パッケージの容量が一致しません");
                a_progress?.Report(new("パッケージを検証しています"));
                var digest = Convert.ToHexString(await SHA256.HashDataAsync(archive, a_cancel));
                if (!digest.Equals(manifest.PackageSha256, StringComparison.OrdinalIgnoreCase)) throw new CryptographicException("パッケージの Hash が一致しません");
                archive.Position = 0;
                staging = Path.Combine(basePath, ".staging-" + Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(staging);
                await extract_async(archive, staging, manifest.ExpandedSize, a_progress, a_cancel);
            }
            var engine = EngineDescriptor.load(staging);
            if (engine.Version != manifest.Version || engine.BuildId != manifest.BuildId)
                throw new InvalidDataException("パッケージ内の Engine 識別情報が違います");
            a_cancel.ThrowIfCancellationRequested();
            lease ??= EngineLease.acquire(engine.LeaseId, true, m_leaseDirectory);
            var receiptId = Guid.NewGuid().ToString("D");
            var id = a_repair?.Id ?? Guid.NewGuid().ToString("D");
            File.WriteAllBytes(Path.Combine(staging, k_receiptFile), JsonSerializer.SerializeToUtf8Bytes(new InstallReceipt(receiptId, id, engine.Version, engine.BuildId), HubStateStore.JsonOptions));
            // 既存版の修復は別配置を完成させてから切り替え、保存失敗時は旧版へ戻す
            if (a_repair != null)
            {
                backup = Path.Combine(basePath, ".backup-" + Guid.NewGuid().ToString("N"));
                Directory.Move(target, backup);
            }
            Directory.Move(staging, target);
            staging = null;
            EngineRegistration installed;
            try
            {
                installed = EngineDescriptor.load(target) with { Id = id, IsManaged = true, ReceiptId = receiptId };
                m_store.update(state =>
                {
                    if (a_repair == null && state.Engines.Any(item => SafePaths.same(item.Root, target))) throw new IOException("Engine が同時に登録されました");
                    state.Engines.RemoveAll(item => item.Id == id);
                    state.Engines.Add(installed);
                });
            }
            catch
            {
                staging = Path.Combine(basePath, ".staging-" + Guid.NewGuid().ToString("N"));
                Directory.Move(target, staging);
                if (backup != null) { Directory.Move(backup, target); backup = null; }
                throw;
            }
            if (backup != null)
            {
                try { SafePaths.delete_owned_tree(backup, basePath); backup = null; }
                catch (IOException error) { a_progress?.Report(new("導入成功。旧修復データの回収は保留: " + error.Message)); }
            }
            a_progress?.Report(new("Engine の導入が完了しました", 100));
            return installed;
        }
        catch
        {
            if (backup != null && target != null && !Directory.Exists(target)) { Directory.Move(backup, target); backup = null; }
            throw;
        }
        finally
        {
            lease?.Dispose();
            try { if (staging != null) SafePaths.delete_owned_tree(staging, basePath); }
            finally { m_operation.Release(); }
        }
    }

    public void uninstall(EngineRegistration a_engine)
    {
        if (!m_operation.Wait(0)) throw new InvalidOperationException("Engine 操作が進行中です");
        try
        {
            var basePath = prepare_base();
            validate_managed(a_engine, basePath);
            using var held = EngineLease.acquire(a_engine.LeaseId, true, m_leaseDirectory);
            var quarantine = Path.Combine(basePath, ".removed-" + Guid.NewGuid().ToString("N"));
            Directory.Move(a_engine.Root, quarantine);
            try { m_store.update(state => state.Engines.RemoveAll(item => item.Id == a_engine.Id)); }
            catch { Directory.Move(quarantine, a_engine.Root); throw; }
            SafePaths.delete_owned_tree(quarantine, basePath);
        }
        finally { m_operation.Release(); }
    }

    private static async Task extract_async(Stream a_archive, string a_staging, long a_expectedSize, IProgress<InstallProgress>? a_progress, CancellationToken a_cancel)
    {
        using var zip = new ZipArchive(a_archive, ZipArchiveMode.Read, true);
        if (zip.Entries.Count is 0 or > 100000) throw new InvalidDataException("Zip の要素数が不正です");
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        long total = 0;
        foreach (var entry in zip.Entries)
        {
            a_cancel.ThrowIfCancellationRequested();
            var relative = entry.FullName;
            SafePaths.validate_relative(relative);
            if (!names.Add(relative.TrimEnd('/')) || relative.TrimEnd('/').Equals(k_receiptFile, StringComparison.OrdinalIgnoreCase) ||
                ((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000 || (entry.ExternalAttributes & (int)FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("Zip に重複・Link・予約項目があります");
            var destination = SafePaths.child(a_staging, relative);
            SafePaths.no_links(destination);
            if (relative.EndsWith('/')) { if (entry.Length != 0) throw new InvalidDataException("Directory の内容が不正です"); Directory.CreateDirectory(destination); continue; }
            total = checked(total + entry.Length);
            if (total > a_expectedSize) throw new InvalidDataException("展開容量の上限を超えました");
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            await using var source = entry.Open();
            await using var file = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None, 65536, true);
            var buffer = new byte[65536];
            long copied = 0;
            for (int count; (count = await source.ReadAsync(buffer, a_cancel)) != 0;)
            {
                copied += count;
                if (copied > entry.Length) throw new InvalidDataException("Zip 要素の容量が不正です");
                await file.WriteAsync(buffer.AsMemory(0, count), a_cancel);
            }
            if (copied != entry.Length) throw new InvalidDataException("Zip 要素が途中で切れています");
            a_progress?.Report(new("Engine を展開しています", total * 100.0 / a_expectedSize));
        }
        if (total != a_expectedSize) throw new InvalidDataException("展開後の容量が一致しません");
    }

    private string prepare_base()
    {
        var root = SafePaths.full(m_store.load().EngineDirectory);
        SafePaths.no_links(root);
        Directory.CreateDirectory(root);
        return root;
    }

    private static void validate_managed(EngineRegistration a_engine, string a_base)
    {
        if (!a_engine.IsManaged || a_engine.ReceiptId.Length == 0 || !SafePaths.same(Directory.GetParent(SafePaths.full(a_engine.Root))!.FullName, a_base) ||
            Path.GetFileName(a_engine.Root) != $"engine-{a_engine.Version}-windows-x64") throw new InvalidOperationException("Hub が導入した Engine だけを変更できます");
        SafePaths.no_links(a_engine.Root);
        var receiptFile = SafePaths.regular_file(Path.Combine(a_engine.Root, k_receiptFile));
        if (new FileInfo(receiptFile).Length > 4096) throw new InvalidDataException("導入記録が不正です");
        var receipt = JsonSerializer.Deserialize<InstallReceipt>(File.ReadAllBytes(receiptFile), HubStateStore.JsonOptions);
        if (receipt == null || receipt.ReceiptId != a_engine.ReceiptId || receipt.EngineId != a_engine.Id || receipt.Version != a_engine.Version || receipt.BuildId != a_engine.BuildId)
            throw new InvalidDataException("導入記録が登録内容と一致しません");
        // 破損版も修復できるよう、Descriptor 再読込は要求しない。Lease ID は導入契約から再計算する
        if (a_engine.LeaseId != $"engine-{a_engine.Version}-windows-x64") throw new InvalidDataException("Lease ID が不正です");
    }

    private async Task<byte[]> download_small_async(string a_url, int a_limit, CancellationToken a_cancel)
    {
        using var response = await m_http.GetAsync(a_url, HttpCompletionOption.ResponseHeadersRead, a_cancel);
        response.EnsureSuccessStatusCode();
        return await EngineCatalogService.read_bounded_async(response.Content, a_limit, a_cancel);
    }

    private async Task download_package_async(string a_url, string a_file, long a_size, IProgress<InstallProgress>? a_progress, CancellationToken a_cancel)
    {
        using var response = await m_http.GetAsync(a_url, HttpCompletionOption.ResponseHeadersRead, a_cancel);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength is long length && length != a_size) throw new InvalidDataException("取得する容量が一致しません");
        await using var input = await response.Content.ReadAsStreamAsync(a_cancel);
        await using var output = new FileStream(a_file, FileMode.CreateNew, FileAccess.Write, FileShare.None, 65536, true);
        var buffer = new byte[65536];
        long received = 0;
        for (int count; (count = await input.ReadAsync(buffer, a_cancel)) != 0;)
        {
            received = checked(received + count);
            if (received > a_size) throw new InvalidDataException("ダウンロード容量の上限を超えました");
            await output.WriteAsync(buffer.AsMemory(0, count), a_cancel);
            a_progress?.Report(new("Engine を取得しています", received * 100.0 / a_size));
        }
        if (received != a_size) throw new InvalidDataException("ダウンロードが途中で切れています");
    }
}

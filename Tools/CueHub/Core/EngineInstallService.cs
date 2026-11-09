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
        var root = json.RootElement;
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("schemaVersion", out var schema) ||
            schema.ValueKind != JsonValueKind.Number || !schema.TryGetInt32(out var version) || version is not (1 or 2))
            throw new InvalidDataException("未対応の配布 Manifest です");
        string[] fields = ["schemaVersion", "product", "version", "tag", "platform", "architecture", "configuration",
            "buildId", "packageFileName", "packageSha256", "packageSize", "expandedSize"];
        require_fields(root, version == 2 ? [.. fields, "packageParts"] : fields);
        require_size(root, "packageSize", version == 1 ? DistributionLimits.PartBytes : DistributionLimits.PackageBytes);
        require_size(root, "expandedSize", DistributionLimits.ExpandedBytes);
        if (version == 2)
        {
            var parts = root.GetProperty("packageParts");
            if (parts.ValueKind != JsonValueKind.Array || parts.GetArrayLength() is < 1 or > DistributionLimits.Parts)
                throw new InvalidDataException("Package Part 数が許容範囲外です");
            foreach (var part in parts.EnumerateArray())
            {
                require_fields(part, ["fileName", "sha256", "size"]);
                require_size(part, "size", DistributionLimits.PartBytes);
            }
        }
        var result = JsonSerializer.Deserialize<ReleasePackageManifest>(a_bytes, HubStateStore.JsonOptions) ?? throw new InvalidDataException("Manifest が空です");
        if (new[] { result.Product, result.Version, result.Tag, result.Platform, result.Architecture, result.Configuration, result.BuildId, result.PackageFileName, result.PackageSha256 }.Any(string.IsNullOrWhiteSpace))
            throw new InvalidDataException("Manifest の必須文字列がありません");
        if (result.Product != "engine" || !SafePaths.is_version(result.Version) || result.Tag != "engine-v" + result.Version ||
            result.Platform != "Windows" || result.Architecture != "x64" || result.Configuration != "Development" ||
            result.BuildId.Length > 128 || result.BuildId.Any(c => !char.IsAsciiLetterOrDigit(c) && c is not ('.' or '_' or '+' or '-')) ||
            result.PackageFileName != $"CueEngine-{result.Version}-windows-x64.zip" ||
            !is_hash(result.PackageSha256))
            throw new InvalidDataException("未対応または不正な配布 Manifest です");
        if (version == 2)
        {
            if (result.PackageParts == null) throw new InvalidDataException("Package Part がありません");
            long total = 0;
            for (var index = 0; index < result.PackageParts.Count; ++index)
            {
                var part = result.PackageParts[index];
                var name = result.PackageFileName + "." + (index + 1).ToString("D3", System.Globalization.CultureInfo.InvariantCulture);
                if (part == null || part.FileName != name || !is_hash(part.Sha256))
                    throw new InvalidDataException("Package Part の File 名または Hash が不正です");
                total = checked(total + part.Size);
            }
            if (total != result.PackageSize) throw new InvalidDataException("Package Part の合計容量が一致しません");
        }
        return result;
    }

    public void verify_signature(ReadOnlySpan<byte> a_bytes, string a_signature, int a_limit = DistributionLimits.ManifestBytes)
    {
        if (a_limit <= 0 || a_bytes.Length == 0 || a_bytes.Length > a_limit || a_signature.Length > DistributionLimits.SignatureBytes)
            throw new InvalidDataException("Manifest の上限を超えました");
        var signature = Convert.FromBase64String(a_signature.Trim());
        using var key = ECDsa.Create();
        key.ImportFromPem(m_publicKey);
        if (signature.Length != 64 || !key.VerifyData(a_bytes, signature, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation))
            throw new CryptographicException("配布 Manifest の署名を確認できません");
    }

    /// <summary>未知項目と大文字小文字違いを含む重複項目を、型への変換前に拒否する</summary>
    private static void require_fields(JsonElement a_object, IReadOnlyList<string> a_fields)
    {
        if (a_object.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Manifest 項目は Object が必要です");
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var property in a_object.EnumerateObject())
        {
            if (!a_fields.Contains(property.Name, StringComparer.Ordinal) || !names.Add(property.Name))
                throw new InvalidDataException("Manifest に未知または重複した項目があります");
        }
        if (names.Count != a_fields.Count) throw new InvalidDataException("Manifest の必須項目がありません");
    }

    /// <summary>JSON Number の整数だけを受け入れ、文字列や小数の暗黙変換を防ぐ</summary>
    private static void require_size(JsonElement a_object, string a_name, long a_maximum)
    {
        var value = a_object.GetProperty(a_name);
        if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt64(out var size) || size <= 0 || size > a_maximum)
            throw new InvalidDataException("Manifest の容量が許容範囲外です");
    }

    private static bool is_hash(string? a_hash) => a_hash is { Length: 64 } && a_hash.All(char.IsAsciiHexDigit);
}

/// <summary>配布元 / 展開処理と独立して、Hub が作った導入先だけを識別する</summary>
internal sealed record InstallReceipt(string ReceiptId, string EngineId, string Version, string BuildId);

/// <summary>一つの所有 Staging を検証して公開する。Engine の既存版と Project を上書きしない</summary>
public sealed class EngineInstallService(HubStateStore a_store, HttpClient a_http, PackageVerifier a_verifier, string? a_leaseDirectory = null)
{
    private readonly HubStateStore m_store = a_store;
    private readonly DistributionClient m_distribution = new(a_http);
    private readonly PackageVerifier m_verifier = a_verifier;
    private readonly string? m_leaseDirectory = a_leaseDirectory;
    private readonly SemaphoreSlim m_operation = new(1, 1);
    private const string k_receiptFile = ".cue-install-receipt.json";

    public async Task<EngineRegistration> install_release_async(PublishedRelease a_release, IProgress<InstallProgress>? a_progress, CancellationToken a_cancel, EngineRegistration? a_repair = null)
    {
        foreach (var pair in a_release.Assets)
            if (!DistributionClient.is_asset_url(pair.Value, a_release.Tag, pair.Key)) throw new InvalidDataException("配布 URL が Release と一致しません");
        if (!a_release.Assets.TryGetValue("engine-release.json", out var manifestUrl) ||
            !a_release.Assets.TryGetValue("engine-release.json.sig", out var signatureUrl))
            throw new InvalidDataException("署名付き配布 Manifest がありません");
        var manifestBytes = await m_distribution.get_bytes_async(manifestUrl, DistributionLimits.ManifestBytes, a_cancel);
        var signature = Encoding.ASCII.GetString(await m_distribution.get_bytes_async(signatureUrl, DistributionLimits.SignatureBytes, a_cancel));
        var manifest = m_verifier.verify(manifestBytes, signature);
        if (a_release.Tag != manifest.Tag || a_release.Version != manifest.Version)
            throw new InvalidDataException("Release と Manifest が一致しません");
        var parts = manifest.Parts;
        if (parts.Any(part => !a_release.Assets.ContainsKey(part.FileName)))
            throw new InvalidDataException("署名付き Package Part が Release にありません");
        require_free_space(prepare_base(), manifest.ExpandedSize);
        // 署名済み Version / Hash ごとに保持し、取消や再起動で検証済みの取得結果を失わない
        var downloadRoot = SafePaths.child(m_store.DirectoryPath, "Downloads/engine-" + manifest.Version);
        SafePaths.no_links(downloadRoot);
        Directory.CreateDirectory(downloadRoot);
        var archives = new List<string>(parts.Count);
        long completed = 0;
        foreach (var part in parts)
        {
            a_cancel.ThrowIfCancellationRequested();
            var directory = SafePaths.child(downloadRoot, part.Sha256.ToLowerInvariant());
            SafePaths.no_links(directory);
            Directory.CreateDirectory(directory);
            var archive = SafePaths.child(directory, part.FileName);
            var before = completed;
            var partProgress = a_progress == null ? null : new InlineProgress(progress =>
                a_progress.Report(new(progress.Message, progress.Percent.HasValue ?
                    (before + part.Size * progress.Percent.Value / 100.0) * 100.0 / manifest.PackageSize : null)));
            await m_distribution.download_async(a_release.Assets[part.FileName], archive, part.Size, part.Sha256, partProgress, a_cancel);
            completed += part.Size;
            archives.Add(archive);
        }
        return await install_archives_async(archives, manifestBytes, signature, a_progress, a_cancel, a_repair);
    }

    public Task<EngineRegistration> install_local_async(string a_archive, byte[] a_manifest, string a_signature, IProgress<InstallProgress>? a_progress, CancellationToken a_cancel, EngineRegistration? a_repair = null)
    {
        var manifest = m_verifier.verify(a_manifest, a_signature);
        var selected = SafePaths.regular_file(a_archive);
        IReadOnlyList<string> archives = [selected];
        if (manifest.SchemaVersion == 2)
        {
            if (!manifest.Parts.Any(part => part.FileName == Path.GetFileName(selected)))
                throw new InvalidDataException("選択した File は署名済み Package Part ではありません");
            var directory = Path.GetDirectoryName(selected)!;
            archives = manifest.Parts.Select(part => SafePaths.regular_file(SafePaths.child(directory, part.FileName))).ToArray();
        }
        return install_archives_async(archives, a_manifest, a_signature, a_progress, a_cancel, a_repair);
    }

    private async Task<EngineRegistration> install_archives_async(IReadOnlyList<string> a_archives, byte[] a_manifest, string a_signature, IProgress<InstallProgress>? a_progress, CancellationToken a_cancel, EngineRegistration? a_repair)
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
            require_free_space(basePath, manifest.ExpandedSize);
            using (var archive = new PackagePartsStream(a_archives))
            {
                if (archive.Length != manifest.PackageSize) throw new InvalidDataException("パッケージの容量が一致しません");
                a_progress?.Report(new("パッケージを検証しています"));
                await archive.verify_async(manifest.Parts, manifest.PackageSha256, a_cancel);
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

    /// <summary>既存版を残したまま新しい Payload を展開できる容量を、配置先 Volume で確認する</summary>
    private static void require_free_space(string a_directory, long a_size)
    {
        var volume = Path.GetPathRoot(SafePaths.full(a_directory)) ?? throw new InvalidDataException("配置先 Volume がありません");
        if (new DriveInfo(volume).AvailableFreeSpace < a_size)
            throw new IOException("Engine の展開に必要な空き容量がありません");
    }

    /// <summary>取得 Thread 上で同期的に進捗を中継し、Part 間の通知順を維持する</summary>
    private sealed class InlineProgress(Action<InstallProgress> a_report) : IProgress<InstallProgress>
    {
        public void Report(InstallProgress a_value) => a_report(a_value);
    }
}

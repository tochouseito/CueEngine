using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Cue.Hub;

namespace Cue.Hub.Tests;

internal static partial class Program
{
    /// <summary>小さい実 ZIP を Byte 境界で分け、v2 の署名・厳密 Schema と安全な導入を確認する</summary>
    private static async Task test_package_parts(string a_root)
    {
        using var signing = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var verifier = new PackageVerifier(signing.ExportSubjectPublicKeyInfoPem());
        var root = Path.Combine(a_root, "SplitPackageFixture");
        Directory.CreateDirectory(root);
        var zip = make_package(root, "0.7.0-preview.1");
        var original = make_manifest(zip, "0.7.0-preview.1");
        var data = await File.ReadAllBytesAsync(zip);
        var parts = new List<ReleasePackagePart>();
        for (var offset = 0; offset < data.Length; offset += 127)
        {
            var partData = data.AsMemory(offset, Math.Min(127, data.Length - offset)).ToArray();
            var name = original.PackageFileName + "." + (parts.Count + 1).ToString("D3", System.Globalization.CultureInfo.InvariantCulture);
            await File.WriteAllBytesAsync(Path.Combine(root, name), partData);
            parts.Add(new() { FileName = name, Sha256 = Convert.ToHexString(SHA256.HashData(partData)), Size = partData.Length });
        }
        var manifest = original with { SchemaVersion = 2, PackageParts = parts };
        var bytes = JsonSerializer.SerializeToUtf8Bytes(manifest, HubStateStore.JsonOptions);
        var signature = sign(signing, bytes);
        check(verifier.verify(bytes, signature).Parts.Count == parts.Count && parts.Count > 1, "multipart signed manifest");

        foreach (var error in new[] { "partHash", "partName", "partSizeString", "partSizeFraction", "partSizeLimit", "extraPartField",
            "missingPartField", "partNull", "partOrder", "emptyParts", "nullParts", "extraRootField", "missingRootField", "rootSizeString",
            "rootSizeLimit", "expandedLimit", "sum", "schemaString", "buildId" })
        {
            var node = JsonNode.Parse(bytes)!.AsObject();
            var part = node["packageParts"]![0]!.AsObject();
            switch (error)
            {
                case "partHash": part["sha256"] = "invalid"; break;
                case "partName": part["fileName"] = "../outside.zip.001"; break;
                case "partSizeString": part["size"] = "127"; break;
                case "partSizeFraction": part["size"] = 1.5; break;
                case "partSizeLimit": part["size"] = DistributionLimits.PartBytes + 1; break;
                case "extraPartField": part["extra"] = true; break;
                case "missingPartField": part.Remove("size"); break;
                case "partNull": node["packageParts"]![0] = null; break;
                case "partOrder": part["fileName"] = parts[1].FileName; break;
                case "emptyParts": node["packageParts"] = new JsonArray(); break;
                case "nullParts": node["packageParts"] = null; break;
                case "extraRootField": node["extra"] = true; break;
                case "missingRootField": node.Remove("buildId"); break;
                case "rootSizeString": node["packageSize"] = manifest.PackageSize.ToString(); break;
                case "rootSizeLimit": node["packageSize"] = DistributionLimits.PackageBytes + 1; break;
                case "expandedLimit": node["expandedSize"] = DistributionLimits.ExpandedBytes + 1; break;
                case "sum": node["packageSize"] = manifest.PackageSize + 1; break;
                case "schemaString": node["schemaVersion"] = "2"; break;
                case "buildId": node["buildId"] = "../outside"; break;
            }
            var badBytes = JsonSerializer.SerializeToUtf8Bytes(node, HubStateStore.JsonOptions);
            await rejected(() => Task.FromResult(verifier.verify(badBytes, sign(signing, badBytes))), "strict multipart schema " + error);
        }
        var duplicate = Encoding.UTF8.GetString(bytes).Replace("\"size\": 127", "\"size\": 127, \"SIZE\": 127", StringComparison.Ordinal);
        var duplicateBytes = Encoding.UTF8.GetBytes(duplicate);
        await rejected(() => Task.FromResult(verifier.verify(duplicateBytes, sign(signing, duplicateBytes))), "nested duplicate part field");

        var maximumParts = Enumerable.Range(1, DistributionLimits.Parts).Select(index => new ReleasePackagePart {
            FileName = original.PackageFileName + "." + index.ToString("D3", System.Globalization.CultureInfo.InvariantCulture),
            Sha256 = original.PackageSha256, Size = 1 }).ToList();
        var maximum = manifest with { PackageParts = maximumParts, PackageSize = maximumParts.Count };
        var maximumBytes = JsonSerializer.SerializeToUtf8Bytes(maximum, HubStateStore.JsonOptions);
        check(maximumBytes.Length > 65536 && verifier.verify(maximumBytes, sign(signing, maximumBytes)).Parts.Count == 512, "512-part manifest exceeds old limit safely");
        // 大容量は Byte 列を確保せず契約値で検証し、32 bit への切り詰めを防ぐ
        var largeParts = maximumParts.Take(100).Select(part => part with { Size = 1L << 30 }).ToList();
        var large = manifest with { PackageParts = largeParts, PackageSize = 100L << 30, ExpandedSize = 150L << 30 };
        var largeBytes = JsonSerializer.SerializeToUtf8Bytes(large, HubStateStore.JsonOptions);
        check(verifier.verify(largeBytes, sign(signing, largeBytes)).PackageSize == 100L << 30, "100 GiB metadata preserves 64-bit sizes");
        var oversizedSingle = original with { PackageSize = DistributionLimits.PartBytes + 1 };
        var oversizedBytes = JsonSerializer.SerializeToUtf8Bytes(oversizedSingle, HubStateStore.JsonOptions);
        await rejected(() => Task.FromResult(verifier.verify(oversizedBytes, sign(signing, oversizedBytes))), "v1 release asset hard cap");
        var catalogBytes = new byte[DistributionLimits.ManifestBytes + 1];
        var catalogSignature = sign(signing, catalogBytes);
        await rejected(() => { verifier.verify_signature(catalogBytes, catalogSignature); return Task.CompletedTask; }, "manifest signature default limit");
        verifier.verify_signature(catalogBytes, catalogSignature, DistributionLimits.CatalogBytes);
        check(true, "catalog signature explicit larger limit");

        using var http = new HttpClient(new FixtureHandler([]));
        var store = new HubStateStore(Path.Combine(root, "Settings"));
        store.update(state => state.EngineDirectory = Path.Combine(root, "Engines"));
        var installer = new EngineInstallService(store, http, verifier, Path.Combine(root, "Locks"));
        var selected = Path.Combine(root, parts[^1].FileName);
        var installed = await installer.install_local_async(selected, bytes, signature, null, CancellationToken.None);
        check(installed.IsManaged && File.Exists(installed.Editor) && !File.Exists(Path.Combine(root, manifest.PackageFileName)), "multipart local install without a joined ZIP");
        var changed = Path.Combine(root, parts[0].FileName);
        var saved = await File.ReadAllBytesAsync(changed);
        var corrupted = saved.ToArray();
        corrupted[0] ^= 1;
        await File.WriteAllBytesAsync(changed, corrupted);
        await rejected(() => installer.install_local_async(selected, bytes, signature, null, CancellationToken.None, installed), "multipart hash failure preserves repair target");
        check(File.Exists(installed.Editor), "multipart failed repair retains installed engine");
        await File.WriteAllBytesAsync(changed, saved);
        File.Delete(changed);
        await rejected(() => installer.install_local_async(selected, bytes, signature, null, CancellationToken.None, installed), "missing local signed part");
        await File.WriteAllBytesAsync(changed, saved);
        installer.uninstall(installed);
    }
}

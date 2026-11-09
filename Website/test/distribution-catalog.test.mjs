import assert from 'node:assert/strict';
import {generateKeyPairSync, sign, verify} from 'node:crypto';
import {mkdtemp, readFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';
import {build_distribution_catalog, create_github_source, k_api, k_repository, read_bounded} from '../src/plugins/distribution-catalog.mjs';
import hub_releases from '../src/plugins/hub-releases.mjs';

const {privateKey, publicKey} = generateKeyPairSync('ec', {namedCurve: 'prime256v1'});
const k_privatePem = privateKey.export({format: 'pem', type: 'pkcs8'});
const k_hash = 'a'.repeat(64);
const k_generatedAt = '2026-11-09T00:00:00.000Z';

function fixture(a_product = 'engine', a_schema = 1, a_version = '1.0.0')
{
    const tag = `${a_product}-v${a_version}`;
    const manifest = {
        schemaVersion: a_schema, product: a_product, version: a_version, tag,
        packageFileName: a_product === 'engine' ? `CueEngine-${a_version}-windows-x64.zip` : `CueEngine.Hub-${a_version}-full.nupkg`,
        packageSha256: k_hash, packageSize: 12,
        ...(a_product === 'engine' ? {
            platform: 'Windows', architecture: 'x64', configuration: 'Development', buildId: 'test-build', expandedSize: 24,
        } : {feedFileName: 'releases.win.json', feedSha256: k_hash, feedSize: 6}),
    };
    if (a_schema === 2)
        manifest.packageParts = [1, 2].map(a_index => ({
            fileName: `${manifest.packageFileName}.${String(a_index).padStart(3, '0')}`, sha256: k_hash, size: 6,
        }));
    const release = {id: 1, tag_name: tag, draft: false, prerelease: a_version.includes('-'),
        published_at: '2026-11-08T00:00:00Z', assets: []};
    const payloads = new Map();
    function asset(a_name, a_size, a_digest)
    {
        const id = release.assets.length + 1;
        const value = {id, name: a_name, size: a_size, state: 'uploaded', url: `${k_api}/releases/assets/${id}`,
            browser_download_url: `${k_repository}/releases/download/${encodeURIComponent(tag)}/${encodeURIComponent(a_name)}`,
            digest: a_digest ? `sha256:${a_digest}` : null};
        release.assets.push(value);
        return value;
    }
    function resign()
    {
        const bytes = Buffer.from(JSON.stringify(manifest));
        const signature = Buffer.from(`${sign('sha256', bytes, {key: privateKey, dsaEncoding: 'ieee-p1363'}).toString('base64')}\n`);
        for (const [name, payload] of [[`${a_product}-release.json`, bytes], [`${a_product}-release.json.sig`, signature]])
        {
            let value = release.assets.find(a_value => a_value.name === name);
            if (!value) value = asset(name, payload.length);
            value.size = payload.length;
            payloads.set(name, payload);
        }
    }
    resign();
    if (a_schema === 2) manifest.packageParts.forEach(a_part => asset(a_part.fileName, a_part.size, a_part.sha256));
    else asset(manifest.packageFileName, manifest.packageSize, manifest.packageSha256);
    if (a_product === 'hub')
    {
        asset(manifest.feedFileName, manifest.feedSize, manifest.feedSha256);
        asset('CueEngine.Hub-win-Setup.exe', 32);
    }
    const source = {
        listAssets: async a_release => a_release.assets,
        downloadAsset: async a_asset => payloads.get(a_asset.name),
    };
    return {release, manifest, payloads, source, resign, asset};
}

function build(a_fixture, a_extra = {})
{
    return build_distribution_catalog({releases: [a_fixture.release], source: a_fixture.source,
        signingKeyPem: k_privatePem, generatedAt: k_generatedAt, ...a_extra});
}

test('Engine v1 / v2 と Hub v1 を検証して通常 HTTPS URL だけを署名する', async () => {
    for (const [product, schema] of [['engine', 1], ['engine', 2], ['hub', 1]])
    {
        const value = fixture(product, schema, '1.0.0-preview.1+test');
        const result = await build(value);
        assert.deepEqual(Object.keys(result.catalog), ['schemaVersion', 'generatedAt', 'engines', 'hubs']);
        const release = result.catalog[`${product}s`][0];
        assert.deepEqual(Object.keys(release), ['version', 'tag', 'isPreview', 'publishedUtc', 'pageUrl', 'assets']);
        assert.equal(release.isPreview, true);
        assert.equal(verify('sha256', result.bytes, {key: publicKey, dsaEncoding: 'ieee-p1363'},
            Buffer.from(result.signature.toString().trim(), 'base64')), true);
        assert.equal(result.signature.length, 89);
        assert.equal(result.bytes.toString().includes('PRIVATE KEY'), false);
        for (const [name, url] of Object.entries(release.assets))
            assert.equal(url, `${k_repository}/releases/download/${encodeURIComponent(value.release.tag_name)}/${encodeURIComponent(name)}`);
        assert.equal(result.bytes.toString().includes('api.github.com'), false);
        if (schema === 2)
        {
            assert.equal(Object.hasOwn(release.assets, value.manifest.packageFileName), false);
            assert.ok(Object.hasOwn(release.assets, `${value.manifest.packageFileName}.001`));
        }
    }
});

test('各製品の正式 / Preview 合計を公開日時順に最大10世代へ制限する', async () => {
    const releases = [];
    const fixtures = new Map();
    for (const product of ['engine', 'hub'])
        for (let index = 0; index < 12; ++index)
        {
            const value = fixture(product, 1, `1.0.${index}`);
            value.release.published_at = `2026-11-${String(index + 1).padStart(2, '0')}T00:00:00Z`;
            releases.push(value.release);
            fixtures.set(value.release, value);
        }
    const source = {
        listAssets: async a_release => {
            const value = fixtures.get(a_release);
            value.release.assets.forEach(a_asset => a_asset.owner = value);
            return value.release.assets;
        },
        downloadAsset: async a_asset => a_asset.owner.payloads.get(a_asset.name),
    };
    const result = await build_distribution_catalog({releases, source, signingKeyPem: k_privatePem});
    for (const product of ['engines', 'hubs'])
    {
        assert.equal(result.catalog[product].length, 10);
        assert.equal(result.catalog[product][0].version, '1.0.11');
        assert.equal(result.catalog[product][9].version, '1.0.2');
    }
});

test('Draft と無関係な製品は対象外、公開候補の不正は除外せず失敗する', async () => {
    const value = fixture();
    assert.equal((await build(value, {releases: [{...value.release, draft: true}, {tag_name: 'other-v1.0.0'}]})).catalog.engines.length, 0);
    await assert.rejects(build(value, {releases: [{...value.release, prerelease: true}]}), /公開状態/);
    await assert.rejects(build(value, {releases: [value.release, value.release]}), /重複/);
});

test('署名または Manifest の改ざんを検出する', async () => {
    const value = fixture();
    value.payloads.get('engine-release.json')[0] = 32;
    await assert.rejects(build(value), /署名が不正/);
    value.resign();
    value.payloads.set('engine-release.json.sig', Buffer.alloc(89, 65));
    await assert.rejects(build(value), /署名が不正/);
});

test('Version の最大長と数値 Preview 識別子を Hub と揃える', async () => {
    await build(fixture('engine', 1, `1.0.0-${'a'.repeat(122)}`));
    await assert.rejects(build(fixture('engine', 1, `1.0.0-${'a'.repeat(123)}`)), /公開状態/);
    await assert.rejects(build(fixture('engine', 1, '1.0.0-preview.01')), /公開状態/);
});

test('署名済み Manifest の Tag / Version / schema / 必須文字列を検証する', async () => {
    for (const mutate of [
        a_manifest => a_manifest.tag = 'engine-v2.0.0',
        a_manifest => a_manifest.version = '2.0.0',
        a_manifest => a_manifest.schemaVersion = 3,
        a_manifest => a_manifest.buildId = '',
        a_manifest => a_manifest.buildId = 'not a build id',
        a_manifest => a_manifest.buildId = 'a'.repeat(129),
        a_manifest => a_manifest.buildId = 'ビルド',
        a_manifest => a_manifest.unexpected = true,
    ])
    {
        const value = fixture();
        mutate(value.manifest);
        value.resign();
        await assert.rejects(build(value));
    }
});

test('Engine v1 は 2 GiB 未満の単体 ZIP を要求する', async () => {
    const value = fixture();
    value.manifest.packageSize = 2 * 1024 * 1024 * 1024;
    value.release.assets.find(a_asset => a_asset.name === value.manifest.packageFileName).size = value.manifest.packageSize;
    value.resign();
    await assert.rejects(build(value), /v1 Package が大きすぎ/);
});

test('必須 Asset 欠落、サイズ不一致、GitHub Digest 不一致で失敗する', async () => {
    for (const mutate of [
        a_value => a_value.release.assets.pop(),
        a_value => ++a_value.release.assets.at(-1).size,
        a_value => a_value.release.assets.at(-1).digest = `sha256:${'b'.repeat(64)}`,
    ])
    {
        const value = fixture();
        mutate(value);
        await assert.rejects(build(value), /Asset/);
    }
});

test('Asset の別 Repository、偽装 URL、未完了 state、重複名を拒否する', async () => {
    for (const mutate of [
        a_value => a_value.release.assets[0].url = 'https://api.github.com/repos/other/repo/releases/assets/1',
        a_value => a_value.release.assets[0].browser_download_url += '?redirect=1',
        a_value => a_value.release.assets[0].state = 'open',
        a_value => a_value.release.assets.push(a_value.release.assets[0]),
    ])
    {
        const value = fixture();
        mutate(value);
        await assert.rejects(build(value), /Asset/);
    }
});

test('Engine v2 の欠番、重複、上限サイズ、合計、論理 ZIP の添付を拒否する', async () => {
    for (const mutate of [
        a_value => a_value.manifest.packageParts[0].fileName = `${a_value.manifest.packageFileName}.000`,
        a_value => a_value.manifest.packageParts[1].fileName = a_value.manifest.packageParts[0].fileName,
        a_value => a_value.manifest.packageParts[0].size = 2 * 1024 * 1024 * 1024,
        a_value => ++a_value.manifest.packageSize,
        a_value => a_value.asset(a_value.manifest.packageFileName, a_value.manifest.packageSize),
        a_value => a_value.manifest.packageParts = [],
        a_value => a_value.manifest.packageParts = Array(513).fill(a_value.manifest.packageParts[0]),
    ])
    {
        const value = fixture('engine', 2);
        mutate(value);
        value.resign();
        await assert.rejects(build(value), /分割/);
    }
});

test('Hub の Setup / Feed がない場合は公開 Catalog を生成しない', async () => {
    for (const name of ['CueEngine.Hub-win-Setup.exe', 'releases.win.json'])
    {
        const value = fixture('hub');
        value.release.assets = value.release.assets.filter(a_asset => a_asset.name !== name);
        await assert.rejects(build(value));
    }
});

test('Manifest / 署名の容量上限を超える場合は失敗する', async () => {
    for (const [name, size] of [['engine-release.json', 256 * 1024 + 1], ['engine-release.json.sig', 4097]])
    {
        const value = fixture();
        value.release.assets.find(a_asset => a_asset.name === name).size = size;
        await assert.rejects(build(value), /Manifest または署名/);
    }
});

test('P-256 以外の署名鍵と不正な鍵を拒否し秘密値を診断へ含めない', async () => {
    const value = fixture();
    const other = generateKeyPairSync('ec', {namedCurve: 'secp384r1'}).privateKey.export({format: 'pem', type: 'pkcs8'});
    await assert.rejects(build(value, {signingKeyPem: other}), /P-256/);
    await assert.rejects(build(value, {signingKeyPem: 'private-secret-test'}), a_error => {
        assert.equal(a_error.message.includes('private-secret-test'), false);
        return true;
    });
});

test('応答の実測サイズを上限と比較し Content-Length がなくても制限する', async () => {
    assert.equal((await read_bounded(new Response('123'), 3)).length, 3);
    await assert.rejects(read_bounded(new Response('1234'), 3), /大きすぎ/);
    await assert.rejects(read_bounded(new Response('1', {headers: {'content-length': '100'}}), 3), /大きすぎ/);
    await assert.rejects(read_bounded(new Response('error', {status: 403}), 3), /HTTP 403/);
});

test('API 一覧の pagination と UA / 認証、Asset binary Accept を維持する', async () => {
    const requests = [];
    const source = create_github_source('test-token', async (a_url, a_options) => {
        requests.push({url: a_url, options: a_options});
        return new Response(a_url.includes('/assets/5') ? 'asset' :
            JSON.stringify(a_url.endsWith('page=1') ? Array(100).fill({id: 1}) : []));
    });
    assert.equal((await source.listReleases()).length, 100);
    assert.equal((await source.listAssets({id: 4})).length, 100);
    assert.equal((await source.downloadAsset({url: `${k_api}/releases/assets/5`}, 10)).toString(), 'asset');
    assert.equal(requests.length, 5);
    assert.equal(requests[0].options.headers.Authorization, 'Bearer test-token');
    assert.ok(requests[0].options.headers['User-Agent']);
    assert.equal(requests[4].options.headers.Accept, 'application/octet-stream');
    await assert.rejects(source.downloadAsset({url: 'https://example.com/'}, 10), /所属/);
    assert.throws(() => create_github_source(''), /GH_TOKEN/);
});

test('ローカルでは API を呼ばずページを Build でき、公開必須時は失敗する', async () => {
    const saved = {...process.env};
    const directory = await mkdtemp(join(tmpdir(), 'cue-catalog-test-'));
    const originalFetch = globalThis.fetch;
    try
    {
        delete process.env.GITHUB_ACTIONS;
        delete process.env.CUE_PACKAGE_SIGNING_KEY_PEM;
        delete process.env.CUE_REQUIRE_RELEASE_CATALOG;
        globalThis.fetch = () => { throw new Error('ローカル API 呼び出し'); };
        const plugin = hub_releases();
        assert.equal((await plugin.loadContent()).unavailable, true);
        await plugin.postBuild({outDir: directory});
        await assert.rejects(readFile(join(directory, 'distribution/catalog-v1.json')), /ENOENT/);
        process.env.CUE_REQUIRE_RELEASE_CATALOG = '1';
        await assert.rejects(hub_releases().loadContent(), /公開 CI/);
    }
    finally
    {
        globalThis.fetch = originalFetch;
        for (const key of Object.keys(process.env)) if (!(key in saved)) delete process.env[key];
        Object.assign(process.env, saved);
        await rm(directory, {recursive: true});
    }
});

test('公開 Build は署名済み成果物だけを書き出し globalData へ秘密鍵を渡さない', async () => {
    const saved = {...process.env};
    const directory = await mkdtemp(join(tmpdir(), 'cue-catalog-public-test-'));
    const originalFetch = globalThis.fetch;
    const value = fixture('hub');
    try
    {
        Object.assign(process.env, {GITHUB_ACTIONS: 'true', GH_TOKEN: 'test-token',
            CUE_PACKAGE_SIGNING_KEY_PEM: k_privatePem, CUE_REQUIRE_RELEASE_CATALOG: '1'});
        globalThis.fetch = async (a_url, a_options) => {
            if (a_options.headers.Accept === 'application/octet-stream')
            {
                const asset = value.release.assets.find(a_asset => a_asset.url === a_url);
                return new Response(value.payloads.get(asset.name));
            }
            return new Response(JSON.stringify(a_url.includes('/1/assets?') ? value.release.assets : [value.release]));
        };
        const plugin = hub_releases();
        const content = await plugin.loadContent();
        let globalData;
        await plugin.contentLoaded({content, actions: {setGlobalData: a_value => globalData = a_value}});
        assert.equal(globalData.stable.version, '1.0.0');
        assert.equal(JSON.stringify(globalData).includes('PRIVATE KEY'), false);
        await plugin.postBuild({outDir: directory});
        const bytes = await readFile(join(directory, 'distribution/catalog-v1.json'));
        const signature = await readFile(join(directory, 'distribution/catalog-v1.json.sig'), 'ascii');
        assert.ok(verify('sha256', bytes, {key: publicKey, dsaEncoding: 'ieee-p1363'}, Buffer.from(signature.trim(), 'base64')));
    }
    finally
    {
        globalThis.fetch = originalFetch;
        for (const key of Object.keys(process.env)) if (!(key in saved)) delete process.env[key];
        Object.assign(process.env, saved);
        await rm(directory, {recursive: true});
    }
});

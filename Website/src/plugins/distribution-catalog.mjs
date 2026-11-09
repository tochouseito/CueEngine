import {createPrivateKey, createPublicKey, sign, verify} from 'node:crypto';

export const k_repository = 'https://github.com/tochouseito/CueEngine';
export const k_api = 'https://api.github.com/repos/tochouseito/CueEngine';
export const k_catalogLimit = 4 * 1024 * 1024;
const k_signatureLimit = 4096;
const k_manifestLimit = 256 * 1024;
const k_engineSizeLimit = 1024 * 1024 * 1024 * 1024;
const k_version = /^(engine|hub)-v((0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?)$/;
const k_hash = /^[a-fA-F0-9]{64}$/;

function require_value(a_condition, a_message)
{
    if (!a_condition) throw new Error(a_message);
}

function is_size(a_value)
{
    return Number.isSafeInteger(a_value) && a_value > 0;
}

function require_fields(a_value, a_fields)
{
    require_value(a_value && !Array.isArray(a_value) && typeof a_value === 'object' &&
        Object.keys(a_value).length === a_fields.length && a_fields.every(a_field => Object.hasOwn(a_value, a_field)),
    'Release Manifest の項目が不正です');
}

export async function read_bounded(a_response, a_limit)
{
    require_value(a_response.ok, `GitHub Releases HTTP ${a_response.status}`);
    const length = a_response.headers.get('content-length');
    require_value(length === null || (Number.isSafeInteger(Number(length)) && Number(length) >= 0 && Number(length) <= a_limit),
        'GitHub Releases の応答が大きすぎます');
    const chunks = [];
    let size = 0;
    const reader = a_response.body?.getReader();
    require_value(reader, 'GitHub Releases の応答が空です');
    try
    {
        for (;;)
        {
            const {done, value} = await reader.read();
            if (done) break;
            size += value.byteLength;
            require_value(size <= a_limit, 'GitHub Releases の応答が大きすぎます');
            chunks.push(Buffer.from(value));
        }
        return Buffer.concat(chunks, size);
    }
    finally
    {
        await reader.cancel();
        reader.releaseLock();
    }
}

// API の呼び出しは公開側 CI に限定し、Hub 向け成果物には Token を含めない
export function create_github_source(a_token, a_fetch = fetch)
{
    require_value(typeof a_token === 'string' && a_token.length > 0, '公開 CI の GH_TOKEN がありません');
    const headers = {
        Authorization: `Bearer ${a_token}`,
        'User-Agent': 'CueEngine-distribution-builder',
        'X-GitHub-Api-Version': '2022-11-28',
    };
    async function request(a_url, a_accept, a_limit)
    {
        require_value(a_url.startsWith(`${k_api}/releases`), 'Release API の所属が不正です');
        return read_bounded(await a_fetch(a_url, {
            headers: {...headers, Accept: a_accept}, signal: AbortSignal.timeout(30000),
        }), a_limit);
    }
    async function list(a_path)
    {
        const result = [];
        for (let page = 1; page <= 100; ++page)
        {
            const batch = JSON.parse((await request(`${k_api}${a_path}?per_page=100&page=${page}`,
                'application/vnd.github+json', k_catalogLimit)).toString('utf8'));
            require_value(Array.isArray(batch), 'GitHub Releases の応答が配列ではありません');
            result.push(...batch);
            if (batch.length < 100) return result;
        }
        throw new Error('GitHub Releases の取得上限に達しました');
    }
    return {
        listReleases: () => list('/releases'),
        listAssets: a_release => {
            require_value(is_size(a_release.id), 'Release ID が不正です');
            return list(`/releases/${a_release.id}/assets`);
        },
        downloadAsset: (a_asset, a_limit) => request(a_asset.url, 'application/octet-stream', a_limit),
    };
}

function select_candidates(a_releases, a_product)
{
    const result = [];
    const tags = new Set();
    for (const release of a_releases)
    {
        if (release?.draft === true || typeof release?.tag_name !== 'string' ||
            !release.tag_name.startsWith(`${a_product}-v`)) continue;
        const match = k_version.exec(release.tag_name);
        require_value(match && match[1] === a_product && match[2].length <= 128 &&
            (!match[6] || match[6].slice(1).split('.').every(a_part => !/^\d+$/.test(a_part) || a_part.length === 1 || a_part[0] !== '0')) &&
            release.draft === false &&
            release.prerelease === Boolean(match[6]) &&
            typeof release.published_at === 'string' && /^\d{4}-\d\d-\d\dT.*Z$/.test(release.published_at) &&
            Number.isFinite(Date.parse(release.published_at)), 'Release の Tag または公開状態が不正です');
        require_value(!tags.has(release.tag_name), 'Release の Tag が重複しています');
        tags.add(release.tag_name);
        result.push({release, version: match[2]});
    }
    return result.sort((a_left, a_right) => Date.parse(a_right.release.published_at) -
        Date.parse(a_left.release.published_at) || a_left.release.tag_name.localeCompare(a_right.release.tag_name)).slice(0, 10);
}

function index_assets(a_assets, a_tag)
{
    require_value(Array.isArray(a_assets), 'Release Assets が配列ではありません');
    const assets = new Map();
    const identifiers = new Set();
    for (const asset of a_assets)
    {
        require_value(asset && typeof asset.name === 'string' && /^[A-Za-z0-9._+-]+$/.test(asset.name) &&
            asset.state === 'uploaded' && is_size(asset.size) && is_size(asset.id) &&
            asset.url === `${k_api}/releases/assets/${asset.id}` &&
            asset.browser_download_url === `${k_repository}/releases/download/${encodeURIComponent(a_tag)}/${encodeURIComponent(asset.name)}` &&
            !assets.has(asset.name) && !identifiers.has(asset.id), 'Release Asset の所属、名前、完了状態が不正です');
        identifiers.add(asset.id);
        assets.set(asset.name, asset);
    }
    return assets;
}

function validate_manifest(a_manifest, a_product, a_version, a_assets)
{
    const common = ['schemaVersion', 'product', 'version', 'tag', 'packageFileName', 'packageSha256', 'packageSize'];
    const fields = a_product === 'engine' ? [...common, 'platform', 'architecture', 'configuration', 'buildId', 'expandedSize'] :
        [...common, 'feedFileName', 'feedSha256', 'feedSize'];
    if (a_product === 'engine' && a_manifest.schemaVersion === 2) fields.push('packageParts');
    require_fields(a_manifest, fields);
    require_value(a_manifest.product === a_product && a_manifest.version === a_version &&
        a_manifest.tag === `${a_product}-v${a_version}` && is_size(a_manifest.packageSize) &&
        typeof a_manifest.packageSha256 === 'string' && k_hash.test(a_manifest.packageSha256),
    'Release Manifest と Tag / Version が一致しません');
    const required = [];
    function require_asset(a_name, a_size, a_hash)
    {
        const asset = a_assets.get(a_name);
        require_value(asset && asset.size === a_size &&
            (asset.digest == null || asset.digest.toLowerCase() === `sha256:${a_hash.toLowerCase()}`),
        '署名対象 Asset のサイズまたは SHA256 が一致しません');
        required.push(a_name);
    }
    if (a_product === 'engine')
    {
        require_value([1, 2].includes(a_manifest.schemaVersion) && a_manifest.platform === 'Windows' &&
            a_manifest.architecture === 'x64' && a_manifest.configuration === 'Development' &&
            typeof a_manifest.buildId === 'string' && /^[0-9A-Za-z._+-]{1,128}$/.test(a_manifest.buildId) &&
            is_size(a_manifest.expandedSize) && a_manifest.expandedSize <= k_engineSizeLimit &&
            a_manifest.packageSize <= k_engineSizeLimit &&
            a_manifest.packageFileName === `CueEngine-${a_version}-windows-x64.zip`, 'Engine Manifest が不正です');
        if (a_manifest.schemaVersion === 2)
        {
            require_value(Array.isArray(a_manifest.packageParts) && a_manifest.packageParts.length > 0 &&
                a_manifest.packageParts.length <= 512 && !a_assets.has(a_manifest.packageFileName),
            'Engine 分割 Package が不正です');
            let total = 0;
            for (const [index, part] of a_manifest.packageParts.entries())
            {
                require_fields(part, ['fileName', 'sha256', 'size']);
                require_value(part.fileName === `${a_manifest.packageFileName}.${String(index + 1).padStart(3, '0')}` &&
                    is_size(part.size) && part.size < 2 * 1024 * 1024 * 1024 &&
                    typeof part.sha256 === 'string' && k_hash.test(part.sha256), 'Engine 分割 Package の連番が不正です');
                require_asset(part.fileName, part.size, part.sha256);
                total += part.size;
            }
            require_value(Number.isSafeInteger(total) && total === a_manifest.packageSize, 'Engine 分割 Package の合計サイズが不正です');
        }
        else
        {
            require_value(a_manifest.packageSize < 2 * 1024 * 1024 * 1024, 'Engine v1 Package が大きすぎます');
            require_asset(a_manifest.packageFileName, a_manifest.packageSize, a_manifest.packageSha256);
        }
    }
    else
    {
        require_value(a_manifest.schemaVersion === 1 &&
            a_manifest.packageFileName === `CueEngine.Hub-${a_version}-full.nupkg` &&
            a_manifest.packageSize <= 1024 * 1024 * 1024 && a_manifest.feedFileName === 'releases.win.json' &&
            is_size(a_manifest.feedSize) && a_manifest.feedSize <= 1024 * 1024 &&
            typeof a_manifest.feedSha256 === 'string' && k_hash.test(a_manifest.feedSha256), 'Hub Manifest が不正です');
        require_asset(a_manifest.packageFileName, a_manifest.packageSize, a_manifest.packageSha256);
        require_asset(a_manifest.feedFileName, a_manifest.feedSize, a_manifest.feedSha256);
        require_value(a_assets.has('CueEngine.Hub-win-Setup.exe'), 'Hub Setup がありません');
        required.push('CueEngine.Hub-win-Setup.exe');
    }
    return required;
}

// 秘密鍵はこの関数内だけで扱い、返り値には公開 Catalog の Byte 列と署名だけを含める
export async function build_distribution_catalog({releases, source, signingKeyPem, generatedAt = new Date().toISOString()})
{
    let privateKey;
    try { privateKey = createPrivateKey(signingKeyPem); }
    catch { throw new Error('配布 Catalog の署名鍵を読み込めません'); }
    require_value(privateKey.asymmetricKeyType === 'ec' && privateKey.asymmetricKeyDetails?.namedCurve === 'prime256v1',
        '配布 Catalog の署名鍵は ECDSA P-256 が必要です');
    const publicKey = createPublicKey(privateKey);
    require_value(typeof generatedAt === 'string' && new Date(generatedAt).toISOString() === generatedAt,
        '配布 Catalog の生成時刻が不正です');
    const catalog = {schemaVersion: 1, generatedAt, engines: [], hubs: []};
    for (const product of ['engine', 'hub'])
    {
        for (const {release, version} of select_candidates(releases, product))
        {
            const assets = index_assets(await source.listAssets(release), release.tag_name);
            const manifestName = `${product}-release.json`;
            const signatureName = `${manifestName}.sig`;
            const manifestAsset = assets.get(manifestName);
            const signatureAsset = assets.get(signatureName);
            require_value(manifestAsset && manifestAsset.size <= k_manifestLimit && signatureAsset &&
                signatureAsset.size <= k_signatureLimit, 'Release Manifest または署名がありません');
            const bytes = await source.downloadAsset(manifestAsset, k_manifestLimit);
            const signatureBytes = await source.downloadAsset(signatureAsset, k_signatureLimit);
            require_value(bytes.length === manifestAsset.size && signatureBytes.length === signatureAsset.size,
                'Release Manifest または署名のサイズが一致しません');
            const signature = signatureBytes.toString('ascii').trim();
            require_value(signatureBytes.every(a_value => a_value <= 127) && /^[A-Za-z0-9+/]{86}==$/.test(signature) &&
                verify('sha256', bytes, {key: publicKey, dsaEncoding: 'ieee-p1363'}, Buffer.from(signature, 'base64')),
            'Release Manifest の署名が不正です');
            const manifest = JSON.parse(new TextDecoder('utf-8', {fatal: true}).decode(bytes));
            const required = [manifestName, signatureName, ...validate_manifest(manifest, product, version, assets)];
            catalog[`${product}s`].push({
                version, tag: release.tag_name, isPreview: release.prerelease, publishedUtc: release.published_at,
                pageUrl: `${k_repository}/releases/tag/${encodeURIComponent(release.tag_name)}`,
                assets: Object.fromEntries(required.map(a_name => [a_name, assets.get(a_name).browser_download_url])),
            });
        }
    }
    const bytes = Buffer.from(`${JSON.stringify(catalog, null, 4)}\n`, 'utf8');
    require_value(bytes.length <= k_catalogLimit, '配布 Catalog が大きすぎます');
    const signature = sign('sha256', bytes, {key: privateKey, dsaEncoding: 'ieee-p1363'});
    require_value(verify('sha256', bytes, {key: publicKey, dsaEncoding: 'ieee-p1363'}, signature),
        '配布 Catalog の自己検証に失敗しました');
    return {catalog, bytes, signature: Buffer.from(`${signature.toString('base64')}\n`, 'ascii')};
}

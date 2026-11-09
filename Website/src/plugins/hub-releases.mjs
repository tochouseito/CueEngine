import {mkdir, writeFile} from 'node:fs/promises';
import {join} from 'node:path';
import {build_distribution_catalog, create_github_source, k_repository} from './distribution-catalog.mjs';

const k_version = /^hub-v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$/;

// Engine や更新専用 Asset を除き、実際に添付された Windows x64 の Setup だけを案内する
export function select_hub_releases(a_releases)
{
    const result = {stable: null, preview: null};
    for (const release of a_releases)
    {
        const tag = release?.tag_name;
        const match = typeof tag === 'string' ? k_version.exec(tag) : null;
        if (!match || release.draft !== false || release.prerelease !== Boolean(match[4]) ||
            typeof release.published_at !== 'string' || !Number.isFinite(Date.parse(release.published_at)) ||
            !Array.isArray(release.assets))
        {
            continue;
        }
        const tagPath = encodeURIComponent(tag);
        const setup = release.assets.find(a_asset => a_asset?.state === 'uploaded' && a_asset.size > 0 &&
            typeof a_asset.name === 'string' && /^CueEngine\.Hub-win-Setup\.exe$/.test(a_asset.name) &&
            a_asset.browser_download_url === `${k_repository}/releases/download/${tagPath}/${a_asset.name}`);
        if (!setup)
        {
            continue;
        }
        const channel = release.prerelease ? 'preview' : 'stable';
        if (!result[channel] || Date.parse(release.published_at) > Date.parse(result[channel].publishedAt))
        {
            result[channel] = {
                version: tag.slice('hub-v'.length),
                publishedAt: release.published_at,
                setupUrl: setup.browser_download_url,
                notesUrl: `${k_repository}/releases/tag/${tagPath}`,
            };
        }
    }
    return result;
}

export default function hub_releases()
{
    let distribution = null;
    return {
        name: 'cue-hub-releases',
        async loadContent()
        {
            try
            {
                distribution = null;
                if (process.env.GITHUB_ACTIONS !== 'true' || !process.env.CUE_PACKAGE_SIGNING_KEY_PEM)
                    throw new Error('署名付き配布 Catalog は署名鍵を設定した公開 CI で生成してください');
                const source = create_github_source(process.env.GH_TOKEN);
                distribution = await build_distribution_catalog({
                    releases: await source.listReleases(), source, signingKeyPem: process.env.CUE_PACKAGE_SIGNING_KEY_PEM,
                });
                const releases = distribution.catalog.hubs.map(a_release => ({
                    tag_name: a_release.tag, published_at: a_release.publishedUtc, prerelease: a_release.isPreview,
                    draft: false, assets: [{name: 'CueEngine.Hub-win-Setup.exe', state: 'uploaded', size: 1,
                        browser_download_url: a_release.assets['CueEngine.Hub-win-Setup.exe']}],
                }));
                return {...select_hub_releases(releases), unavailable: false};
            }
            catch (error)
            {
                // 公開ビルドでは一時的な取得失敗で既存のダウンロード案内を消さない
                if (process.env.CUE_REQUIRE_RELEASE_CATALOG === '1')
                {
                    throw error;
                }
                console.warn(`CueHub の配布情報を取得できません: ${error.message}`);
                return {stable: null, preview: null, unavailable: true};
            }
        },
        async contentLoaded({content, actions})
        {
            actions.setGlobalData(content);
        },
        async postBuild({outDir})
        {
            if (!distribution) return;
            const directory = join(outDir, 'distribution');
            await mkdir(directory, {recursive: true});
            await writeFile(join(directory, 'catalog-v1.json'), distribution.bytes);
            await writeFile(join(directory, 'catalog-v1.json.sig'), distribution.signature);
        },
    };
}

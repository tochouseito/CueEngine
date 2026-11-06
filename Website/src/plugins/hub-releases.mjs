const k_repository = 'https://github.com/tochouseito/CueEngine';
const k_releaseApi = 'https://api.github.com/repos/tochouseito/CueEngine/releases';
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
    return {
        name: 'cue-hub-releases',
        async loadContent()
        {
            try
            {
                const releases = [];
                const headers = {Accept: 'application/vnd.github+json', 'User-Agent': 'CueEngine-reference'};
                if (process.env.GH_TOKEN)
                {
                    headers.Authorization = `Bearer ${process.env.GH_TOKEN}`;
                }
                for (let page = 1; page <= 10; ++page)
                {
                    const response = await fetch(`${k_releaseApi}?per_page=100&page=${page}`, {
                        headers, signal: AbortSignal.timeout(10000),
                    });
                    if (!response.ok)
                    {
                        throw new Error(`GitHub Releases HTTP ${response.status}`);
                    }
                    const batch = await response.json();
                    if (!Array.isArray(batch))
                    {
                        throw new Error('GitHub Releases の応答が配列ではありません');
                    }
                    releases.push(...batch);
                    if (batch.length < 100)
                    {
                        return {...select_hub_releases(releases), unavailable: false};
                    }
                }
                throw new Error('GitHub Releases の取得上限に達しました');
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
    };
}

import assert from 'node:assert/strict';
import test from 'node:test';
import {select_hub_releases} from '../src/plugins/hub-releases.mjs';

function make_release(a_tag, a_date)
{
    const name = 'CueEngine.Hub-win-Setup.exe';
    return {
        tag_name: a_tag,
        published_at: a_date,
        prerelease: a_tag.includes('-preview.'),
        draft: false,
        assets: [{name, state: 'uploaded', size: 42,
            browser_download_url: `https://github.com/tochouseito/CueEngine/releases/download/${a_tag}/${name}`}],
    };
}

test('正式版と Preview の最新 Setup を独立して選ぶ', () => {
    const result = select_hub_releases([
        make_release('hub-v1.0.0', '2026-09-01T00:00:00Z'),
        make_release('hub-v2.0.0-preview.1', '2026-10-02T00:00:00Z'),
        make_release('hub-v1.1.0', '2026-10-01T00:00:00Z'),
    ]);
    assert.equal(result.stable.version, '1.1.0');
    assert.equal(result.preview.version, '2.0.0-preview.1');
    assert.equal(result.stable.setupUrl,
        'https://github.com/tochouseito/CueEngine/releases/download/hub-v1.1.0/CueEngine.Hub-win-Setup.exe');
});

test('公開前、異なる製品、Setup 欠落、不正なリンクを表示しない', () => {
    const date = '2026-10-01T00:00:00Z';
    const draft = {...make_release('hub-v1.0.0', date), draft: true};
    const missing = {...make_release('hub-v1.0.1', date), assets: []};
    const invalid = make_release('hub-v1.0.2', date);
    invalid.assets[0].browser_download_url = 'https://example.com/CueEngine.Hub-win-Setup.exe';
    const wrongChannel = {...make_release('hub-v1.0.3-preview.1', date), prerelease: false};
    assert.deepEqual(select_hub_releases([draft, missing, invalid, wrongChannel, make_release('engine-v1.0.0', date)]),
        {stable: null, preview: null});
});

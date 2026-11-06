import React from 'react';
import Link from '@docusaurus/Link';
import {usePluginData} from '@docusaurus/useGlobalData';
import Layout from '@theme/Layout';

function DownloadCard({title, release, emptyMessage})
{
    return (
        <article className="cue-card cue-download-card">
            <h2>{title}</h2>
            {release ? (
                <>
                    <p><strong>{release.version}</strong> · Windows x64</p>
                    <p><a className="button button--primary" href={release.setupUrl}>CueHub Setup をダウンロード</a></p>
                    <Link href={release.notesUrl}>リリースノート</Link>
                </>
            ) : <p>{emptyMessage}</p>}
        </article>
    );
}

export default function Download()
{
    const {stable, preview, unavailable} = usePluginData('cue-hub-releases');
    const unavailableMessage = '現在、配布情報を取得できません。GitHub Releases でご確認ください。';
    return (
        <Layout title="CueHub のダウンロード" description="Windows x64 向け CueHub の公式セットアップ">
            <main className="container cue-download">
                <p className="cue-eyebrow">DOWNLOAD · WINDOWS ×64</p>
                <h1>CueHub を入手</h1>
                <p className="cue-lede">CueHub をセットアップすると、Engine の導入と Project の作成ができます。</p>
                <div className="cue-download-cards">
                    <DownloadCard title="正式版" release={stable}
                        emptyMessage={unavailable ? unavailableMessage : '正式版のインストーラーはまだ公開されていません。'} />
                    <DownloadCard title="Preview" release={preview}
                        emptyMessage={unavailable ? unavailableMessage : 'Preview のインストーラーはまだ公開されていません。'} />
                </div>
                <div className="cue-actions">
                    <Link to="/docs/install">インストール手順へ →</Link>
                    <Link href="https://github.com/tochouseito/CueEngine/releases">GitHub Releases を見る</Link>
                </div>
            </main>
        </Layout>
    );
}

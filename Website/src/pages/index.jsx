import React from 'react';
import Link from '@docusaurus/Link';
import Layout from '@theme/Layout';

export default function Home()
{
    return (
        <Layout title="ホーム" description="CueEngine の導入、使い方、リファレンス">
            <main>
                <section className="cue-hero">
                    <div className="container cue-hero-layout">
                        <div>
                            <p className="cue-eyebrow">WINDOWS ×64 · C++ GAME ENGINE</p>
                            <h1>CueEngine</h1>
                            <p className="cue-lede">
                                Editor でシーンを作り、C++ Script で動かす。
                                まずは、回転する Cube を単独アプリにしてみましょう。
                            </p>
                            <div className="cue-actions">
                                <Link className="button button--primary button--lg" to="/download">CueHub を入手</Link>
                                <Link className="button button--secondary button--outline button--lg" to="/docs/install">
                                    インストール手順
                                </Link>
                            </div>
                        </div>
                        <div className="cue-cube-frame" aria-hidden="true">
                            <svg viewBox="0 0 240 240" className="cue-cube">
                                <path d="M120 30 198 75 120 120 42 75Z" fill="var(--cue-cube-top)" />
                                <path d="M42 75 120 120 120 210 42 165Z" fill="var(--cue-cube-left)" />
                                <path d="M120 120 198 75 198 165 120 210Z" fill="var(--cue-cube-right)" />
                                <path d="M120 30 198 75 198 165 120 210 42 165 42 75Z M42 75 120 120 198 75 M120 120V210"
                                    fill="none" stroke="var(--cue-cube-stroke)" strokeWidth="2" />
                            </svg>
                        </div>
                    </div>
                </section>
                <section className="container cue-start">
                    <h2>はじめる</h2>
                    <div className="cue-cards">
                        <Link className="cue-card" to="/download">
                            <span className="cue-eyebrow">01 · DOWNLOAD</span>
                            <h3>CueHub を入手</h3>
                            <p>Windows x64 用のセットアップと公開バージョン。</p>
                        </Link>
                        <Link className="cue-card" to="/docs/install">
                            <span className="cue-eyebrow">02 · INSTALL</span>
                            <h3>Engine を導入</h3>
                            <p>必要な開発ツールを用意し、Hub から Engine をインストール。</p>
                        </Link>
                        <Link className="cue-card" to="/docs/first-app">
                            <span className="cue-eyebrow">03 · BUILD</span>
                            <h3>最初のアプリを作る</h3>
                            <p>Project を作り、回転する Cube をビルドして起動。</p>
                        </Link>
                    </div>
                </section>
            </main>
        </Layout>
    );
}

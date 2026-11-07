import hubReleases from './src/plugins/hub-releases.mjs';

/** @type {import('@docusaurus/types').Config} */
export default {
    title: 'CueEngine',
    tagline: 'Cubeひとつから、ゲームを作る。',
    url: 'https://tochouseito.github.io',
    baseUrl: '/CueEngine/',
    organizationName: 'tochouseito',
    projectName: 'CueEngine',
    trailingSlash: true,
    onBrokenLinks: 'throw',
    i18n: {defaultLocale: 'ja', locales: ['ja']},
    plugins: [hubReleases],
    presets: [['classic', {
        docs: {sidebarPath: './sidebars.mjs'},
        blog: false,
        theme: {customCss: './src/css/custom.css'},
    }]],
    themeConfig: {
        colorMode: {defaultMode: 'dark', respectPrefersColorScheme: true},
        navbar: {
            title: 'CueEngine',
            items: [
                {to: '/download', label: 'ダウンロード', position: 'left'},
                {type: 'docSidebar', sidebarId: 'guide', label: '導入ガイド', position: 'left'},
                {href: 'https://github.com/tochouseito/CueEngine', label: 'GitHub', position: 'right'},
            ],
        },
        footer: {
            copyright: 'Copyright © 2026 tochouseito',
            links: [{items: [{html: '<a class="footer__link-item" href="/CueEngine/third-party-notices.txt">第三者ライセンス</a>'}]}],
        },
    },
};

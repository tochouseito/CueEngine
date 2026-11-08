# Editor Fonts

旧 CueEngine と同じ未改変の TTF を保持する。出自は `CueEngineLegacy` の Commit
`8476b656c24e4ef6262761fefed9b37566932306`、`Engine/Fonts/`。
各 File の name table にある Version と SHA-256 を以下に記録する。

| File | Version | SHA-256 |
| --- | --- | --- |
| `Inter-VariableFont_opsz,wght.ttf` | Version 4.001;git-66647c0bb | `0be2399ea925f1f83ff974764761da9860ec50742ed29a5d4c1ffd0c5c7ac3a8` |
| `JetBrainsMono-VariableFont_wght.ttf` | Version 2.211 | `192a3158a926768d1fa520164f0fb0f40a6ad085435aa854e43eb14cb40d1aab` |
| `MaterialSymbolsOutlined-VariableFont_FILL,GRAD,opsz,wght.ttf` | Version 2.930 | `61b0abbbc3ffab7c5f364cc942e03c01a502ca0da14e14bf97d43daba48fbba6` |
| `NotoSansJP-VariableFont_wght.ttf` | Version 2.004-H2;hotconv 1.0.118;makeotfexe 2.5.65603 | `04b2ac921347b12c63bc35adda5722dd2b1860d900f668dc5050a44202464fa5` |

- Inter: https://github.com/rsms/inter （SIL OFL 1.1）
- Noto Sans JP: https://github.com/notofonts/noto-cjk （SIL OFL 1.1）
- JetBrains Mono: https://github.com/JetBrains/JetBrainsMono （SIL OFL 1.1）
- Material Symbols Outlined: https://github.com/google/material-design-icons （Apache-2.0）

原文 License と Copyright は `../Licenses/Inter-OFL.txt`、`NotoSansJP-OFL.txt`、
`JetBrainsMono-OFL.txt`、`MaterialSymbols-Apache-2.0.txt`、`MaterialSymbols-Notice.txt` に保持する。
TTF の埋め込み著作権・ライセンス情報は変更しない。

Editor と Test では実行 File 隣の `EngineResources/Fonts/` に Font と License を配置する。
Source-backed SDK にはこの Directory と `ThirdParty/Licenses/` も含め、再 Build に対応する。

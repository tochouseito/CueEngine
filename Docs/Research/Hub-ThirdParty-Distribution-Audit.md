# Hub / Engine・Editor 配布の Third-party 監査

- Date: 2026-11-04
- 対象: Engine・Editor・SDK・Tools のパッケージを GitHub Releases に置き、Windows Hub から取得・展開する計画
- 実施: 現在の固定版、配布 Archive 内の License / Notice、Build 依存を読み取り確認した
- 本記録は個別 File の許諾条件と配布物の照合であり、未確認の依存物まで包括して配布可能と判定しない

## 確認した範囲の結果

下表の対象では、商用・非商用の Binary / Source / SDK 配布を一律に禁止する条件は確認できなかった。
MIT 対象は Copyright と許諾文を保持する必要があり、ソース公開義務を要求する条文はない。
ImGui 本体以外の埋め込み Font・補助実装の Notice を追加した。

| 対象 | 現在の識別情報 | 配布に関係する条件 |
| --- | --- | --- |
| Dear ImGui Core / Demo / Win32・DX12 Backend | 上流 `v1.92.9b-docking`、vcpkg `1.92.9` | MIT の Copyright と許諾文を Binary / Header / Source 配布へ保持する |
| ProggyClean 埋め込み Font | 固定 ImGui Tag の `imgui_draw.cpp`、Copyright (c) 2004, 2005 Tristan Grimmer | MIT 全文と帰属を Editor 配布へ保持する |
| ProggyForever 埋め込み Font | 同 File、Copyright (c) 2026 Disco Hello / Copyright (c) 2019,2023 Tristan Grimmer | MIT 全文と帰属を Editor 配布へ保持する |
| ImGui 同梱 stb 補助実装 | `imstb_rectpack.h` 1.01、`imstb_textedit.h` 1.14、`imstb_truetype.h` 1.26 の ImGui 修正版 | MIT / Public Domain の選択から MIT を使用し、共通 License Block を配布へ保持する |
| ImGui の日本語 Glyph Range データ | 固定 Tag の `ImFontAtlas::GetGlyphRangesJapanese` | 上流が明記する文化庁・法務省の出典と CC BY 4.0 へのリンク、派生データの説明を保持する |
| WinPixEventRuntime | `1.0.240308001`、Archive SHA-256 `726acc93d6968e2146261a1e415521747d50ad69894c2b42b5d0d4c29fd66ec4` | Archive の `.nuspec` が MIT の `license.txt` を指定する。MIT 本文と同梱 Notice を保持する |
| vcpkg | Tool `2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8` | MIT。通常は開発時だけ使用する。Tool Source / Script を配布する場合は Notice を保持する |
| vcpkg-cmake / vcpkg-cmake-config | `2025-08-07` / `2026-07-21` | 各 MIT。Build 補助 Source / Script を配布する場合は各 Copyright・許諾文を保持する |

導入済み vcpkg の `status` にある実 Package は上記 3 種であり、ImGui の Feature は
`docking-experimental` / `win32-binding` / `dx12-binding` である。
この調査では追加の FreeType や ImGui の第三者拡張を配布対象として確認していない。

## Notice の補完

既存の `ThirdParty/Licenses/ImGui-MIT.txt` と vcpkg `share/imgui/copyright` は、Omar Cornut の本体 MIT のみだった。
現在の Editor は `ImGuiManager.cpp` で `AddFontDefault` を呼び、既定 Font の除外定義も設定していない。
Font・補助実装を含む配布物が同じ Notice を失わないよう、次を追加した。

- `ThirdParty/Licenses/ProggyClean-MIT.txt`
- `ThirdParty/Licenses/ProggyForever-MIT.txt`
- `ThirdParty/Licenses/ImGui-STB-Licenses.txt`
- `ThirdParty/Licenses/ImGui-JapaneseGlyphRanges-Notice.txt`

固定 ImGui Tag にある Font の Copyright と MIT 表示を、Font 上流の License 本文と照合した。
ProggyClean の上流 Commit は `139ec08a38096161291792313ef5803fc4f0e37b`、
ProggyForever は `f8868ba43bb42c4d88454d03ad81cbd139789f72`。
stb の 3 Header は末尾の License Block が一致することを確認し、その共通 Block を保持した。
圧縮 Font の展開処理は `stb.h` 由来の Public Domain と上流 Source に明記される。

Source SDK に第三者 Source を含める場合は、これらの追加 Notice だけで置き換えず、元 Source 内の帰属・License コメントも保持する。
`misc/fonts/` にある追加 Font を一括 Copy する運用は採用せず、配布対象に追加する時点で個別の条件を照合する。

## PIX Event Runtime と PIX Tool の区別

`WinPixEventRuntime.nuspec` は `license type="file"` に `license.txt` を指定し、その File は MIT である。
Repository に保持した MIT / Third-party Notice は、Archive 内の File とそれぞれ SHA-256 が一致した。

一方、`WinPixEventRuntime-ThirdPartyNotices.txt` の冒頭は Microsoft PIX Developer Tool に関する説明で、
WiX / MS-RL なども含む広い依存一覧を持つ。この一覧の全 Library が CueEngine にリンクされると判断しない。
また、Event Runtime の MIT を根拠に PIX 本体・Capture Tool の自由な再配布を認める判断もしない。
実際のパッケージ収集は許可対象の Header / Import Library / DLL と対応 Notice に限定する。

## First-party Source の帰属

移植 ECS は同じ所有者の旧 `CueEngineLegacy` にある自作実装で、第三者 ECS Library ではない。
出典 Commit は `8476b656c24e4ef6262761fefed9b37566932306`。
監査開始時点では、元 Root の License と `Engine/Source/ECS/LICENSE.txt` は SHA-256
`19200a6a9407e592065a5a504c0eefe58adf102c9ac5aa2151bd6f257faa7a9c` が一致した。

元の Root `LICENSE.txt` と ECS の License は `Copyright (c) [year] [fullname]` のままだった。
監査者による推定補完は行わず、2026-11-04 のユーザー明示指定を受け、両 File を
`Copyright (c) 2026 tochouseito` に更新した。MIT の許諾本文は変更していない。
これはユーザー所有の First-party Source の表記確定であり、第三者の帰属や許諾文を置き換えるものではない。

## 配布前の確認範囲

- Engine・Editor・SDK・Hub の実パッケージごとに、含めた Binary / Source と Notice の対応を検証する
- MIT 本文、フォント帰属、stb、CC BY の出典をインストーラーや ZIP への同梱後も保持する
- MSVC / CRT / Windows SDK / DXC / .NET / Hub 更新基盤の配布条件は、下記 OSS の MIT だけで判断しない。別途の実物・条項確認と統合する
- 公開サイト、署名、実際の GitHub Release / Tag はこの監査では作成していない

## 一次資料

- [Dear ImGui 固定 Tag の MIT](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/LICENSE.txt)
- [固定 Tag の Font・補助実装](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/imgui_draw.cpp)
- [固定 Tag の Font 説明](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/docs/FONTS.md)
- [固定 Tag の stb License](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/imstb_rectpack.h)
- [ProggyClean の確認 Commit](https://github.com/bluescan/proggyfonts/blob/139ec08a38096161291792313ef5803fc4f0e37b/LICENSE)
- [ProggyForever の確認 Commit](https://github.com/ocornut/proggyforever/blob/f8868ba43bb42c4d88454d03ad81cbd139789f72/LICENSE.txt)
- [WinPixEventRuntime の MIT 化発表](https://devblogs.microsoft.com/pix/open-sourcing-the-winpixeventruntime-under-mit/)
- [PixEvents 公式 Repository](https://github.com/microsoft/PixEvents)
- [固定版 WinPixEventRuntime Package](https://www.nuget.org/packages/WinPixEventRuntime/1.0.240308001)
- [vcpkg の MIT](https://github.com/microsoft/vcpkg/blob/master/LICENSE.txt)
- [既存 ECS 移植記録](M11-Legacy-ECS-Migration.md)

## 検証結果

License 本文・固定版・現物の Notice を読み取り確認し、上記 Notice を補完した。
変更対象は Documentation と License Text のみであるため、Engine Build / CTest は実行していない。
配布パッケージそのものの生成・展開・Notice 同梱検証は、Packaging 実装後の確認対象として残る。

# ADR-0030: 大容量 Engine の分割配布と静的 Catalog

- Status: Accepted（2026-11-09 のユーザー指定）
- 対象: Windows / x64 Engine SDK、Hub の配布取得、公開 CI
- ADR-0021 の署名・新規 Stage・既存 Release 保全を維持する

## 配信経路

公開 CI だけが GitHub API で Release を列挙し、Pages 用の署名付き Catalog を生成する。
Hub は Pages の署名付き Catalog と GitHub Releases の通常 HTTPS URL を読む。
利用者の Token や Release Asset API を通常取得の前提にしない。
Engine と Hub の公開完了後はどちらも reference-site workflow を起動し、Catalog を更新する。
署名鍵は既存の公開設定を維持し、ネットワークから取得した鍵へ置換しない。

GitHub の公式資料では Release ごとの Asset は最大 1000 個、1 File は 2 GiB 未満、Release 全体の容量と帯域は上限なしとされる。
この配布では追加の Object Storage や CDN 契約を導入せず、100 GB 程度の Engine を 100 人が別回線から取得できる構成を目指す。
これは帯域・同時取得速度・稼働率の保証ではない。100 GB × 100 人の実負荷試験は行わない。
根拠: [GitHub About releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases#storage-and-bandwidth-quotas)（2026-11-09 確認）

## Engine Manifest

小さい SDK は既存の `schemaVersion=1` と ZIP 1 個を維持する。
既定では圧縮 ZIP が 2,147,483,647 Byte を超えた場合だけ、ZIP の Byte 列を順序付き File に分ける。
ZIP Entry の境界で分けないため、単一の巨大な元 File にも対応する。
ZIP は .NET の ZipArchive を使い、必要な場合は ZIP64 となる。

`schemaVersion=2` は v1 の全項目を保持し、`packageParts` だけを追加する。

```json
{
    "schemaVersion": 2,
    "product": "engine",
    "version": "1.0.0",
    "tag": "engine-v1.0.0",
    "platform": "Windows",
    "architecture": "x64",
    "configuration": "Development",
    "buildId": "0123456789012345678901234567890123456789",
    "packageFileName": "CueEngine-1.0.0-windows-x64.zip",
    "packageSha256": "<結合 ZIP の SHA-256 Hex 64文字>",
    "packageSize": 2147483648,
    "expandedSize": 3221225472,
    "packageParts": [
        {
            "fileName": "CueEngine-1.0.0-windows-x64.zip.001",
            "sha256": "<Part の SHA-256 Hex 64文字>",
            "size": 1073741824
        },
        {
            "fileName": "CueEngine-1.0.0-windows-x64.zip.002",
            "sha256": "<Part の SHA-256 Hex 64文字>",
            "size": 1073741824
        }
    ]
}
```

例の Hash は説明用であり、有効な Manifest ではない。
`packageFileName` は論理 ZIP 名で、v2 の Release にその ZIP 自体を添付しない。
`packageSha256` と `packageSize` は Part を Array 順に結合した ZIP 全体、`expandedSize` は展開全体を指す。
Part は論理 ZIP 名へ `.001` から 3 桁の連番を付け、各 Part の項目を `fileName`、`sha256`、`size` に限定する。
個別 Size は正の整数かつ 2 GiB 未満、個別 Hash と合計 Size および結合 Hash を検証する。
Root / Part の重複 Key、未知項目、欠落、順序違反、Path、Link、文字列の Size を拒否する。
Manifest の元 UTF-8 Byte 列を従来どおり ECDSA P-256 / SHA-256 / P1363 で署名する。
Hub 自己更新の Manifest v1 は変更しない。

| 制限 | 値 |
| --- | --- |
| 既定 Part 容量 | 1 GiB |
| Part 最大容量 / 単一 ZIP 最大容量 | 2 GiB − 1 Byte |
| Part 最大個数 | 512 |
| ZIP 全体 / 展開全体 | それぞれ 1 TiB 以下 |
| 署名付き Manifest | 256 KiB 以下 |

個数制限と容量制限は同時に適用する。既定 1 GiB の Part では 512 GiB までとなる。
100 GB は既定設定の範囲内に入る。

## 生成と公開

`New-EnginePackage.ps1` は所有 Marker 付き新規 Stage に Payload と一時 ZIP を作る。
分割時は固定 Buffer で読み、Part を CreateNew で書き、既存 File を上書きしない。
全 Part と結合 Hash の検証後、今回生成した Stage 直下の ZIP だけを範囲確認して除去する。
成功した Stage を新規出力先へ Rename する。生成・署名の失敗時は既存出力を変更せず、失敗 Stage を診断用に保持する。
現在の生成は Payload、一時 ZIP、Part を同時に保持するため、それらと元 Build が収まる空き容量が必要になる。
標準 hosted runner の空き容量や Job 時間が 100 GB の梱包に十分であるとは扱わず、大容量を実際に公開する際は Runner の容量を用意する。

`Publish-TaggedRelease.ps1` は署名、厳密な Schema、File 名、個別 Hash / Size、結合 Hash / Size を検証してから GitHub CLI を呼ぶ。
巨大 ZIP の再結合コピーは作らない。Manifest / Signature を添付した Draft を作り、検証した各 Part を個別に Upload してから公開する。
個別 Upload により、最大 512 個の絶対 Path が Windows の Command line 長上限に達することを避ける。
既存 Draft を含む同一 Tag の Release は上書きしない。

## Hub の導入

Hub は署名済みメタデータに対応した専用 Cache に、単一 ZIP または各 Part を取得する。
中断後は HTTPS Range と署名済み容量に基づいて再開し、最後に署名済み SHA-256 と全 Byte を照合する。ETag / Last-Modified は保存しない。
Server が Range を無視した場合は先頭から再取得し、内容が一致しない場合は途中 File を破棄する。
各 Part の Hash と Size、結合 ZIP の Hash と Size を確認した上で、seek 可能な結合 Stream を ZipArchive に渡す。
100 GB の再結合 File を別途作らず、従来どおり展開先 Stage の安全性と展開上限を検証してから導入先を公開する。
詳細な再開条件と Cache 所有契約は Hub 実装の検証対象とする。

## 検証

`PackageCommonTests.ps1` は低い分割閾値で実 ZIP の分割、連番、容量、Hash、既存出力保全と上限拒否を確認する。
`TaggedReleaseTests.ps1` は GitHub CLI を模擬し、v1 の互換性と v2 の公開 Asset、署名、Part 改変、欠損、連番、未知項目、結合不一致を確認する。
100 GB の実梱包、公開 Upload、100 人の並行取得、実際の長時間中断はこの変更のローカル検証に含めない。

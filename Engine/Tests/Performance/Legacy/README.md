# 過去の比較実験

ここには #101 / #144 で使った、当時の本体変更を前提とする部品を保存する。通常の Source / Shader 配布と現行の性能 Tool Target には含めない。

## 保存した部品

- `PerformanceStudy.h` / `EditorTimingProbe.cpp` / `PresentDispatchProbe.h`: 旧比較条件と記録形式
- `DesktopInvestigation.h`: Input Lock / Timestamp Frequency Cache などの旧実験フラグ
- `StudyImGuiRecorder.h` / `Hlsl/StudyImGui.hlsl`: 独自 ImGui Recorder の比較
- `Run*.ps1` / `Summarize*.py` / `AnalyzePixPresent.py`: 当時の Binary / 出力を前提とする実行・解析 Script

これらは現行エンジンへそのまま Include / Build して再利用するものではない。旧 `EditorTimingProbe` を単に Build しても、当時の本体側の計測・分岐は復元されない。`StudyImGuiRecorder` 内の Shader 探索先も当時の配置を前提とする。

現行でも単独利用できる Descriptor 検索 Tool は親 Directory の `CueImGuiDescriptorLookupProbe` として分離した。

## 削除前の本体を参照する

- 基準 Commit: `64ae3d2bde45f2eebbe1016387300d5522165420`
- ローカル保全タグ: `research/m06-issue144-before-cleanup-2026-10-26`

```powershell
git show 'research/m06-issue144-before-cleanup-2026-10-26:Engine/Source/Foundation/Public/Foundation/DesktopInvestigation.h'
git show '64ae3d2:Engine/Source/EditorHost/Private/ImGuiManager.cpp'
```

タグはローカルだけに作成しており、今回 Push は行わない。別 Clone では基準 Commit を指定して読む。整理前から存在した未 Commit のユーザーコメント差分は `out/validation/Performance20261026/InvestigationCleanup/BeforeUserChanges.patch` と対象 File のコピーで保全している。

古い実験を再開する場合は、現行の基準 Branch から調査 Branch を作り、仮説に必要な変更だけを移す。過去の実行 File と現行コードを同じ条件と見なさない。

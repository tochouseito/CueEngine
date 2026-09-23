# CueEngine 作業指示

- 回答とFirst-partyのCodeコメントは日本語を基本とする。識別子、API名、外部Tool名は英語のまま扱う。
- C++、HLSL、CMake、Build設定を変更する前に[コーディング規約](Engine/Documents/CODING_RULES.md)を読む。命名、所有権、Error処理、コメント、Include順、File配置、整形に従う。周辺Codeとの一貫性を優先した例外は作業報告に理由を残す。
- 設計境界は[ADR-0001](Docs/Decisions/0001-architecture-boundaries.md)、Build定義は[ADR-0002](Docs/Decisions/0002-build-system-source-of-truth.md)に従う。CMakeがBuild定義の正本であり、生成されたVisual Studio Projectを管理対象にしない。
- `develop`を開発の統合先とし、Issue単位のBranchで作業する。作業前にBranchと未コミット変更を確認し、既存の変更を保全する。Commit、Push、Mergeはユーザーの明示指示がある場合に行う。
- 変更した範囲とBuild／Testの結果、実施できなかった検証、残るリスクを報告する。

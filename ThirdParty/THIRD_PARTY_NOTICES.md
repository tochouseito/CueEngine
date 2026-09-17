# Third-Party Notices

CueEngineが使用する第三者Softwareと配布条件を記録する。

## Dear ImGui

- Project: Dear ImGui
- Version: 1.92.9b-docking
- Source: <https://github.com/ocornut/imgui>
- Introduction: vcpkg Manifest Mode
- Enabled features: `docking-experimental`, `dx12-binding`, `win32-binding`
- License: MIT License
- License copy: `Licenses/DearImGui-LICENSE.txt`
- Usage: CueEngineのWindows Tool UIだけで使用し、Runtime Moduleへ公開しない

## vcpkg

- Project: vcpkg
- Tool release: 2026-07-27
- Tool commit: `386d7c478221b7ee0c97bfe6ea61dcf65121d564`
- Source: <https://github.com/microsoft/vcpkg>
- License: MIT License
- License copy: `Licenses/vcpkg-LICENSE.txt`
- Usage: 承認済み第三者Dependencyの復元に使用するBuild Tool

第三者SoftwareのSource、Header、Binary、Install TreeはGit管理対象の`Engine`配下へ配置しない。
生成された`ThirdParty/.tools`と`ThirdParty/vcpkg_installed`は配布物ではなく、明示Dependency Restoreの出力である。

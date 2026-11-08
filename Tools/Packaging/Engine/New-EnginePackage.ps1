param(
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [Parameter(Mandatory)][string]$SigningKeyFile,
    [string]$CrtDirectory,
    [switch]$SkipBuild,
    [switch]$AllowDirtyLocal
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../Scripts/PackageCommon.ps1')
$engineRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$definition = Read-CueVersion (Join-Path $engineRoot 'Engine/Version.json') engine
$version = $definition.version
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $engineRoot 'out/build/windows-vs2026-package' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $engineRoot "out/packages/Engine/$version" }
$buildRoot = Assert-CuePlainPath $BuildDirectory
$outputRoot = Assert-CuePlainPath $OutputDirectory
if (-not (Test-Path -LiteralPath $SigningKeyFile -PathType Leaf)) { throw 'Package signing key is required' }
if (Test-Path -LiteralPath $outputRoot) { throw "Package output already exists: $outputRoot" }

# 公開用は確定した Source から生成する。作業中 Tree はローカル検証に限って明示許可する
$commit = (& git -C $engineRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -cnotmatch '^[0-9a-f]{40}$') { throw 'A valid source commit is required' }
$dirty = @(& git -C $engineRoot status --porcelain).Count -gt 0
if ($dirty -and -not $AllowDirtyLocal) { throw 'Public packages require a clean checkout; use AllowDirtyLocal only for local validation' }
$buildId = $commit + $(if ($dirty) { '-dirty' } else { '' })

# Build 定義は CMake を正本とし、通常の Debug 開発 Tree と配布用 Tree を混ぜない
if (-not $SkipBuild)
{
    . (Join-Path $engineRoot 'scripts/vcpkg_environment.ps1')
    $env:VCPKG_ROOT = get_vcpkg_root
    Push-Location -LiteralPath $engineRoot
    try
    {
        & cmake --preset windows-vs2026 -B $buildRoot '-DBUILD_TESTING=OFF' '-DCUE_GAME_PROJECT=' '-DCUE_DISTRIBUTION_BUILD=ON'
        if ($LASTEXITCODE -ne 0) { throw 'Engine package Configure failed' }
        & cmake --build $buildRoot --config Development --target CueEditorHost CuePlayHost CueProjectTool --parallel
        if ($LASTEXITCODE -ne 0) { throw 'Engine package Build failed' }
    }
    finally { Pop-Location }
}
$cacheFile = Join-Path $buildRoot 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cacheFile -PathType Leaf)) { throw 'Package CMake cache is missing' }

# 複数一致や空値を診断し、別 Tree の依存物を推測でコピーしない
function Read-CueCache([string]$Name)
{
    $lines = @(Get-Content -LiteralPath $cacheFile | Where-Object { $_ -cmatch ('^' + [regex]::Escape($Name) + ':[^=]+=') })
    if ($lines.Count -ne 1) { throw "Package cache value is missing or ambiguous: $Name" }
    return $lines[0].Split('=', 2)[1]
}
if ((Read-CueCache 'CUE_DISTRIBUTION_BUILD') -cne 'ON') { throw 'Package binaries must use CUE_DISTRIBUTION_BUILD=ON' }
if ([IO.Path]::GetFullPath((Read-CueCache 'CMAKE_HOME_DIRECTORY')) -ine $engineRoot) { throw 'Build directory belongs to another Engine checkout' }

# Binary に埋めた識別値を正本にし、別 Commit の既存出力へ新しいラベルを付けない
$generatedHeader = Join-Path $buildRoot 'Generated/EngineInstallation/EngineVersion.h'
$builtVersion = @(Select-String -LiteralPath $generatedHeader -Pattern '^#define CUE_ENGINE_VERSION "([^"\r\n]+)"$')
$builtIdentifier = @(Select-String -LiteralPath $generatedHeader -Pattern '^#define CUE_ENGINE_BUILD_ID "([0-9A-Za-z._+-]+)"$')
if ($builtVersion.Count -ne 1 -or $builtIdentifier.Count -ne 1 -or
    $builtVersion[0].Matches[0].Groups[1].Value -cne $version -or
    $builtIdentifier[0].Matches[0].Groups[1].Value -cne $buildId)
{
    throw 'Built binaries do not match the requested version and source identity; rebuild before packaging'
}
$buildId = $builtIdentifier[0].Matches[0].Groups[1].Value

$stage = New-CuePackageStage $outputRoot
$payload = Join-Path $stage 'payload'
[IO.Directory]::CreateDirectory($payload) | Out-Null
try
{
    # Source-backed SDK の許可リスト。Git、個人設定、Test、Compiler、元Projectの生成物は含めない
    foreach ($name in @('CMakeLists.txt', 'CMakePresets.json', 'LICENSE.txt'))
    {
        [IO.File]::Copy((Join-Path $engineRoot $name), (Join-Path $payload $name), $false)
    }
    foreach ($name in @('Engine/Source', 'Engine/Shader', 'cmake', 'Templates', 'Tools/VisualStudioBridge', 'ThirdParty/Fonts'))
    {
        Copy-CuePackageTree (Join-Path $engineRoot $name) (Join-Path $payload $name)
    }
    [IO.Directory]::CreateDirectory((Join-Path $payload 'Engine')) | Out-Null
    [IO.File]::Copy((Join-Path $engineRoot 'Engine/Version.json'), (Join-Path $payload 'Engine/Version.json'), $false)
    Copy-CuePackageTree (Join-Path $engineRoot 'ThirdParty/Licenses') (Join-Path $payload 'ThirdParty/Licenses')
    Copy-CuePackageTree (Join-Path $engineRoot 'ThirdParty/Licenses') (Join-Path $payload 'Licenses')
    [IO.File]::Copy((Join-Path $engineRoot 'ThirdParty/THIRD_PARTY_NOTICES.md'), (Join-Path $payload 'Licenses/THIRD_PARTY_NOTICES.md'), $false)
    [IO.File]::Copy((Join-Path $engineRoot 'LICENSE.txt'), (Join-Path $payload 'Licenses/CueEngine-MIT.txt'), $false)
    foreach ($name in @('WinPixEventRuntime.cmake', 'DirectXShaderCompiler.cmake', 'vcpkg.json'))
    {
        [IO.File]::Copy((Join-Path $engineRoot "ThirdParty/$name"), (Join-Path $payload "ThirdParty/$name"), $false)
    }

    # Toolchain は利用者のものを検出する。SDK 内には imgui の再配置可能な成果物だけを入れる
    foreach ($name in @('include', 'lib', 'debug/lib', 'share/imgui'))
    {
        $dependencySource = Join-Path $engineRoot "ThirdParty/vcpkg_installed/x64-windows/$name"
        $dependencyTarget = Join-Path $payload "ThirdParty/vcpkg_installed/x64-windows/$name"
        Copy-CuePackageTree $dependencySource $dependencyTarget
    }
    $packageDirectory = Join-Path $payload 'ThirdParty/Packages'
    [IO.Directory]::CreateDirectory($packageDirectory) | Out-Null
    $pixArchive = Join-Path $buildRoot 'ThirdParty/WinPixEventRuntime-1.0.240308001.zip'
    $pixOverride = Read-CueCache 'CUE_PIX_ARCHIVE'
    if ($pixOverride) { $pixArchive = $pixOverride }
    $dxcArchive = Read-CueCache 'CUE_DXC_ARCHIVE'
    foreach ($archive in @($pixArchive, $dxcArchive))
    {
        if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) { throw 'Verified dependency archive is missing' }
        [IO.File]::Copy($archive, (Join-Path $packageDirectory ([IO.Path]::GetFileName($archive))), $false)
    }

    $editorDirectory = Join-Path $payload 'Bin/Editor'
    $toolDirectory = Join-Path $payload 'Bin/Tools'
    [IO.Directory]::CreateDirectory($editorDirectory) | Out-Null
    [IO.Directory]::CreateDirectory($toolDirectory) | Out-Null
    $binaries = Join-Path $buildRoot 'bin/Development'
    foreach ($name in @('CueEditorHost.exe', 'CuePlayHost.exe', 'VisualStudioBridge.exe', 'VisualStudioBridge.exe.config',
        'dxcompiler.dll', 'dxil.dll', 'WinPixEventRuntime.dll'))
    {
        [IO.File]::Copy((Join-Path $binaries $name), (Join-Path $editorDirectory $name), $false)
    }
    foreach ($name in @('CueEditorHost.pdb', 'CuePlayHost.pdb', 'VisualStudioBridge.pdb'))
    {
        $file = Join-Path $binaries $name
        if (Test-Path -LiteralPath $file -PathType Leaf) { [IO.File]::Copy($file, (Join-Path $editorDirectory $name), $false) }
    }
    Copy-CuePackageTree (Join-Path $binaries 'EngineResources') (Join-Path $editorDirectory 'EngineResources')
    foreach ($name in @('CueProjectTool.exe', 'CueProjectTool.pdb'))
    {
        $file = Join-Path $binaries $name
        if (Test-Path -LiteralPath $file -PathType Leaf) { [IO.File]::Copy($file, (Join-Path $toolDirectory $name), $false) }
        elseif ($name.EndsWith('.exe')) { throw 'ProjectTool executable is missing' }
    }

    # 再配布一覧に属する Retail CRT のみを app-local に配置する
    if (-not $CrtDirectory)
    {
        $vsRoot = Read-CueCache 'CMAKE_GENERATOR_INSTANCE'
        $redistRoot = Join-Path $vsRoot 'VC/Redist/MSVC'
        $redist = Get-ChildItem -LiteralPath $redistRoot -Directory | Where-Object Name -Match '^[0-9]+\.[0-9]+\.[0-9]+$' |
            Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
        if (-not $redist) { throw 'Retail VC++ redistributable directory is missing' }
        $crt = @(Get-ChildItem -LiteralPath (Join-Path $redist.FullName 'x64') -Directory | Where-Object Name -Match '^Microsoft\.VC[0-9]+\.CRT$')
        if ($crt.Count -ne 1) { throw 'Retail CRT selection is ambiguous' }
        $CrtDirectory = $crt[0].FullName
    }
    $null = Assert-CuePlainPath $CrtDirectory
    foreach ($file in Get-ChildItem -LiteralPath $CrtDirectory -Filter '*.dll' -File)
    {
        foreach ($destination in @($editorDirectory, $toolDirectory))
        {
            [IO.File]::Copy($file.FullName, (Join-Path $destination $file.Name), $false)
        }
    }
    $descriptor = @(
        'formatVersion=1', 'product=engine', "version=$version", "buildId=$buildId", 'platform=Windows',
        'architecture=x64', 'configuration=Development', "sdkAbiVersion=$($definition.sdkAbiVersion)",
        'projectFormatMin=1', 'projectFormatMax=2', 'editor=Bin/Editor/CueEditorHost.exe',
        'projectTool=Bin/Tools/CueProjectTool.exe', "useLeaseId=engine-$version-windows-x64"
    ) -join "`n"
    [IO.File]::WriteAllText((Join-Path $payload 'engine.cueengine'), $descriptor + "`n", [Text.UTF8Encoding]::new($false))
    Assert-CueRedistributablePayload $payload
    $packageName = "CueEngine-$version-windows-x64.zip"
    $packageFile = Join-Path $stage $packageName
    $expandedSize = New-CueZip $payload $packageFile
    $manifestFile = Join-Path $stage 'engine-release.json'
    Write-CueJson $manifestFile ([ordered]@{
        schemaVersion = 1; product = 'engine'; version = $version; tag = "engine-v$version";
        platform = 'Windows'; architecture = 'x64'; configuration = 'Development'; buildId = $buildId;
        packageFileName = $packageName; packageSha256 = (Get-FileHash -LiteralPath $packageFile -Algorithm SHA256).Hash.ToLowerInvariant();
        packageSize = (Get-Item -LiteralPath $packageFile).Length; expandedSize = $expandedSize
    })
    Protect-CueManifest $manifestFile $SigningKeyFile
    $null = Assert-CuePlainPath $outputRoot
    if (Test-Path -LiteralPath $outputRoot) { throw 'Another package writer has published this output' }
    [IO.Directory]::Move($stage, $outputRoot)
    Write-Host "Engine package: $outputRoot"
}
catch
{
    # 不明な混入 File を再帰削除せず、失敗した所有 Stage を診断用に残す
    Write-Warning "Package generation failed; staging retained: $stage"
    throw
}

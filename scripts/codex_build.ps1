param(
    [ValidateSet('Debug', 'Development', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$GameProject,
    [string]$GamePlatform,
    [string]$GameArchitecture,
    [switch]$PlayerOnly
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vcpkg_environment.ps1')
$env:VCPKG_ROOT = get_vcpkg_root
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$buildPreset = "windows-vs2026-$($Configuration.ToLowerInvariant())"
# 明示した Project だけを Configure へ渡す。未指定時は Preset の未選択値を使う
$configureArguments = @('--preset', 'windows-vs2026')
if ($PSBoundParameters.ContainsKey('GameProject'))
{
    if ([string]::IsNullOrWhiteSpace($GameProject))
    {
        $configureArguments += '-DCUE_GAME_PROJECT='
    }
    else
    {
        $resolvedProject = (Resolve-Path -LiteralPath $GameProject -ErrorAction Stop).Path
        $configureArguments += "-DCUE_GAME_PROJECT=$resolvedProject"
    }
}
# 出力対象は Build 要求として渡す。未対応値も CMake の登録情報で診断し、Native 値へ置き換えない
if ($PSBoundParameters.ContainsKey('GamePlatform'))
{
    $configureArguments += "-DCUE_GAME_PLATFORM=$GamePlatform"
}
if ($PSBoundParameters.ContainsKey('GameArchitecture'))
{
    $configureArguments += "-DCUE_GAME_ARCHITECTURE=$GameArchitecture"
}

Push-Location -LiteralPath $repositoryRoot
try
{
    cmake @configureArguments
    if ($LASTEXITCODE -ne 0)
    {
        exit $LASTEXITCODE
    }

    $buildArguments = @('--build', '--preset', $buildPreset)
    if ($PlayerOnly)
    {
        $buildArguments += @('--target', 'Cue.GamePlayerPackage')
    }
    cmake @buildArguments
    exit $LASTEXITCODE
}
finally
{
    Pop-Location
}

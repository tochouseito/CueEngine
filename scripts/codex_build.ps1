param(
    [ValidateSet('Debug', 'Development', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vcpkg_environment.ps1')
$env:VCPKG_ROOT = get_vcpkg_root
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$buildPreset = "windows-vs2026-$($Configuration.ToLowerInvariant())"

Push-Location -LiteralPath $repositoryRoot
try
{
    cmake --preset windows-vs2026
    if ($LASTEXITCODE -ne 0)
    {
        exit $LASTEXITCODE
    }

    cmake --build --preset $buildPreset
    exit $LASTEXITCODE
}
finally
{
    Pop-Location
}

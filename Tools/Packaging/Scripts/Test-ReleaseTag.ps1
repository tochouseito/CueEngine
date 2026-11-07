param([Parameter(Mandatory)][string]$Tag)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageCommon.ps1')
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if ($Tag.StartsWith('engine-v', [StringComparison]::Ordinal))
{
    $product = 'engine'
    $definition = Read-CueVersion (Join-Path $sourceRoot 'Engine/Version.json') $product
}
elseif ($Tag.StartsWith('hub-v', [StringComparison]::Ordinal))
{
    $product = 'hub'
    $definition = Read-CueVersion (Join-Path $sourceRoot 'Tools/CueHub/Version.json') $product
}
else { throw 'Only engine-v and hub-v release tags are supported' }
if ($Tag -cne "$product-v$($definition.version)") { throw 'Tag does not match the committed version definition' }
$preview = $definition.version.Split('+')[0].Contains('-').ToString().ToLowerInvariant()
if ($env:GITHUB_OUTPUT)
{
    [IO.File]::AppendAllText($env:GITHUB_OUTPUT, "product=$product`nversion=$($definition.version)`npreview=$preview`n", [Text.UTF8Encoding]::new($false))
}
Write-Output "Validated $product $($definition.version); preview=$preview"

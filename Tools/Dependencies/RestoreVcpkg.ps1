[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$thirdPartyRoot = Join-Path $repositoryRoot "ThirdParty"
$toolRoot = Join-Path $thirdPartyRoot ".tools\vcpkg"
$configurationPath = Join-Path $thirdPartyRoot "vcpkg-tool.json"
$configuration = Get-Content -Raw -LiteralPath $configurationPath | ConvertFrom-Json
$env:VCPKG_ROOT = $toolRoot

function Invoke-CheckedProcess
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string[]]$ArgumentList,

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory
    )

    Push-Location $WorkingDirectory
    try
    {
        & $FilePath @ArgumentList
        if ($LASTEXITCODE -ne 0)
        {
            throw "Process failed with exit code ${LASTEXITCODE}: $FilePath $($ArgumentList -join ' ')"
        }
    }
    finally
    {
        Pop-Location
    }
}

function Test-VcpkgExecutable
{
    $executablePath = Join-Path $toolRoot "vcpkg.exe"
    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf))
    {
        return $false
    }

    $actualHash = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $configuration.windowsX64Sha256)
    {
        return $false
    }

    $versionText = (& $executablePath version | Out-String)
    if ($LASTEXITCODE -ne 0 -or -not $versionText.Contains($configuration.windowsX64Version))
    {
        return $false
    }

    return $true
}

if (-not (Test-Path -LiteralPath $toolRoot -PathType Container))
{
    $toolParent = Split-Path -Parent $toolRoot
    New-Item -ItemType Directory -Path $toolParent -Force | Out-Null
    Invoke-CheckedProcess -FilePath "git" -ArgumentList @(
        "clone",
        "--filter=blob:none",
        "--no-checkout",
        $configuration.repository,
        $toolRoot
    ) -WorkingDirectory $toolParent
    Invoke-CheckedProcess -FilePath "git" -ArgumentList @(
        "checkout",
        "--detach",
        $configuration.commit
    ) -WorkingDirectory $toolRoot
}

$trackedChanges = (& git -c "safe.directory=$toolRoot" -C $toolRoot status --porcelain --untracked-files=no | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $trackedChanges.Length -ne 0)
{
    throw "Managed vcpkg checkout contains tracked changes."
}

$actualRepository = (& git -c "safe.directory=$toolRoot" -C $toolRoot remote get-url origin).Trim()
if ($LASTEXITCODE -ne 0 -or $actualRepository -ne $configuration.repository)
{
    throw "Managed vcpkg checkout origin does not match the pinned repository."
}

$actualCommit = (& git -c "safe.directory=$toolRoot" -C $toolRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0)
{
    throw "Managed vcpkg checkout commit could not be read."
}
if ($actualCommit -ne $configuration.commit)
{
    Invoke-CheckedProcess -FilePath "git" -ArgumentList @(
        "-c",
        "safe.directory=$toolRoot",
        "-C",
        $toolRoot,
        "fetch",
        "--filter=blob:none",
        "origin",
        $configuration.commit
    ) -WorkingDirectory $repositoryRoot
    Invoke-CheckedProcess -FilePath "git" -ArgumentList @(
        "-c",
        "safe.directory=$toolRoot",
        "-C",
        $toolRoot,
        "checkout",
        "--detach",
        $configuration.commit
    ) -WorkingDirectory $repositoryRoot
}

$actualCommit = (& git -c "safe.directory=$toolRoot" -C $toolRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $configuration.commit)
{
    throw "Managed vcpkg checkout does not match the pinned commit."
}

$metadataPath = Join-Path $toolRoot "scripts\vcpkg-tool-metadata.txt"
$metadata = Get-Content -Raw -LiteralPath $metadataPath | ConvertFrom-StringData
if ($metadata.VCPKG_TOOL_RELEASE_TAG -ne $configuration.release)
{
    throw "Managed vcpkg tool release does not match the pinned release."
}
if ($metadata.VCPKG_TOOL_SOURCE_SHA -ne $configuration.sourceSha512)
{
    throw "Managed vcpkg tool source hash does not match the pinned hash."
}

if (-not (Test-VcpkgExecutable))
{
    $bootstrapPath = Join-Path $toolRoot "bootstrap-vcpkg.bat"
    Invoke-CheckedProcess -FilePath $bootstrapPath -ArgumentList @("-disableMetrics") -WorkingDirectory $toolRoot
}

if (-not (Test-VcpkgExecutable))
{
    throw "Bootstrapped vcpkg executable does not match the pinned version and hash."
}

$vcpkgPath = Join-Path $toolRoot "vcpkg.exe"
$installedRoot = Join-Path $thirdPartyRoot "vcpkg_installed"
$env:VCPKG_DISABLE_METRICS = "1"
Invoke-CheckedProcess -FilePath $vcpkgPath -ArgumentList @(
    "install",
    "--x-manifest-root=$thirdPartyRoot",
    "--x-install-root=$installedRoot",
    "--triplet=x64-windows"
) -WorkingDirectory $repositoryRoot

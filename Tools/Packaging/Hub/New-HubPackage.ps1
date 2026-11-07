param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$SigningKeyFile,
    [Parameter(Mandatory)][string]$TrustedPublisherKeyFile,
    [string]$VersionOverride
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
. (Join-Path $PSScriptRoot '../Scripts/PackageCommon.ps1')
$definition = Read-CueVersion (Join-Path $repositoryRoot 'Tools/CueHub/Version.json') 'hub'
$version = $definition.version
if ($VersionOverride)
{
    if (-not (Test-CueVersion $VersionOverride)) { throw 'Invalid local test version override' }
    $version = $VersionOverride
}
$privateKey = Assert-CuePlainPath $SigningKeyFile
$publicKey = Assert-CuePlainPath $TrustedPublisherKeyFile
if (-not (Test-Path -LiteralPath $privateKey -PathType Leaf) -or -not (Test-Path -LiteralPath $publicKey -PathType Leaf))
{
    throw 'Manifest signing key and trusted publisher public key are required'
}
if ([IO.File]::ReadAllText($publicKey).Contains('PRIVATE KEY')) { throw 'Only a public key may be included in Hub' }
# 署名鍵と同梱信頼鍵が異なる配布物を生成しない
$signer = [Security.Cryptography.ECDsa]::Create()
$verifier = [Security.Cryptography.ECDsa]::Create()
try
{
    $signer.ImportFromPem([IO.File]::ReadAllText($privateKey))
    $verifier.ImportFromPem([IO.File]::ReadAllText($publicKey))
    $challenge = [Text.Encoding]::UTF8.GetBytes('CueEngine Hub Publisher Key Check')
    $signature = $signer.SignData($challenge, [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
    if ($signer.KeySize -ne 256 -or $verifier.KeySize -ne 256 -or
        $signer.ExportParameters($false).Curve.Oid.Value -cne '1.2.840.10045.3.1.7' -or
        $verifier.ExportParameters($false).Curve.Oid.Value -cne '1.2.840.10045.3.1.7' -or -not $verifier.VerifyData($challenge, $signature,
        [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation))
    {
        throw 'Manifest signing key does not match trusted publisher public key'
    }
}
finally { $signer.Dispose(); $verifier.Dispose() }

$output = Assert-CuePlainPath $OutputDirectory
$stage = New-CuePackageStage $output
$publish = Join-Path $stage 'Publish'
$artifacts = Join-Path $stage 'Release'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
if (-not $env:DOTNET_CLI_HOME) { $env:DOTNET_CLI_HOME = Join-Path $repositoryRoot 'out/dotnet-home' }
if (-not $env:NUGET_PACKAGES) { $env:NUGET_PACKAGES = Join-Path $repositoryRoot 'out/nuget/packages' }
Push-Location -LiteralPath $repositoryRoot
try
{
    & dotnet publish Tools/CueHub/CueHub.csproj -c Release -r win-x64 --self-contained true -o $publish "/p:Version=$version" /p:RestoreLockedMode=true
    if ($LASTEXITCODE -ne 0) { throw 'Hub self-contained publish failed' }
    [IO.File]::Copy($publicKey, (Join-Path $publish 'PublisherPublicKey.pem'), $false)
    $licenseRoot = Join-Path $publish 'Licenses'
    [IO.Directory]::CreateDirectory($licenseRoot) | Out-Null
    [IO.File]::Copy((Join-Path $repositoryRoot 'LICENSE.txt'), (Join-Path $licenseRoot 'CueEngine-MIT.txt'), $false)
    foreach ($notice in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'Licenses') -File)
    {
        [IO.File]::Copy($notice.FullName, (Join-Path $licenseRoot $notice.Name), $false)
    }

    # 実際に Publish へ組み込んだ Runtime の版を根拠に Notice を収録する
    $runtime = Get-Content -LiteralPath (Join-Path $publish 'CueHub.runtimeconfig.json') -Raw | ConvertFrom-Json
    foreach ($framework in $runtime.runtimeOptions.includedFrameworks)
    {
        $packageId = ($framework.name + '.Runtime.win-x64').ToLowerInvariant()
        $packageRoot = Join-Path $env:NUGET_PACKAGES ($packageId + '/' + $framework.version)
        $null = Assert-CuePlainPath $packageRoot
        $notices = @(Get-ChildItem -LiteralPath $packageRoot -File | Where-Object { $_.Name -imatch '^(?:LICENSE(?:\.TXT)?|THIRD[-_]?PARTY[-_]?NOTICES.*\.txt)$' })
        if ($notices.Count -eq 0 -or ($framework.name -ceq 'Microsoft.NETCore.App' -and $notices.Count -lt 2)) { throw "Runtime License / ThirdPartyNotices are missing: $packageId $($framework.version)" }
        $destination = Join-Path $licenseRoot ($framework.name + '-' + $framework.version)
        [IO.Directory]::CreateDirectory($destination) | Out-Null
        foreach ($notice in $notices) { [IO.File]::Copy($notice.FullName, (Join-Path $destination $notice.Name), $false) }
    }
    $lock = Get-Content -LiteralPath (Join-Path $repositoryRoot 'Tools/CueHub/Core/packages.lock.json') -Raw | ConvertFrom-Json -AsHashtable
    $dependencies = $lock.dependencies.Values | Select-Object -First 1
    if ($dependencies.Count -ne 1 -or -not $dependencies.ContainsKey('Velopack') -or $dependencies.Velopack.resolved -cne '1.2.161')
    {
        throw 'Hub dependency inventory changed; review licenses before packaging'
    }
    Assert-CueRedistributablePayload $publish
    & dotnet tool restore --tool-manifest Tools/Packaging/Hub/.config/dotnet-tools.json
    if ($LASTEXITCODE -ne 0) { throw 'Pinned Velopack CLI restore failed' }
    $vendorNotice = Join-Path $env:NUGET_PACKAGES 'vpk/1.2.161/vendor/THIRD_PARTY_NOTICES.md'
    [IO.File]::Copy((Assert-CuePlainPath $vendorNotice), (Join-Path $licenseRoot 'Velopack-Vendor-Notices.md'), $false)
    Push-Location -LiteralPath $PSScriptRoot
    try
    {
        & dotnet tool run vpk -- pack --packId CueEngine.Hub --packVersion $version --packDir $publish --mainExe CueHub.exe --packTitle 'CueEngine Hub' --packAuthors tochouseito --runtime win-x64 --channel win --delta None --shortcuts StartMenuRoot --instLocation PerUser --outputDir $artifacts
        if ($LASTEXITCODE -ne 0) { throw 'Velopack packaging failed' }
    }
    finally { Pop-Location }
    $feedFile = Join-Path $artifacts 'releases.win.json'
    $feed = Get-Content -LiteralPath $feedFile -Raw | ConvertFrom-Json -AsHashtable
    $full = @($feed.Assets | Where-Object { $_.Type -ceq 'Full' -and $_.Version -ceq $version })
    if ($full.Count -ne 1) { throw 'Exactly one full Hub update package is required' }
    $feed.Assets = @($full[0])
    Write-CueJson $feedFile $feed
    $package = Join-Path $artifacts $full[0].FileName
    $manifestFile = Join-Path $artifacts 'hub-release.json'
    Write-CueJson $manifestFile ([ordered]@{
        schemaVersion = 1
        product = 'hub'
        version = $version
        tag = 'hub-v' + $version
        packageFileName = [IO.Path]::GetFileName($package)
        packageSha256 = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
        packageSize = (Get-Item -LiteralPath $package).Length
        feedFileName = 'releases.win.json'
        feedSha256 = (Get-FileHash -LiteralPath $feedFile -Algorithm SHA256).Hash.ToLowerInvariant()
        feedSize = (Get-Item -LiteralPath $feedFile).Length
    })
    Protect-CueManifest $manifestFile $privateKey
    [IO.File]::Delete((Join-Path $stage '.cue-package-owner'))
    [IO.Directory]::Move($stage, $output)
    Write-Output "Hub package: $output"
}
catch
{
    Write-Warning "Package stage preserved for diagnosis: $stage"
    throw
}
finally { Pop-Location }

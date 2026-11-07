param([string]$TestRoot)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageCommon.ps1')
if (-not $TestRoot) { $TestRoot = Join-Path ([IO.Path]::GetTempPath()) 'CuePackageTests' }
$root = Join-Path (Assert-CuePlainPath $TestRoot) ([Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$checks = 0
function Assert-CueTest([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw $Message }
    $script:checks++
}
foreach ($version in @('0.1.0-preview.1', '1.2.3', '1.2.3-rc.1+build.02'))
{
    Assert-CueTest (Test-CueVersion $version) "Rejected valid version: $version"
}
foreach ($version in @('', '1.2', '01.2.3', '1.2.3-preview.01', '1.2.3-preview..1', '../1.2.3', ('1.2.3-' + ('x' * 129))))
{
    Assert-CueTest (-not (Test-CueVersion $version)) 'Accepted invalid version'
}

# 原本 JSON の Byte を保持した署名が成功し、改変内容は拒否する
$key = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
try
{
    $privateFile = Join-Path $root 'test-private.pem'
    [IO.File]::WriteAllText($privateFile, $key.ExportPkcs8PrivateKeyPem())
    $manifest = Join-Path $root 'manifest.json'
    Write-CueJson $manifest ([ordered]@{ schemaVersion = 1; name = '日本語 Package'; value = 1 })
    Protect-CueManifest $manifest $privateFile
    $bytes = [IO.File]::ReadAllBytes($manifest)
    $signature = [Convert]::FromBase64String([IO.File]::ReadAllText($manifest + '.sig').Trim())
    Assert-CueTest ($signature.Length -eq 64) 'Signature format differs from the Hub contract'
    Assert-CueTest ($key.VerifyData($bytes, $signature, [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)) 'Signature verification failed'
    $bytes[0] = $bytes[0] -bxor 1
    Assert-CueTest (-not $key.VerifyData($bytes, $signature, [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)) 'Modified manifest was accepted'
}
finally
{
    $key.Dispose()
    if (Test-Path -LiteralPath $privateFile) { Remove-Item -LiteralPath $privateFile -Force }
}

# 所有 Stage の公開と ZIP の階層・容量を確認する
$destination = Join-Path $root 'Published'
$stage = New-CuePackageStage $destination
$nested = Join-Path $stage 'Dir'
[IO.Directory]::CreateDirectory($nested) | Out-Null
[IO.File]::WriteAllBytes((Join-Path $nested 'File.bin'), [byte[]](1, 2, 3, 4))
$zipFile = Join-Path $root 'payload.zip'
$expandedSize = New-CueZip $stage $zipFile
Assert-CueTest ($expandedSize -eq 4) 'Expanded size includes unrelated ownership metadata'
$archive = [IO.Compression.ZipFile]::OpenRead($zipFile)
try
{
    Assert-CueTest ($archive.Entries.Count -eq 1 -and $archive.Entries[0].FullName -ceq 'Dir/File.bin') 'ZIP layout is not relative and portable'
}
finally { $archive.Dispose() }
[IO.Directory]::Move($stage, $destination)
$refused = $false
try { $null = New-CuePackageStage $destination } catch { $refused = $true }
Assert-CueTest $refused 'Existing package output was not protected'

# Debug CRT の名前を見落とさず、通常の Retail CRT と区別する
$blockedRoot = Join-Path $root 'Blocked'
[IO.Directory]::CreateDirectory($blockedRoot) | Out-Null
foreach ($name in @('MSVCP140D.dll', 'MSVCP140D_ATOMIC_WAIT.dll', 'VCRUNTIME140_1D.dll', 'ucrtbased.dll', 'cl.exe'))
{
    $file = Join-Path $blockedRoot $name
    [IO.File]::WriteAllBytes($file, [byte[]]::new(0))
    $refused = $false
    try { Assert-CueRedistributablePayload $blockedRoot } catch { $refused = $true }
    Assert-CueTest $refused "Non-redistributable fixture was accepted: $name"
    Remove-Item -LiteralPath $file
}
Write-Output "Package contract checks passed: $checks"

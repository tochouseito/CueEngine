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
# ZIP の Byte 境界を小さくして単一巨大 Entry と同じ経路を検証する
$singleParts = @(Split-CuePackage $zipFile 23 2147483647)
Assert-CueTest ($singleParts.Count -eq 0) 'Small package no longer uses a single ZIP'
$parts = @(Split-CuePackage $zipFile 23 32)
$metadata = [ordered]@{ packageFileName = 'payload.zip'; packageSize = (Get-Item -LiteralPath $zipFile).Length;
    packageSha256 = (Get-FileHash -LiteralPath $zipFile -Algorithm SHA256).Hash; packageParts = $parts }
$assets = @(Assert-CuePackageParts $metadata $root)
Assert-CueTest ($assets.Count -eq $parts.Count -and $parts.Count -gt 1) 'Multipart asset count is incorrect'
$totalPartSize = [long]0
foreach ($part in $parts) { $totalPartSize += $part.size }
Assert-CueTest ($totalPartSize -eq $metadata.packageSize) 'Part sizes do not match ZIP size'
for ($index = 0; $index -lt $parts.Count; $index++)
{
    Assert-CueTest ($parts[$index].fileName -ceq ('payload.zip.' + ($index + 1).ToString('000')) -and
        $parts[$index].size -le 23) 'Part ordering or size is incorrect'
}
$originalHash = (Get-FileHash -LiteralPath $zipFile -Algorithm SHA256).Hash
$existingHash = (Get-FileHash -LiteralPath $assets[0] -Algorithm SHA256).Hash
$refused = $false
try { $null = Split-CuePackage $zipFile 23 32 } catch { $refused = $true }
Assert-CueTest ($refused -and (Get-FileHash -LiteralPath $zipFile -Algorithm SHA256).Hash -ceq $originalHash -and
    (Get-FileHash -LiteralPath $assets[0] -Algorithm SHA256).Hash -ceq $existingHash) 'Failed split modified an existing output'
# 後方 Part の既存 File も出力開始前に検知する
$blockedZip = Join-Path $root 'blocked.zip'
[IO.File]::WriteAllBytes($blockedZip, [byte[]]::new(80))
$blockedPart = $blockedZip + '.003'
[IO.File]::WriteAllText($blockedPart, 'preserve')
$refused = $false
try { $null = Split-CuePackage $blockedZip 23 32 } catch { $refused = $true }
Assert-CueTest ($refused -and -not (Test-Path -LiteralPath ($blockedZip + '.001')) -and
    [IO.File]::ReadAllText($blockedPart) -ceq 'preserve') 'A late existing part was overwritten or produced partial output'
# Fixture に限り小さい結合 ZIP を作り、元 Entry を開けることを確認する
$joinedZip = Join-Path $root 'joined.zip'
$joinedStream = [IO.File]::Create($joinedZip)
try
{
    foreach ($asset in $assets)
    {
        $partStream = [IO.File]::OpenRead($asset)
        try { $partStream.CopyTo($joinedStream) } finally { $partStream.Dispose() }
    }
}
finally { $joinedStream.Dispose() }
$joinedArchive = [IO.Compression.ZipFile]::OpenRead($joinedZip)
try
{
    Assert-CueTest ($joinedArchive.Entries.Count -eq 1 -and $joinedArchive.Entries[0].FullName -ceq 'Dir/File.bin' -and
        $joinedArchive.Entries[0].Length -eq 4) 'Joining byte-split ZIP parts did not preserve the ZIP entry'
}
finally { $joinedArchive.Dispose() }
# 別の Fixture で個数上限を出力前に確認する
$tooMany = Join-Path $root 'too-many.zip'
[IO.File]::WriteAllBytes($tooMany, [byte[]]::new(513))
$refused = $false
try { $null = Split-CuePackage $tooMany 1 1 } catch { $refused = $true }
Assert-CueTest ($refused -and -not (Test-Path -LiteralPath ($tooMany + '.001'))) 'Excess part count was not rejected before output'
foreach ($limit in @(0, -1, 2147483648))
{
    $refused = $false
    try { $null = Split-CuePackage $zipFile $limit 32 } catch { $refused = $true }
    Assert-CueTest $refused 'Invalid part size was accepted'
}
Write-Output "Package contract checks passed: $checks"

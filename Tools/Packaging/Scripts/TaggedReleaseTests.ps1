param([string]$TestRoot)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageCommon.ps1')
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (-not $TestRoot) { $TestRoot = Join-Path $repositoryRoot 'out/validation/TaggedReleaseTests' }
$root = Join-Path (Assert-CuePlainPath $TestRoot) ([Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
$publisherScript = Join-Path $PSScriptRoot 'Publish-TaggedRelease.ps1'
$privateFile = Join-Path $root 'test-private.pem'
$publicFile = Join-Path $root 'test-public.pem'
$script:checks = 0
$global:CueTaggedReleaseTestMockState = @{
    Calls = [Collections.Generic.List[object]]::new()
    Exists = $false
    CreateExitCode = 0
    EditExitCode = 0
}

# Source の Version と Tag を対応させ、Test の都合で正本を書き換えない
$engineVersionFile = Join-Path $repositoryRoot 'Engine/Version.json'
$hubVersionFile = Join-Path $repositoryRoot 'Tools/CueHub/Version.json'
$versionFileHashes = @{
    $engineVersionFile = (Get-FileHash -LiteralPath $engineVersionFile -Algorithm SHA256).Hash
    $hubVersionFile = (Get-FileHash -LiteralPath $hubVersionFile -Algorithm SHA256).Hash
}
$engineVersion = (Read-CueVersion $engineVersionFile engine).version
$hubVersion = (Read-CueVersion $hubVersionFile hub).version

function Assert-CueReleaseTest([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw "Tagged release check failed: $Message" }
    $script:checks++
}

# どの呼出しも外部 CLI に委譲せず、許可した公開段階の引数だけを記録する
function gh
{
    $arguments = [string[]]$args
    if ($arguments.Count -lt 2 -or $arguments[0] -cne 'release' -or
        $arguments[1] -cnotin @('view', 'create', 'edit'))
    {
        throw 'Unexpected command reached the offline gh fixture'
    }
    $global:CueTaggedReleaseTestMockState.Calls.Add($arguments)
    switch -CaseSensitive ($arguments[1])
    {
        'view' {
            $global:LASTEXITCODE = if ($global:CueTaggedReleaseTestMockState.Exists) { 0 } else { 1 }
            if ($global:CueTaggedReleaseTestMockState.Exists) { Write-Output '{"tagName":"already-published"}' }
        }
        'create' { $global:LASTEXITCODE = $global:CueTaggedReleaseTestMockState.CreateExitCode }
        'edit' { $global:LASTEXITCODE = $global:CueTaggedReleaseTestMockState.EditExitCode }
    }
}

function Reset-CueReleaseMock
{
    $global:CueTaggedReleaseTestMockState.Calls.Clear()
    $global:CueTaggedReleaseTestMockState.Exists = $false
    $global:CueTaggedReleaseTestMockState.CreateExitCode = 0
    $global:CueTaggedReleaseTestMockState.EditExitCode = 0
}

# 小さい実 ZIP を作り、パッケージの検証と公開段階の検証を分離する
function New-CueReleaseFixture([string]$Product)
{
    $directory = Join-Path $root ($Product + '-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $version = if ($Product -ceq 'engine') { $engineVersion } else { $hubVersion }
    $name = if ($Product -ceq 'engine') { "CueEngine-$version-windows-x64.zip" } else { "CueEngine.Hub-$version-full.nupkg" }
    $package = Join-Path $directory $name
    $archive = [IO.Compression.ZipFile]::Open($package, [IO.Compression.ZipArchiveMode]::Create)
    try
    {
        $entry = $archive.CreateEntry('Fixture.txt')
        $stream = $entry.Open()
        try { $stream.Write([byte[]](1, 2, 3)) }
        finally { $stream.Dispose() }
    }
    finally { $archive.Dispose() }
    $manifest = [ordered]@{
        schemaVersion = 1
        product = $Product
        version = $version
        tag = "$Product-v$version"
    }
    if ($Product -ceq 'engine')
    {
        $manifest.platform = 'Windows'
        $manifest.architecture = 'x64'
        $manifest.configuration = 'Development'
        $manifest.buildId = '0123456789012345678901234567890123456789'
    }
    $manifest.packageFileName = $name
    $manifest.packageSha256 = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest.packageSize = (Get-Item -LiteralPath $package).Length
    if ($Product -ceq 'engine') { $manifest.expandedSize = 3 }
    else
    {
        $feed = Join-Path $directory 'releases.win.json'
        Write-CueJson $feed ([ordered]@{ Assets = @([ordered]@{ PackageId='CueEngine.Hub'; Version=$version; FileName=$name; Type='Full' }) })
        $manifest.feedFileName = 'releases.win.json'
        $manifest.feedSha256 = (Get-FileHash -LiteralPath $feed -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifest.feedSize = (Get-Item -LiteralPath $feed).Length
        [IO.File]::WriteAllBytes((Join-Path $directory 'CueEngine.Hub-win-Setup.exe'), [byte[]](77, 90))
    }
    $manifestFile = Join-Path $directory "$Product-release.json"
    Write-CueJson $manifestFile $manifest
    Protect-CueManifest $manifestFile $privateFile
    return [pscustomobject]@{ Product=$Product; Directory=$directory; Tag="$Product-v$version"; Version=$version; Package=$package; Manifest=$manifest; ManifestFile=$manifestFile }
}

function Invoke-CueReleaseFixture([object]$Fixture, [string]$Tag = '', [string]$PublicKeyFile = '')
{
    if (-not $Tag) { $Tag = $Fixture.Tag }
    if (-not $PublicKeyFile) { $PublicKeyFile = $publicFile }
    & $publisherScript -Tag $Tag -PackageDirectory $Fixture.Directory -Repository 'tochouseito/CueEngine' -PublisherPublicKeyFile $PublicKeyFile
}

# 検証失敗は gh の読取段階にすら達しないことを確認する
function Assert-CueRejectedBeforeGh([object]$Fixture, [string]$Message, [string]$Tag = '', [string]$PublicKeyFile = '')
{
    Reset-CueReleaseMock
    $rejected = $false
    try { $null = Invoke-CueReleaseFixture $Fixture $Tag $PublicKeyFile }
    catch { $rejected = $true }
    Assert-CueReleaseTest $rejected $Message
    Assert-CueReleaseTest ($global:CueTaggedReleaseTestMockState.Calls.Count -eq 0) "$Message reached gh"
}

$savedPath = $env:PATH
$savedToken = $env:GH_TOKEN
$savedOutput = $env:GITHUB_OUTPUT
$key = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
try
{
    # Function Mock が失われても実 GitHub CLI を探索できない状態にする
    $env:PATH = ''
    $env:GH_TOKEN = $null
    $env:GITHUB_OUTPUT = $null
    Assert-CueReleaseTest ((Get-Command gh).CommandType -eq [Management.Automation.CommandTypes]::Function) 'gh fixture was not selected'
    [IO.File]::WriteAllText($privateFile, $key.ExportPkcs8PrivateKeyPem())
    [IO.File]::WriteAllText($publicFile, $key.ExportSubjectPublicKeyInfoPem())

    foreach ($product in @('engine', 'hub'))
    {
        $fixture = New-CueReleaseFixture $product
        Reset-CueReleaseMock
        $null = Invoke-CueReleaseFixture $fixture
        Assert-CueReleaseTest ($global:CueTaggedReleaseTestMockState.Calls.Count -eq 3) "$product publication stage count"
        Assert-CueReleaseTest ($global:CueTaggedReleaseTestMockState.Calls[0][1] -ceq 'view' -and
            $global:CueTaggedReleaseTestMockState.Calls[1][1] -ceq 'create' -and $global:CueTaggedReleaseTestMockState.Calls[2][1] -ceq 'edit') "$product Draft then Publish order"
        $create = $global:CueTaggedReleaseTestMockState.Calls[1]
        $edit = $global:CueTaggedReleaseTestMockState.Calls[2]
        Assert-CueReleaseTest ($create -ccontains '--draft' -and $create -ccontains '--verify-tag' -and $edit -ccontains '--draft=false') "$product draft boundary"
        Assert-CueReleaseTest ((@($create | Where-Object { $_ -ceq ($fixture.ManifestFile + '.sig') })).Count -eq 1 -and $create -cnotcontains '.sig') "$product signature asset grouping"
        Assert-CueReleaseTest ($create -ccontains $fixture.Package -and $create -ccontains $fixture.ManifestFile) "$product package assets"
        $preview = $fixture.Version.Split('+')[0].Contains('-')
        Assert-CueReleaseTest (($create -ccontains '--prerelease') -eq $preview -and
            $edit -ccontains ('--prerelease=' + $preview.ToString().ToLowerInvariant())) "$product preview flag"
        $latest = ($product -ceq 'engine' -and -not $preview).ToString().ToLowerInvariant()
        Assert-CueReleaseTest ($edit -ccontains ('--latest=' + $latest)) "$product latest flag"
        if ($product -ceq 'hub')
        {
            Assert-CueReleaseTest ($create -ccontains (Join-Path $fixture.Directory 'releases.win.json') -and
                $create -ccontains (Join-Path $fixture.Directory 'CueEngine.Hub-win-Setup.exe')) 'Hub feed and installer assets'
            Assert-CueReleaseTest ($edit -ccontains '--latest=false') 'Hub must not become the Engine latest release'
        }

        Reset-CueReleaseMock
        $global:CueTaggedReleaseTestMockState.Exists = $true
        $rejected = $false
        try { $null = Invoke-CueReleaseFixture $fixture } catch { $rejected = $true }
        Assert-CueReleaseTest ($rejected -and $global:CueTaggedReleaseTestMockState.Calls.Count -eq 1 -and $global:CueTaggedReleaseTestMockState.Calls[0][1] -ceq 'view') "$product existing release protection"

        Reset-CueReleaseMock
        $global:CueTaggedReleaseTestMockState.CreateExitCode = 1
        $rejected = $false
        try { $null = Invoke-CueReleaseFixture $fixture } catch { $rejected = $true }
        Assert-CueReleaseTest ($rejected -and $global:CueTaggedReleaseTestMockState.Calls.Count -eq 2) "$product failed draft did not publish"

        Reset-CueReleaseMock
        $global:CueTaggedReleaseTestMockState.EditExitCode = 1
        $rejected = $false
        try { $null = Invoke-CueReleaseFixture $fixture } catch { $rejected = $true }
        Assert-CueReleaseTest ($rejected -and $global:CueTaggedReleaseTestMockState.Calls.Count -eq 3) "$product public completion failure reported"

        $bad = New-CueReleaseFixture $product
        [IO.File]::WriteAllText($bad.ManifestFile + '.sig', 'invalid-signature')
        Assert-CueRejectedBeforeGh $bad "$product invalid signature"

        $bad = New-CueReleaseFixture $product
        Remove-Item -LiteralPath ($bad.ManifestFile + '.sig')
        Assert-CueRejectedBeforeGh $bad "$product missing signature asset"

        $bad = New-CueReleaseFixture $product
        [IO.File]::AppendAllText($bad.ManifestFile, ' ')
        Assert-CueRejectedBeforeGh $bad "$product signed content tampering"

        $bad = New-CueReleaseFixture $product
        [IO.File]::AppendAllText($bad.Package, 'changed')
        Assert-CueRejectedBeforeGh $bad "$product package hash mismatch"

        $bad = New-CueReleaseFixture $product
        $bad.Manifest.unexpected = 'extra'
        Write-CueJson $bad.ManifestFile $bad.Manifest
        Protect-CueManifest $bad.ManifestFile $privateFile
        Assert-CueRejectedBeforeGh $bad "$product unexpected schema field"

        $bad = New-CueReleaseFixture $product
        $bad.Manifest.version = '999.0.0'
        # File 名の不一致による拒否に頼らず、署名済み Version と Tag の照合を検証する
        $renamedPackage = if ($product -ceq 'engine') { 'CueEngine-999.0.0-windows-x64.zip' } else { 'CueEngine.Hub-999.0.0-full.nupkg' }
        $renamedPath = Join-Path $bad.Directory $renamedPackage
        Move-Item -LiteralPath $bad.Package -Destination $renamedPath
        $bad.Package = $renamedPath
        $bad.Manifest.packageFileName = $renamedPackage
        Write-CueJson $bad.ManifestFile $bad.Manifest
        Protect-CueManifest $bad.ManifestFile $privateFile
        Assert-CueRejectedBeforeGh $bad "$product signed Version differs from Tag"

        $bad = New-CueReleaseFixture $product
        Assert-CueRejectedBeforeGh $bad "$product Tag mismatch" ($product + '-v999.0.0')
    }

    $missingFeed = New-CueReleaseFixture hub
    Remove-Item -LiteralPath (Join-Path $missingFeed.Directory 'releases.win.json')
    Assert-CueRejectedBeforeGh $missingFeed 'Hub missing feed'
    $changedFeed = New-CueReleaseFixture hub
    [IO.File]::AppendAllText((Join-Path $changedFeed.Directory 'releases.win.json'), ' ')
    Assert-CueRejectedBeforeGh $changedFeed 'Hub feed hash mismatch'

    $wrongPublisher = [Security.Cryptography.ECDsa]::Create([Security.Cryptography.ECCurve+NamedCurves]::nistP256)
    try
    {
        $otherPublicFile = Join-Path $root 'wrong-public.pem'
        [IO.File]::WriteAllText($otherPublicFile, $wrongPublisher.ExportSubjectPublicKeyInfoPem())
        $bad = New-CueReleaseFixture engine
        Assert-CueRejectedBeforeGh $bad 'Untrusted publisher key' '' $otherPublicFile
    }
    finally { $wrongPublisher.Dispose() }

    foreach ($pair in $versionFileHashes.GetEnumerator())
    {
        Assert-CueReleaseTest ((Get-FileHash -LiteralPath $pair.Key -Algorithm SHA256).Hash -ceq $pair.Value) 'Source Version changed during fixture tests'
    }
    Write-Output "Tagged release offline checks passed: $script:checks"
    Write-Output "Fixture output: $root"
}
finally
{
    $key.Dispose()
    if (Test-Path -LiteralPath $privateFile) { Remove-Item -LiteralPath $privateFile -Force }
    $env:PATH = $savedPath
    $env:GH_TOKEN = $savedToken
    $env:GITHUB_OUTPUT = $savedOutput
    Remove-Item Function:gh
    Remove-Variable -Name CueTaggedReleaseTestMockState -Scope Global
}

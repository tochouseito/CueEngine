param(
    [Parameter(Mandatory)] [string]$OutputDirectory,
    [ValidateRange(1, 180)] [int]$Seconds = 3,
    [ValidateSet('all', 'worker', 'latency', 'latency2')] [string]$Cases = 'all',
    [switch]$Collector,
    [switch]$Stacks,
    [switch]$HardwareScheduling,
    [switch]$RequireForeground,
    [switch]$DeferredMarkers,
    [switch]$PresentationOnly,
    [switch]$Triggered,
    [ValidateRange(0.001, 10000)] [double]$RenderTriggerMs = 20,
    [ValidateRange(0.001, 10000)] [double]$GpuTriggerMs = 5
)

$ErrorActionPreference = 'Stop'
if ($Triggered -and ($Cases -ne 'latency2' -or $Seconds -lt 8))
{
    throw 'Triggered は latency2、8 秒以上を指定してください。'
}
if (!$Triggered -and $Seconds -gt 20) { throw '通常記録は 20 秒以内です。' }
if ($Triggered) { $DeferredMarkers = $true }
if ($HardwareScheduling -and $PresentationOnly)
{
    throw 'HardwareScheduling と PresentationOnly は同時指定できません。'
}
$repositoryPath = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
$allowedRoot = Join-Path $repositoryPath 'out'
if (!$outputPath.StartsWith($allowedRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase))
{
    throw '出力先は Repository の out 配下を指定してください。'
}
$xperf = 'C:/Program Files (x86)/Windows Kits/10/Windows Performance Toolkit/xperf.exe'
$probe = Join-Path $repositoryPath 'out/build/windows-vs2026/Engine/Tests/Development/CueSpikeTimingProbe.exe'
$configurationFile = Join-Path $outputPath 'Capture.json'
$readyFile = Join-Path $outputPath 'Ready.json'
$stopFile = Join-Path $outputPath 'Stop.flag'
$doneFile = Join-Path $outputPath 'Done.json'

# 管理者側は記録制御だけを行い、Probe は通常権限の呼出元から起動する
if ($Collector)
{
    $kernelStarted = $false
    $userStarted = $false
    $markersStarted = $false
    $markerSession = $null
    $status = @{ KernelStarted=$false; UserStarted=$false; KernelStopped=$false; UserStopped=$false; Merged=$false; Error='' }
    try
    {
        $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
        if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw '管理者権限がありません。' }
        $configuration = Get-Content -LiteralPath $configurationFile -Raw | ConvertFrom-Json
        $session = [string]$configuration.Session
        $markerSession = $session + '-Markers'
        $isTriggered = [bool]$configuration.Triggered
        $freezeFile = Join-Path $outputPath 'on-q2-freeze.flag'
        $frozenFile = Join-Path $outputPath 'on-q2-frozen.flag'
        if ($session -notmatch '^CueSpike-[0-9a-f]{32}$') { throw 'Session 名が不正です。' }
        $existing = (& $xperf -loggers | Out-String)
        if ($existing -match '(?m)^Logger Name\s*:\s*NT Kernel Logger\s*$') { throw '既存の Kernel Session を保全するため停止します。' }
        $kernelArguments = @('-on','PROC_THREAD+LOADER+CSWITCH+DISPATCHER+DPC+INTERRUPT')
        if ($configuration.Stacks) { $kernelArguments += @('-stackwalk','CSwitch+ReadyThread') }
        $kernelArguments += @('-ClockType','PerfCounter','-f',(Join-Path $outputPath 'Kernel.etl'),'-buffersize','256','-minbuffers','64','-maxbuffers','256')
        if ($isTriggered)
        {
            # Buffering は開始時に File を指定せず、停止前の flush で保存先を指定する
            $fileIndex = $kernelArguments.IndexOf('-f')
            $kernelArguments = @($kernelArguments[0..($fileIndex - 1)]) + @($kernelArguments[($fileIndex + 2)..($kernelArguments.Count - 1)])
            $kernelArguments += '-Buffering'
            $kernelArguments[$kernelArguments.IndexOf('-on') + 1] += '+HARD_FAULTS'
            $kernelArguments[$kernelArguments.IndexOf('-minbuffers') + 1] = '512'
            $kernelArguments[$kernelArguments.IndexOf('-maxbuffers') + 1] = '512'
        }
        $kernelArguments | ConvertTo-Json | Set-Content (Join-Path $outputPath 'KernelArguments.json') -Encoding utf8
        & $xperf @kernelArguments *> (Join-Path $outputPath 'KernelStart.log')
        if ($LASTEXITCODE -ne 0) { throw ('Kernel start exit=' + $LASTEXITCODE) }
        $kernelStarted = $true
        $status.KernelStarted = $true
        # modern Provider 自身の Base と GPU Scheduler を含める。legacy DX の指定だけでは Queue / DMA を得られなかった
        $providers = '9d765397-a1ac-4d0f-9ae5-0fa779d47f90:0x1:5+802ec45a-1e99-4b83-9920-87c98277ba9d:0x408841:5+65cd4c8a-0848-4583-92a0-31c0fbaf00c0:0x2f:5+ca11c036-0102-4a2d-a6ad-f03cfed5d3c9:0xf:5+9e9bba3c-2e38-40cb-99f4-9e8281425164:0xffff:5'
        if ($configuration.HardwareScheduling) { $providers = $providers.Replace(':0x408841:5', ':0x4408841:5') }
        if ($configuration.PresentationOnly)
        {
            # Base / GPU Scheduler の大量 Event を外し、表示経路と Kernel Scheduling の再現を優先する
            $providers = '9d765397-a1ac-4d0f-9ae5-0fa779d47f90:0x1:5+802ec45a-1e99-4b83-9920-87c98277ba9d:0x8000840:5+ca11c036-0102-4a2d-a6ad-f03cfed5d3c9:0xf:5+9e9bba3c-2e38-40cb-99f4-9e8281425164:0xffff:5'
        }
        if ($isTriggered) { $providers = $providers.Replace('9d765397-a1ac-4d0f-9ae5-0fa779d47f90:0x1:5+', '') }
        Set-Content -LiteralPath (Join-Path $outputPath 'Providers.txt') -Value $providers -Encoding ascii
        $userArguments = @('-start',$session,'-on',$providers,'-ClockType','PerfCounter','-f',
                           (Join-Path $outputPath 'UserGpu.etl'),'-buffersize','256','-minbuffers','64','-maxbuffers','256')
        if ($isTriggered)
        {
            $fileIndex = $userArguments.IndexOf('-f')
            $userArguments = @($userArguments[0..($fileIndex - 1)]) + @($userArguments[($fileIndex + 2)..($userArguments.Count - 1)])
            $userArguments += '-Buffering'
            $userArguments[$userArguments.IndexOf('-minbuffers') + 1] = '1024'
            $userArguments[$userArguments.IndexOf('-maxbuffers') + 1] = '1024'
        }
        & $xperf @userArguments *> (Join-Path $outputPath 'UserStart.log')
        if ($LASTEXITCODE -ne 0) { throw ('User start exit=' + $LASTEXITCODE) }
        $userStarted = $true
        $status.UserStarted = $true
        if ($isTriggered)
        {
            # 停止後に出す全 CPU Marker は別の逐次 Session に残し、GPU 循環 Buffer の上書きを避ける
            & $xperf -start $markerSession -on '9d765397-a1ac-4d0f-9ae5-0fa779d47f90:0x1:5' -ClockType PerfCounter -f (Join-Path $outputPath 'Markers.etl') -buffersize 256 -minbuffers 64 -maxbuffers 256 *> (Join-Path $outputPath 'MarkerStart.log')
            if ($LASTEXITCODE -ne 0) { throw ('Marker start exit=' + $LASTEXITCODE) }
            $markersStarted = $true
        }
        & $xperf -capturestate $session '802ec45a-1e99-4b83-9920-87c98277ba9d:0x90ffff:5' *> (Join-Path $outputPath 'CaptureState.log')
        if ($LASTEXITCODE -ne 0) { throw ('CaptureState exit=' + $LASTEXITCODE) }
        @{ Ready=$true; Session=$session } | ConvertTo-Json | Set-Content -LiteralPath $readyFile -Encoding utf8
        $deadline = [DateTime]::UtcNow.AddSeconds(180 + [int]$configuration.Seconds)
        while (!(Test-Path -LiteralPath $stopFile) -and [DateTime]::UtcNow -lt $deadline)
        {
            if ($isTriggered -and $kernelStarted -and (Test-Path -LiteralPath $freezeFile))
            {
                & $xperf -capturestate $session '802ec45a-1e99-4b83-9920-87c98277ba9d:0x490ffff:5' *> (Join-Path $outputPath 'FinalCaptureState.log')
                if ($LASTEXITCODE -ne 0) { throw 'Final CaptureState に失敗しました。' }
                & $xperf -flush 'NT Kernel Logger' -f (Join-Path $outputPath 'Kernel.etl') *> (Join-Path $outputPath 'KernelSave.log')
                if ($LASTEXITCODE -ne 0) { throw 'Kernel Snapshot に失敗しました。' }
                $status.KernelSaved = $true
                & $xperf -flush $session -f (Join-Path $outputPath 'UserGpu.etl') *> (Join-Path $outputPath 'UserSave.log')
                if ($LASTEXITCODE -ne 0) { throw 'GPU Snapshot に失敗しました。' }
                $status.UserSaved = $true
                & $xperf -stop $session *> (Join-Path $outputPath 'UserStop.log')
                if ($LASTEXITCODE -ne 0) { throw 'GPU Session 停止に失敗しました。' }
                $status.UserStopped = $true
                $userStarted = $false
                & $xperf -stop 'NT Kernel Logger' *> (Join-Path $outputPath 'KernelStop.log')
                if ($LASTEXITCODE -ne 0) { throw 'Kernel Session 停止に失敗しました。' }
                $status.KernelStopped = $true
                $kernelStarted = $false
                Set-Content -LiteralPath $frozenFile -Value 'frozen' -Encoding ascii
            }
            Start-Sleep -Milliseconds 100
        }
        if (!(Test-Path -LiteralPath $stopFile)) { throw '呼出側の停止要求を待つ期限を超えました。' }
    }
    catch { $status.Error = $_.ToString() }
    finally
    {
        if ($userStarted)
        {
            & $xperf -stop $session *> (Join-Path $outputPath 'UserStop.log')
            $status.UserStopped = $LASTEXITCODE -eq 0
        }
        if ($kernelStarted)
        {
            & $xperf -stop 'NT Kernel Logger' *> (Join-Path $outputPath 'KernelStop.log')
            $status.KernelStopped = $LASTEXITCODE -eq 0
        }
        if ($markersStarted)
        {
            & $xperf -stop $markerSession *> (Join-Path $outputPath 'MarkerStop.log')
            $status.MarkersStopped = $LASTEXITCODE -eq 0
        }
        if ($status.UserStopped -and $status.KernelStopped -and (!$markersStarted -or $status.MarkersStopped))
        {
            $mergeFiles = @((Join-Path $outputPath 'Kernel.etl'), (Join-Path $outputPath 'UserGpu.etl'))
            if ($markersStarted) { $mergeFiles += Join-Path $outputPath 'Markers.etl' }
            & $xperf -merge @mergeFiles (Join-Path $outputPath 'Merged.etl') -compress *> (Join-Path $outputPath 'Merge.log')
            $status.Merged = $LASTEXITCODE -eq 0
        }
        $status | ConvertTo-Json | Set-Content -LiteralPath $doneFile -Encoding utf8
    }
    exit
}

if (!(Test-Path -LiteralPath $probe)) { throw 'Trace ON の Development Probe を Build してください。' }
if (Test-Path -LiteralPath $outputPath)
{
    if (@(Get-ChildItem -LiteralPath $outputPath -Force).Count -ne 0) { throw '既存記録を保全するため空の出力先を指定してください。' }
}
else { New-Item -ItemType Directory -Path $outputPath | Out-Null }
@{ Session=('CueSpike-' + [Guid]::NewGuid().ToString('N')); Seconds=$Seconds; Cases=$Cases; Probe=$probe; Stacks=[bool]$Stacks; HardwareScheduling=[bool]$HardwareScheduling; RequireForeground=[bool]$RequireForeground; DeferredMarkers=[bool]$DeferredMarkers; PresentationOnly=[bool]$PresentationOnly; Triggered=[bool]$Triggered; RenderTriggerMs=$RenderTriggerMs; GpuTriggerMs=$GpuTriggerMs } | ConvertTo-Json | Set-Content -LiteralPath $configurationFile -Encoding utf8
$arguments = @('-NoProfile','-File',('"' + $PSCommandPath + '"'),'-OutputDirectory',('"' + $outputPath + '"'),'-Collector')
$process = Start-Process -FilePath (Get-Command pwsh).Source -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru
try
{
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while (!(Test-Path -LiteralPath $readyFile))
    {
        if ($process.HasExited -or (Test-Path -LiteralPath $doneFile)) { throw 'Collector 起動に失敗しました。Done.json を確認してください。' }
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Collector の準備待ちが期限を超えました。' }
        Start-Sleep -Milliseconds 100
    }
    $measurements = @(@('off',2,0,1), @('on-q1',2,1,1))
    if ($Cases -eq 'latency') { $measurements = @(@('on-q1',2,1,1), @('on-q2',2,1,2)) }
    if ($Cases -eq 'latency2') { $measurements = @(,@('on-q2',2,1,2)) }
    if ($Cases -eq 'all') { $measurements += ,@('single-off',1,0,1) }
    foreach ($case in $measurements)
    {
        Write-Output ('Recording ' + $case[0])
        $prefix = Join-Path $outputPath $case[0]
        & $probe $case[1] $case[2] $case[3] 0 $Seconds $prefix 1 1 ([int][bool]$RequireForeground) ([int][bool]$DeferredMarkers) ([int][bool]$Triggered) $RenderTriggerMs $GpuTriggerMs 2>&1 | Tee-Object -FilePath ($prefix + '.log')
        if ($LASTEXITCODE -ne 0) { throw ('Probe exit=' + $LASTEXITCODE) }
    }
}
finally
{
    Set-Content -LiteralPath $stopFile -Value 'stop' -Encoding ascii
    if (!$process.WaitForExit(45000)) { Write-Warning 'Collector が停止・Merge を続行中です。Done.json を確認してください。' }
}
if (!(Test-Path -LiteralPath $doneFile)) { throw 'Collector の終了記録がありません。' }
$result = Get-Content -LiteralPath $doneFile -Raw | ConvertFrom-Json
$result | Format-List
if (!$result.Merged -or $result.Error) { throw 'ETW 記録に失敗しました。ログを確認してください。' }

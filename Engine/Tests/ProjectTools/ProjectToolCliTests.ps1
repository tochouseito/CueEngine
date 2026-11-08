param(
    [Parameter(Mandatory)][string]$Tool,
    [Parameter(Mandatory)][string]$TestRoot,
    [Parameter(Mandatory)][string]$EngineVersion
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

# 呼出しごとに所有 Root を分け、既存の Project や他 Test の成果物を変更しない
$taskRoot = Join-Path ([IO.Path]::GetFullPath($TestRoot)) ([Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($taskRoot) | Out-Null
$taskProject = Join-Path $taskRoot '日本語 Project'
$taskOutput = & $Tool create --directory $taskProject --name '回転 Game' --application-name TestGame `
    --template RotateCube --engine-version $EngineVersion
if ($LASTEXITCODE -ne 0) { throw "Project creation failed: $taskOutput" }
$taskCreated = $taskOutput | ConvertFrom-Json
if ($taskCreated.schemaVersion -ne 1 -or !$taskCreated.success -or $taskCreated.engineVersion -ne $EngineVersion) {
    throw 'Creation response did not match the JSON contract'
}
if (!(Test-Path -LiteralPath $taskCreated.projectFile -PathType Leaf)) { throw 'Created Project is missing' }
$taskBaseline = [IO.File]::ReadAllBytes($taskCreated.projectFile)

$taskOutput = & $Tool validate --project $taskCreated.projectFile
if ($LASTEXITCODE -ne 0) { throw "Project validation failed: $taskOutput" }
$taskValidated = $taskOutput | ConvertFrom-Json
if (!$taskValidated.success -or $taskValidated.projectId -ne $taskCreated.projectId) {
    throw 'Validation changed the Project identity'
}

# 一覧登録の Metadata 読取りは別 Engine の指定も返す。作成した正本は変更しない
$taskOtherEngineProject = Join-Path $taskProject 'OtherEngine.cueproj'
$taskOtherEngineText = [IO.File]::ReadAllText($taskCreated.projectFile).Replace("engineVersion=$EngineVersion", 'engineVersion=9.9.9')
[IO.File]::WriteAllText($taskOtherEngineProject, $taskOtherEngineText, [Text.UTF8Encoding]::new($false))
$taskOutput = & $Tool validate --project $taskOtherEngineProject
if ($LASTEXITCODE -ne 0) { throw "Other Engine Project metadata validation failed: $taskOutput" }
$taskOtherEngineValidated = $taskOutput | ConvertFrom-Json
if (!$taskOtherEngineValidated.success -or $taskOtherEngineValidated.engineVersion -ne '9.9.9') {
    throw 'Validation did not return the requested other Engine version'
}

$taskOutput = & $Tool create --directory $taskProject --name Game --application-name TestGame `
    --template RotateCube --engine-version $EngineVersion
if ($LASTEXITCODE -eq 0 -or ($taskOutput | ConvertFrom-Json).success) { throw 'Existing Project was overwritten' }
if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($taskCreated.projectFile)) -ne [Convert]::ToBase64String($taskBaseline)) {
    throw 'Existing Project contents changed'
}

$taskOutput = & $Tool validate --project $taskCreated.projectFile --unknown Value
if ($LASTEXITCODE -eq 0 -or ($taskOutput | ConvertFrom-Json).success) { throw 'Unknown arguments were accepted' }

$taskOutput = & $Tool create --directory (Join-Path $taskRoot Invalid) --name Game --application-name Invalid `
    --template RotateCube --engine-version '../bad'
if ($LASTEXITCODE -eq 0 -or ($taskOutput | ConvertFrom-Json).success) { throw 'Invalid Engine version was accepted' }
Write-Output 'Project CLI JSON / Unicode / identity / overwrite / argument checks passed'

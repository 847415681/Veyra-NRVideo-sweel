param(
    [string]$AppDirectory = $PSScriptRoot,
    [ValidateSet('Compatible','Legacy')][string]$Variant = 'Compatible',
    [string]$ProfileSource = '',
    [string]$OutputDirectory = '',
    [ValidateRange(15,280)][int]$DurationSeconds = 180
)
$ErrorActionPreference='Stop'
function File-Sha256([string]$Path) {
    $stream=[IO.File]::OpenRead($Path);$hasher=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','') }
    finally { $hasher.Dispose();$stream.Dispose() }
}
$app=[IO.Path]::GetFullPath($AppDirectory);$player=Join-Path $app 'veyra_qml_ui.exe'
if (-not (Test-Path -LiteralPath $player -PathType Leaf)) { throw 'Extract the patch into a Veyra 2.0.6 portable copy first.' }
if (@(Get-Process -Name 'veyra_qml_ui','Veyra' -ErrorAction SilentlyContinue).Count -gt 0) { throw 'Close Veyra first. Close OBS before testing the same capture card.' }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory=Join-Path $app ('logs/bmd-'+$Variant+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$output=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Output already exists; evidence will not be overwritten.' }
$profile=Join-Path $output 'private-profile';$tmp=Join-Path $output 'tmp'
$null=New-Item -ItemType Directory -Path $profile,$tmp -Force
$copied=@()
if (-not [string]::IsNullOrWhiteSpace($ProfileSource)) {
    $fromProfile=[IO.Path]::GetFullPath($ProfileSource)
    # Only preferences, never PSN/Xbox credentials or whole user directories.
    foreach ($name in @('capture-preferences.v1','chain-session.v1','chain-session.v1.vfg','qml-preferences.v1.json','ui-session.v1')) {
        $from=Join-Path $fromProfile $name
        if (Test-Path -LiteralPath $from -PathType Leaf) {
            $digest=File-Sha256 $from;Copy-Item -LiteralPath $from -Destination (Join-Path $profile $name)
            $copied += [pscustomobject]@{path=$from;sha256=$digest}
        }
    }
}
$names=@('TEMP','TMP','CUDA_CACHE_PATH','QML_DISK_CACHE_PATH','VEYRA_LOG_FILE','VEYRA_BMD_DSHOW_LEGACY')
$saved=@{};foreach ($name in $names) { $saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process') }
$arguments=@('--page','pro','--data-dir',$profile,'--exit-after',($DurationSeconds*1000).ToString())
if (@($arguments | Where-Object { $_.Contains('"') -or $_.EndsWith('\') }).Count -gt 0) { throw 'Unsupported argument quoting.' }
$quoted=@($arguments | ForEach-Object { '"'+$_+'"' })
$result=125;$forcedStop=$false
try {
    $env:TEMP=$tmp;$env:TMP=$tmp;$env:CUDA_CACHE_PATH=Join-Path $tmp 'cuda-cache';$env:QML_DISK_CACHE_PATH=Join-Path $tmp 'qml-cache'
    $env:VEYRA_LOG_FILE=Join-Path $output 'engine.log'
    if ($Variant -eq 'Legacy') { $env:VEYRA_BMD_DSHOW_LEGACY='1' }
    else { [Environment]::SetEnvironmentVariable('VEYRA_BMD_DSHOW_LEGACY',$null,'Process') }
    Write-Host ('BMD '+$Variant+': select the same device and exact input resolution/FPS/pixel format as OBS. Disable enhancements for the first check; then test your usual settings.')
    # Visible player is intentional: this launcher is an interactive field test.
    $process=Start-Process -FilePath $player -ArgumentList $quoted -WorkingDirectory $app -PassThru -WindowStyle Normal `
        -RedirectStandardOutput (Join-Path $output 'console.log') -RedirectStandardError (Join-Path $output 'stderr.log')
    $null=$process.Handle
    if (-not $process.WaitForExit(($DurationSeconds+20)*1000)) { $forcedStop=$true;Stop-Process -Id $process.Id -Force;$process.WaitForExit() }
    $process.WaitForExit();$process.Refresh();$result=$process.ExitCode
    if ($forcedStop) { $result=124 } elseif ($null -eq $result) { $result=125 }
} finally { foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') } }
$unchanged=$true;foreach ($item in $copied) { if (-not (Test-Path -LiteralPath $item.path -PathType Leaf) -or (File-Sha256 $item.path) -ne $item.sha256) { $unchanged=$false } }
$receipt=[pscustomobject]@{exit=$result;forcedStop=$forcedStop;variant=$Variant;executableSha256=(File-Sha256 $player);privateProfile=$profile;sourceProfileUnchanged=$unchanged;durationLimitSeconds=$DurationSeconds;realBmdHardwareValidated=$false}
$receipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'test-receipt.json') -Encoding UTF8
Add-Type -AssemblyName System.IO.Compression;Add-Type -AssemblyName System.IO.Compression.FileSystem
$stream=[IO.File]::Open(($output+'.zip'),[IO.FileMode]::CreateNew);$zip=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create)
try { foreach ($file in @(Get-ChildItem -LiteralPath $output -File | Where-Object { $_.Extension -in '.log','.json' })) { $null=[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$file.FullName,$file.Name,[IO.Compression.CompressionLevel]::Optimal) } }
finally { $zip.Dispose();$stream.Dispose() }
Write-Host ('Send this evidence: '+$output+'.zip')
if (-not $unchanged) { throw 'Original preferences changed during comparison.' }
exit $result

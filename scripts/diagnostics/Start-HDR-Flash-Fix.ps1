param(
    [string]$AppDirectory = $PSScriptRoot,
    [string]$ProfileSource = '',
    [string]$OutputDirectory = '',
    [string]$Source = '',
    [ValidateSet('Compatible','Direct')][string]$Variant = 'Compatible',
    [ValidateRange(0,3600)][int]$DurationSeconds = 0
)
$ErrorActionPreference = 'Stop'
function File-Sha256([string]$Path) {
    $stream=[IO.File]::OpenRead($Path);$hasher=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','') }
    finally { $hasher.Dispose();$stream.Dispose() }
}
$app = [IO.Path]::GetFullPath($AppDirectory)
$player = Join-Path $app 'veyra_qml_ui.exe'
foreach ($required in @($player,(Join-Path $app 'qt.conf'),(Join-Path $app 'Qt6Core.dll'),(Join-Path $app 'qml/Veyra/Main.qml'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw 'Place this overlay in a complete Veyra 2.0.6 NVIDIA folder first. Back up the original EXE.' }
}
$manifest = Join-Path $app 'hdr-flash-fix-manifest.json'
if (Test-Path -LiteralPath $manifest -PathType Leaf) {
    $expected = (Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).executableSha256
    if ((File-Sha256 $player) -ne $expected) { throw 'The EXE is not the matching HDR flash candidate. Runtime DLL replacement remains allowed.' }
}
if (@(Get-Process -Name 'veyra_qml_ui','veyra-flash-diagnostic','veyra-hdr-flash-fix','flash-pixel-probe' -ErrorAction SilentlyContinue).Count -gt 0) { throw 'Close other Veyra players before this test.' }
if ([string]::IsNullOrWhiteSpace($ProfileSource)) { $ProfileSource = Join-Path $app 'runtime_local/user-data-2.0.0' }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $app ('logs/hdr-flash-' + $Variant + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Output already exists; refusing to overwrite evidence.' }
$profile = Join-Path $output 'private-profile';$tmp = Join-Path $output 'tmp'
$null = New-Item -ItemType Directory -Path $profile,$tmp -Force
$copied = @()
foreach ($name in @('chain-session.v1','chain-session.v1.vfg','qml-preferences.v1.json','ui-session.v1','presets.v1','node-anchors.json')) {
    $from = Join-Path $ProfileSource $name
    if (Test-Path -LiteralPath $from -PathType Leaf) {
        $digest = File-Sha256 $from
        Copy-Item -LiteralPath $from -Destination (Join-Path $profile $name)
        $copied += [pscustomobject]@{path=$from;sha256=$digest}
    }
}
$prefsPath=Join-Path $profile 'qml-preferences.v1.json'
$prefs=if(Test-Path -LiteralPath $prefsPath -PathType Leaf){Get-Content -LiteralPath $prefsPath -Raw | ConvertFrom-Json}else{'{}' | ConvertFrom-Json}
$prefs | Add-Member -NotePropertyName 'hdrFgCompatibility' -NotePropertyValue ($Variant -eq 'Compatible') -Force
[IO.File]::WriteAllText($prefsPath,($prefs | ConvertTo-Json -Depth 40),(New-Object Text.UTF8Encoding($false)))
$envNames = @('TEMP','TMP','CUDA_CACHE_PATH','VEYRA_LOG_FILE','VEYRA_TEST_HDR_PRESENT_COPY')
$savedEnv = @{}
foreach ($name in $envNames) { $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
$arguments = @('--data-dir',$profile,'--page','pro')
if ($DurationSeconds -gt 0) { $arguments += @('--exit-after',($DurationSeconds*1000).ToString()) }
if (-not [string]::IsNullOrWhiteSpace($Source)) { $arguments += $Source }
if (@($arguments | Where-Object { $_.Contains('"') -or $_.EndsWith('\') }).Count -gt 0) { throw 'Unsupported argument quoting.' }
$quoted = @($arguments | ForEach-Object { '"' + $_ + '"' })
$result=1;$forcedStop=$false
try {
    $env:TEMP=$tmp;$env:TMP=$tmp;$env:CUDA_CACHE_PATH=Join-Path $tmp 'cuda-cache'
    $env:VEYRA_LOG_FILE=Join-Path $output 'engine.log'
    # Exercise the same saved setting as Settings > Compatibility, with no
    # environment override. Only this private configuration is changed.
    [Environment]::SetEnvironmentVariable('VEYRA_TEST_HDR_PRESENT_COPY',$null,'Process')
    # Keep the normal filename/path and QML disk-cache setting. Changing the
    # filename could bypass NVIDIA App/RTSS per-program profiles.
    Write-Host ('Variant: {0}. Use the same source/settings in both runs; close the player when finished.' -f $Variant)
    $process=Start-Process -FilePath $player -ArgumentList $quoted -WorkingDirectory $app -PassThru -WindowStyle Normal `
        -RedirectStandardOutput (Join-Path $output 'console.log') -RedirectStandardError (Join-Path $output 'stderr.log')
    $null=$process.Handle
    if ($DurationSeconds -eq 0) { $process.WaitForExit() }
    elseif (-not $process.WaitForExit(($DurationSeconds+35)*1000)) { $forcedStop=$true;Stop-Process -Id $process.Id -Force;$process.WaitForExit() }
    $process.WaitForExit();$process.Refresh();$result=$process.ExitCode
    if ($forcedStop) { $result=124 } elseif ($null -eq $result) { $result=125 }
} finally {
    foreach ($name in $envNames) { [Environment]::SetEnvironmentVariable($name,$savedEnv[$name],'Process') }
}
$unchanged=$true
foreach ($item in $copied) {
    if (-not (Test-Path -LiteralPath $item.path -PathType Leaf) -or (File-Sha256 $item.path) -ne $item.sha256) { $unchanged=$false }
}
$receipt=[pscustomobject]@{
    exit=$result;forcedStop=$forcedStop;variant=$Variant;executable=$player;fileName='veyra_qml_ui.exe';
    executableSha256=(File-Sha256 $player);
    privateProfile=$profile;sourceProfileUnchanged=$unchanged;durationLimitSeconds=$DurationSeconds;
    qmlDiskCacheInherited=[Environment]::GetEnvironmentVariable('QML_DISABLE_DISK_CACHE','Process');
    pixelProbeIncluded=$false
}
$receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'test-receipt.json') -Encoding UTF8
$evidence=@(Get-ChildItem -LiteralPath $output -File | Where-Object { $_.Extension -in '.log','.json' } | Select-Object -ExpandProperty FullName)
if ($evidence.Count -gt 0) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipStream=[IO.File]::Open(($output+'.zip'),[IO.FileMode]::CreateNew)
    $zip=[IO.Compression.ZipArchive]::new($zipStream,[IO.Compression.ZipArchiveMode]::Create)
    try { foreach ($file in $evidence) { $null=[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$file,[IO.Path]::GetFileName($file),[IO.Compression.CompressionLevel]::Optimal) } }
    finally { $zip.Dispose();$zipStream.Dispose() }
    Write-Host ('Evidence: '+$output+'.zip')
}
if (-not $unchanged) { throw 'The original profile changed during the test; evidence is not a controlled comparison.' }
exit $result

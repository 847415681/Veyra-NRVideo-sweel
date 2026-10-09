param(
    [string]$AppDirectory = $PSScriptRoot,
    [string]$ProfileSource = '',
    [string]$OutputDirectory = '',
    [string]$Source = '',
    [ValidateRange(15,90)][int]$DurationSeconds = 90
)
$ErrorActionPreference = 'Stop'
$app = [IO.Path]::GetFullPath($AppDirectory)
$diagnostic = Join-Path $app 'veyra-flash-diagnostic.exe'
foreach ($required in @($diagnostic, (Join-Path $app 'qt.conf'), (Join-Path $app 'Qt6Core.dll'), (Join-Path $app 'qml/Veyra/Main.qml'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw 'Place the diagnostic files beside the original Veyra 2.0.6 NVIDIA executable first.'
    }
}
$players = @(Get-Process -Name 'veyra_qml_ui','veyra-flash-diagnostic','flash-pixel-probe' -ErrorAction SilentlyContinue)
if ($players.Count -gt 0) { throw 'Close other Veyra players before starting this diagnostic.' }
if ([string]::IsNullOrWhiteSpace($ProfileSource)) { $ProfileSource = Join-Path $app 'runtime_local/user-data-2.0.0' }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $app ('logs/flash-diagnostic-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Output directory already exists; refusing to overwrite evidence.' }
$null = New-Item -ItemType Directory -Path $output
$profile = Join-Path $output 'private-profile'
$tmp = Join-Path $output 'tmp'
$null = New-Item -ItemType Directory -Path $profile,$tmp
$copied = @()
if (Test-Path -LiteralPath $ProfileSource -PathType Container) {
    $names = @('chain-session.v1','chain-session.v1.vfg','qml-preferences.v1.json','ui-session.v1','presets.v1','node-anchors.json')
    foreach ($name in $names) {
        $from = Join-Path $ProfileSource $name
        if (Test-Path -LiteralPath $from -PathType Leaf) {
            $digest = (Get-FileHash -LiteralPath $from -Algorithm SHA256).Hash
            Copy-Item -LiteralPath $from -Destination (Join-Path $profile $name)
            $copied += [pscustomobject]@{path=$from; sha256=$digest}
        }
    }
}
$envNames = @('TEMP','TMP','CUDA_CACHE_PATH','VEYRA_LOG_FILE','VEYRA_FLASH_PROBE_DIR','QML_DISABLE_DISK_CACHE')
$savedEnv = @{}
foreach ($name in $envNames) { $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$arguments = @('--data-dir',$profile,'--page','pro','--exit-after',($DurationSeconds * 1000).ToString())
if (-not [string]::IsNullOrWhiteSpace($Source)) { $arguments += $Source }
$result = 1
$forcedStop = $false
try {
    $env:TEMP=$tmp; $env:TMP=$tmp
    $env:CUDA_CACHE_PATH=Join-Path $tmp 'cuda-cache'
    $env:VEYRA_LOG_FILE=Join-Path $output 'engine.log'
    $env:VEYRA_FLASH_PROBE_DIR=$output
    $env:QML_DISABLE_DISK_CACHE='1'
    Write-Host 'Reproduce the flash with the original source and frame-generation settings, then close the diagnostic player.'
    Write-Host ('The player exits automatically after {0} seconds. Logs: {1}' -f $DurationSeconds,$output)
    # All arguments are flags, numbers or filesystem paths. Quoting each one
    # preserves spaces for Windows PowerShell 5.1's Start-Process serializer.
    if (@($arguments | Where-Object { $_.Contains('"') -or $_.EndsWith('\') }).Count -gt 0) {
        throw 'Unsupported diagnostic argument quoting.'
    }
    $quoted = @($arguments | ForEach-Object { '"' + $_ + '"' })
    # This is the interactive diagnostic player; its window must be operable.
    $process = Start-Process -FilePath $diagnostic -ArgumentList $quoted -WorkingDirectory $app -PassThru -WindowStyle Normal `
        -RedirectStandardOutput (Join-Path $output 'console.log') -RedirectStandardError (Join-Path $output 'stderr.log')
    # Keep the process handle open before exit. Windows PowerShell 5.1 can
    # otherwise return a null ExitCode after a manual WaitForExit call.
    $null = $process.Handle
    if (-not $process.WaitForExit(($DurationSeconds + 45) * 1000)) {
        $forcedStop = $true
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
        $result = 124
    } else {
        $process.WaitForExit()
        $process.Refresh()
        $result = $process.ExitCode
        if ($null -eq $result) { $result = 125 }
    }
} finally {
    foreach ($name in $envNames) { [Environment]::SetEnvironmentVariable($name,$savedEnv[$name],'Process') }
}
$unchanged = $true
foreach ($item in $copied) {
    if (-not (Test-Path -LiteralPath $item.path -PathType Leaf) -or
        (Get-FileHash -LiteralPath $item.path -Algorithm SHA256).Hash -ne $item.sha256) { $unchanged=$false }
}
$receipt = [pscustomobject]@{
    exit=$result; forcedStop=$forcedStop; diagnosticSha256=(Get-FileHash -LiteralPath $diagnostic -Algorithm SHA256).Hash;
    privateProfile=$profile; copiedSourceProfileFilesUnchanged=$unchanged; durationLimitSeconds=$DurationSeconds;
    pixelFiles=@(Get-ChildItem -LiteralPath $output -File -Filter 'flash-pixels-*.csv' | Select-Object -ExpandProperty Name)
}
$receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'diagnostic-receipt.json') -Encoding UTF8
# Include logs and samples only; never include the copied profile or credentials.
$evidence = @(Get-ChildItem -LiteralPath $output -File | Where-Object { $_.Extension -in '.log','.csv','.json' } | Select-Object -ExpandProperty FullName)
if ($evidence.Count -gt 0) {
    Compress-Archive -LiteralPath $evidence -DestinationPath ($output + '.zip') -CompressionLevel Optimal
    Write-Host ('Diagnostic evidence saved: ' + $output + '.zip')
}
exit $result

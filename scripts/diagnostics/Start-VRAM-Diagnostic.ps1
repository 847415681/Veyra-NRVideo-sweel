param(
    [string]$AppDirectory=$PSScriptRoot,
    [string]$ProfileSource='',
    [string]$OutputDirectory='',
    [string]$Source='',
    [ValidateRange(0,280)][int]$MaxSeconds=0,
    [ValidateRange(1,30)][int]$SampleSeconds=5
)
$ErrorActionPreference='Stop'
# A PowerShell 7 caller can pass module paths that exclude Windows PS 5.1's
# modules. Resolve the actual executing host's modules explicitly.
foreach($module in @('Microsoft.PowerShell.Utility','Microsoft.PowerShell.Management','CimCmdlets','Microsoft.PowerShell.Archive')) {
    Import-Module (Join-Path $PSHOME ('Modules/'+$module+'/'+$module+'.psd1')) -Force
}
$encoding=New-Object System.Text.UTF8Encoding($false)
function Write-JsonFile($Path,$Value) { [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 12),$encoding) }
function Open-Evidence($Path) {
    $file=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    return New-Object System.IO.StreamWriter($file,$encoding)
}
function Add-Evidence($Stream,$Value) { $Stream.WriteLine(($Value | ConvertTo-Json -Depth 10 -Compress));$Stream.Flush() }
$app=[IO.Path]::GetFullPath($AppDirectory)
$exe=Join-Path $app 'veyra-vram-diagnostic.exe'
foreach($file in @($exe,(Join-Path $app 'Qt6Core.dll'),(Join-Path $app 'qt.conf'),(Join-Path $app 'qml/Veyra/Main.qml'))) {
    if(-not(Test-Path -LiteralPath $file -PathType Leaf)){throw '请将诊断 EXE 和启动文件放到解压后的 Veyra 2.0.6 NVIDIA 主程序旁边。'}
}
if(@(Get-Process -Name 'veyra_qml_ui','veyra-vram-diagnostic','veyra-flash-diagnostic' -ErrorAction SilentlyContinue).Count -gt 0){throw '请先退出其他 Veyra 播放器，再启动诊断。'}
if(-not $ProfileSource){$ProfileSource=Join-Path $app 'runtime_local/user-data-2.0.0'}
if(-not $OutputDirectory){$OutputDirectory=Join-Path $app ('logs/vram-diagnostic-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))}
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $output){throw '输出目录已存在，拒绝覆盖已有诊断证据。'}
$null=New-Item -ItemType Directory -Path $output
$profile=Join-Path $output 'private-profile';$tmp=Join-Path $output 'tmp'
$null=New-Item -ItemType Directory -Path $profile,$tmp
$copied=@()
foreach($name in @('chain-session.v1','chain-session.v1.vfg','qml-preferences.v1.json','ui-session.v1','presets.v1','node-anchors.json')) {
    $from=Join-Path $ProfileSource $name
    if(Test-Path -LiteralPath $from -PathType Leaf){
        $copied+=@{path=$from;sha256=(Get-FileHash -LiteralPath $from -Algorithm SHA256).Hash}
        Copy-Item -LiteralPath $from -Destination (Join-Path $profile $name)
    }
}
$machine=@{utc=[DateTime]::UtcNow.ToString('o');powershell=$PSVersionTable.PSVersion.ToString();
    os=[Environment]::OSVersion.VersionString;is64BitProcess=[Environment]::Is64BitProcess;
    diagnosticSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash;
    executable=$exe;sampleSeconds=$SampleSeconds;dxgiSampleSeconds=2;maxSeconds=$MaxSeconds;
    scope='Process WDDM/DXGI accounting and partial application resource lifetime; not physical residency or complete allocation ownership.'}
try{$machine.videoControllers=@(Get-CimInstance Win32_VideoController -OperationTimeoutSec 5 | Select-Object Name,PNPDeviceID,DriverVersion,AdapterRAM)}catch{$machine.videoControllersUnavailable=$_.Exception.Message}
try{$machine.windows=@(Get-CimInstance Win32_OperatingSystem -OperationTimeoutSec 5 | Select-Object Caption,Version,BuildNumber)}catch{$machine.windowsUnavailable=$_.Exception.Message}
Write-JsonFile (Join-Path $output 'machine.json') $machine
$records=Open-Evidence (Join-Path $output 'process-memory.jsonl')
$modules=Open-Evidence (Join-Path $output 'modules.jsonl')
$markers=Open-Evidence (Join-Path $output 'markers.jsonl')
$names=@('TEMP','TMP','CUDA_CACHE_PATH','VEYRA_LOG_FILE','VEYRA_VRAM_DIAGNOSTIC_DIR')
$saved=@{};foreach($name in $names){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
$known=@{};$process=$null;$result=1;$forcedStop=$false;$failure=$null;$reason='player-exited'
$clock=[Diagnostics.Stopwatch]::StartNew()
try{
    $env:TEMP=$tmp;$env:TMP=$tmp;$env:CUDA_CACHE_PATH=Join-Path $tmp 'cuda-cache'
    $env:VEYRA_LOG_FILE=Join-Path $output 'engine.log';$env:VEYRA_VRAM_DIAGNOSTIC_DIR=$output
    # Only child-process environment is changed; no registry or global driver setting.
    $arguments=@('--data-dir',$profile,'--page','pro')
    if($MaxSeconds -gt 0){$arguments+=@('--exit-after',($MaxSeconds*1000).ToString())}
    if($Source){$arguments+=$Source}
    if(@($arguments | Where-Object {$_.Contains('"') -or $_.EndsWith('\')}).Count){throw 'Unsupported argument quoting.'}
    $quoted=@($arguments | ForEach-Object {'"'+$_+'"'})
    $process=Start-Process -FilePath $exe -ArgumentList $quoted -WorkingDirectory $app -PassThru -WindowStyle Normal `
        -RedirectStandardOutput (Join-Path $output 'console.log') -RedirectStandardError (Join-Path $output 'stderr.log')
    $null=$process.Handle
    $start=$process.StartTime.ToUniversalTime()
    Add-Evidence $records @{type='process';pid=$process.Id;startUtc=$start.ToString('o');executable=$exe;utc=[DateTime]::UtcNow.ToString('o');elapsedMs=$clock.ElapsedMilliseconds}
    Write-Host '显存诊断已启动。按平时的方法复现；先保持设置不变，不要反复重启播放器。'
    Write-Host '显存持续涨或明显卡顿时：停止播放，保持播放器打开约 30 秒，再正常退出。'
    Write-Host '可选：切回此窗口按数字打标记（只记录，不替你改设置）：'
    Write-Host '1 窗口播放  2 全屏播放  3 已关闭 NR  4 已开启 NR  5 已停止播放  6 已看到显存增长'
    Write-Host ('退出诊断播放器后自动打包。输出：'+$output)
    $nextSample=0L;$lastGpu=$null;$phase='unmarked'
    while(-not $process.HasExited){
        try{
            while(-not [Console]::IsInputRedirected -and [Console]::KeyAvailable){
                $key=[Console]::ReadKey($true).KeyChar.ToString()
                $labels=@{'1'='windowed';'2'='fullscreen';'3'='nr-off';'4'='nr-on';'5'='playback-stopped';'6'='growth-observed'}
                if($labels.ContainsKey($key)){
                    $phase=$labels[$key]
                    Add-Evidence $markers @{type='user-marker';utc=[DateTime]::UtcNow.ToString('o');elapsedMs=$clock.ElapsedMilliseconds;pid=$process.Id;phase=$phase;meaning='User annotation, not automatic proof of actual state'}
                    Write-Host ('已记录：'+$phase)
                }
            }
        }catch{} # noninteractive consoles still collect evidence
        if($clock.ElapsedMilliseconds -ge $nextSample){
            $current=Get-Process -Id $process.Id -ErrorAction SilentlyContinue
            if(-not $current){break}
            if($current.Path -ne $exe -or $current.StartTime.ToUniversalTime() -ne $start){throw 'PID/executable identity changed; refusing to mix sessions.'}
            $record=@{type='sample';utc=[DateTime]::UtcNow.ToString('o');elapsedMs=$clock.ElapsedMilliseconds;pid=$process.Id;phase=$phase}
            try{
                $record.gpu=@(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory -Filter "Name LIKE 'pid_$($process.Id)_%'" -OperationTimeoutSec 5 |
                    Where-Object {$_.Name.StartsWith('pid_'+$process.Id+'_')} | Select-Object Name,DedicatedUsage,SharedUsage,TotalCommitted)
                $record.gpuAvailable=$record.gpu.Count -gt 0
                if($record.gpuAvailable){$lastGpu=($record.gpu | Measure-Object DedicatedUsage -Sum).Sum}
            }catch{$record.gpuAvailable=$false;$record.gpuUnavailable=$_.Exception.Message}
            try{
                $current.Refresh();$record.privateBytes=$current.PrivateMemorySize64;$record.workingSetBytes=$current.WorkingSet64;$record.handles=$current.HandleCount
                $record.modules=@($current.Modules | Select-Object ModuleName,FileName)
                foreach($module in $record.modules){
                    $path=$module.FileName
                    if(-not $known.ContainsKey($path)){
                        $identity=@{type='module';utc=[DateTime]::UtcNow.ToString('o');elapsedMs=$clock.ElapsedMilliseconds;name=$module.ModuleName;path=$path}
                        try{$item=Get-Item -LiteralPath $path;$identity.bytes=$item.Length;$identity.modifiedUtc=$item.LastWriteTimeUtc.ToString('o');
                            $identity.version=$item.VersionInfo.FileVersion;$identity.sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
                        }catch{$identity.identityUnavailable=$_.Exception.Message}
                        Add-Evidence $modules $identity;$known[$path]=$identity
                    }
                }
            }catch{$record.processOrModulesUnavailable=$_.Exception.Message}
            Add-Evidence $records $record
            if($record.gpuAvailable){Write-Host ('[{0:hh\:mm\:ss}] 专用进程计数 {1:N0} MiB，共享 {2:N0} MiB，标记 {3}' -f $clock.Elapsed,($lastGpu/1MB),((($record.gpu | Measure-Object SharedUsage -Sum).Sum)/1MB),$phase)}
            else{Write-Host 'GPU 计数暂不可用，已明确记录；没有用 0 代替。'}
            $nextSample=$clock.ElapsedMilliseconds+$SampleSeconds*1000
        }
        if($MaxSeconds -gt 0 -and $clock.Elapsed.TotalSeconds -gt $MaxSeconds+45){
            $forcedStop=$true;$reason='bounded-test-timeout';Stop-Process -Id $process.Id -Force;$process.WaitForExit();break
        }
        Start-Sleep -Milliseconds 200
    }
    $process.WaitForExit();$process.Refresh();$result=$process.ExitCode
    if($null -eq $result){$result=125};if($forcedStop){$result=124}
}catch{
    $failure=$_.Exception.Message;$reason='collector-error';$result=1
    Write-Host ('采集失败，已有数据会保留：'+$failure)
}finally{
    foreach($name in $names){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
    $records.Dispose();$modules.Dispose();$markers.Dispose()
}
$unchanged=$true
foreach($item in $copied){if(-not(Test-Path -LiteralPath $item.path -PathType Leaf) -or (Get-FileHash -LiteralPath $item.path -Algorithm SHA256).Hash -ne $item.sha256){$unchanged=$false}}
$moduleChanged=@()
foreach($path in $known.Keys){
    try{
        $file=Get-Item -LiteralPath $path;$prior=$known[$path]
        if($prior.ContainsKey('sha256') -and ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $prior.sha256)){$moduleChanged+=$path}
    }catch{$moduleChanged+=($path+' [unavailable at end]')}
}
Write-JsonFile (Join-Path $output 'receipt.json') @{exit=$result;forcedStop=$forcedStop;reason=$reason;collectorFailure=$failure;elapsedSeconds=$clock.Elapsed.TotalSeconds;
    pid=$(if($process){$process.Id}else{$null});originalSettingsUnchanged=$unchanged;moduleFilesChanged=$moduleChanged;
    privacy='Evidence includes paths/device/module identities and application logs; excludes profile, credentials, media and crash dumps.'}
try{& (Join-Path $PSScriptRoot 'Summarize-VRAM-Diagnostic.ps1') -EvidenceDirectory $output}catch{[IO.File]::WriteAllText((Join-Path $output 'summary-failed.txt'),$_.Exception.ToString(),$encoding)}
# Strict, named whitelist. Never recursively archive the private profile/tmp.
$allow=@('machine.json','process-memory.jsonl','modules.jsonl','markers.jsonl','dxgi-memory.csv','resource-events.csv','tracked-live-resources.csv','windows.csv','engine.log','console.log','stderr.log','receipt.json','summary.txt','summary.json','summary-failed.txt')
$evidence=@($allow | ForEach-Object {Join-Path $output $_} | Where-Object {Test-Path -LiteralPath $_ -PathType Leaf})
Compress-Archive -LiteralPath $evidence -DestinationPath ($output+'.zip') -CompressionLevel Optimal
Write-Host ('完成。只需要发回这个文件：'+$output+'.zip')
exit $result

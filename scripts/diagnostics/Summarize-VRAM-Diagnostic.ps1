param([Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force
$encoding=New-Object System.Text.UTF8Encoding($false)
$root=[IO.Path]::GetFullPath($EvidenceDirectory)
$summary=@{units='MiB = 1048576 bytes';allocationOwner='unconfirmed';notes=@(
    'DXGI and WDDM are separate process accounting sources. They do not equal physical board residency.',
    'Tracked resources cover selected graph/NR textures and graph uploads only; not Qt/decoder/SDK internal allocations.',
    'User markers annotate phases; use engine logs to verify actual changes. Whole-session endpoints mix stages and are not a steady-state leak rate.')}
$lines=New-Object 'System.Collections.Generic.List[string]'
$lines.Add('Veyra 显存诊断摘要（根因未确认，不能据此单独判定驱动泄漏）')
function Add-Range($Rows,$TimeColumn,$ValueColumn,$Label){
    if($Rows.Count -lt 2){$lines.Add($Label+'：有效样本不足');return $null}
    $first=$Rows[0];$last=$Rows[-1];$seconds=([double]$last.$TimeColumn-[double]$first.$TimeColumn)/1000
    if($seconds -le 0){$lines.Add($Label+'：时间跨度不足');return $null}
    $begin=[double]$first.$ValueColumn/1MB;$end=[double]$last.$ValueColumn/1MB
    $peak=($Rows | ForEach-Object {[double]$_.$ValueColumn/1MB} | Measure-Object -Maximum).Maximum
    $result=@{samples=$Rows.Count;seconds=$seconds;firstMiB=$begin;lastMiB=$end;peakMiB=$peak;netMiB=$end-$begin;endpointMiBPerMinute=($end-$begin)*60/$seconds}
    $lines.Add(('{0}：{1:N1} → {2:N1} MiB，峰值 {3:N1}，净变化 {4:N1}；{5:N1} 秒 / {6} 样本' -f $Label,$begin,$end,$peak,($end-$begin),$seconds,$Rows.Count))
    return $result
}
$dxgiFile=Join-Path $root 'dxgi-memory.csv'
if(Test-Path -LiteralPath $dxgiFile){
    $rows=@(Import-Csv -LiteralPath $dxgiFile)
    $summary.dxgi=@()
    foreach($group in @($rows | Group-Object luid,generation,segment)){
        $valid=@($group.Group | Where-Object {$_.queryHr -eq '0' -and $_.usageBytes -ne ''})
        $range=Add-Range $valid 'tickMs' 'usageBytes' ('DXGI '+$group.Name)
        $summary.dxgi+=@{identity=$group.Name;failedSamples=@($group.Group | Where-Object {$_.queryHr -ne '0'}).Count;range=$range}
    }
    if($rows.Count){
        $last=$rows[-1];$summary.trackedAtLastSample=@{created=$last.trackedCreated;destroyed=$last.trackedDestroyed;live=$last.trackedLive;bytesEstimate=$last.trackedBytesEstimate;attachFailures=$last.attachFailures;unknownSizes=$last.unknownSizes}
        $lines.Add(('末次已覆盖资源：创建 {0}，销毁 {1}，存活 {2}，分配大小估计 {3:N1} MiB（不是总显存）' -f $last.trackedCreated,$last.trackedDestroyed,$last.trackedLive,([double]$last.trackedBytesEstimate/1MB)))
    }
}else{$lines.Add('DXGI：无文件；可能尚未打开任何视频或设备创建失败。')}
$processFile=Join-Path $root 'process-memory.jsonl'
if(Test-Path -LiteralPath $processFile){
    $samples=@(Get-Content -LiteralPath $processFile -Encoding UTF8 | ForEach-Object {$_ | ConvertFrom-Json} | Where-Object {$_.type -eq 'sample'})
    $wddm=@($samples | Where-Object {$_.gpuAvailable} | ForEach-Object {
        [pscustomobject]@{elapsedMs=$_.elapsedMs;dedicated=($_.gpu | Measure-Object DedicatedUsage -Sum).Sum;shared=($_.gpu | Measure-Object SharedUsage -Sum).Sum;phase=$_.phase}
    })
    $summary.wddmDedicated=Add-Range $wddm 'elapsedMs' 'dedicated' 'WDDM Dedicated（所有实例合计）'
    $summary.wddmShared=Add-Range $wddm 'elapsedMs' 'shared' 'WDDM Shared（所有实例合计）'
    $summary.gpuUnavailableSamples=@($samples | Where-Object {-not $_.gpuAvailable}).Count
    $summary.markedPhases=@()
    # Each contiguous phase is kept separate; repeated phase names are never merged.
    $phaseRows=@();$phaseName=$null;$ordinal=0
    foreach($row in $wddm+@([pscustomobject]@{phase='__end__'})){
        if($row.phase -ne $phaseName -and $phaseRows.Count){
            $ordinal++;$range=Add-Range $phaseRows 'elapsedMs' 'dedicated' ('标记段 '+$ordinal+' '+$phaseName)
            $summary.markedPhases+=@{ordinal=$ordinal;phase=$phaseName;wddmDedicated=$range};$phaseRows=@()
        }
        $phaseName=$row.phase;if($row.phase -ne '__end__'){$phaseRows+=,$row}
    }
}
$lines.Add('出现持续增长时：保持同 PID，记录关闭 NR / 停止播放后的回收；不必等到显存耗尽。')
$lines.Add('返回 evidence ZIP 即可；原始 JSONL/CSV 和引擎日志比摘要更重要。')
[IO.File]::WriteAllText((Join-Path $root 'summary.txt'),($lines -join [Environment]::NewLine),$encoding)
[IO.File]::WriteAllText((Join-Path $root 'summary.json'),($summary | ConvertTo-Json -Depth 14),$encoding)

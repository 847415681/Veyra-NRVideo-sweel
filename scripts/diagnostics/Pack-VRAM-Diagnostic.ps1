param([string]$EvidenceDirectory='')
$ErrorActionPreference='Stop'
foreach($module in @('Microsoft.PowerShell.Utility','Microsoft.PowerShell.Management','Microsoft.PowerShell.Archive')) {
    Import-Module (Join-Path $PSHOME ('Modules/'+$module+'/'+$module+'.psd1')) -Force
}
if(@(Get-Process -Name 'veyra-vram-diagnostic' -ErrorAction SilentlyContinue).Count){throw '请先正常退出显存诊断播放器，再打包已有记录。'}
if(-not $EvidenceDirectory){
    $latest=Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'logs') -Directory -Filter 'vram-diagnostic-*' |
        Sort-Object CreationTimeUtc -Descending | Select-Object -First 1
    if(-not $latest){throw '未找到诊断记录目录。'}
    $EvidenceDirectory=$latest.FullName
}
$root=[IO.Path]::GetFullPath($EvidenceDirectory)
if(-not(Test-Path -LiteralPath (Join-Path $root 'machine.json') -PathType Leaf)){throw '该目录不是诊断记录目录。'}
& (Join-Path $PSScriptRoot 'Summarize-VRAM-Diagnostic.ps1') -EvidenceDirectory $root
$allow=@('machine.json','process-memory.jsonl','modules.jsonl','markers.jsonl','dxgi-memory.csv','resource-events.csv','tracked-live-resources.csv','windows.csv','engine.log','console.log','stderr.log','receipt.json','summary.txt','summary.json','summary-failed.txt')
$files=@($allow | ForEach-Object {Join-Path $root $_} | Where-Object {Test-Path -LiteralPath $_ -PathType Leaf})
$zip=$root+'-repacked-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'.zip'
Compress-Archive -LiteralPath $files -DestinationPath $zip -CompressionLevel Optimal
Write-Host ('只需要发回这个文件：'+$zip)

param(
 [Parameter(Mandatory=$true)]
 [ValidateSet('cpu','info','vector','transfer','compute','memory')][string]$Mode,
 [ValidateRange(1,600)][int]$Seconds=1,
 [ValidateRange(8,256)][int]$MiB=64,
 [ValidatePattern('(?i)^(?!(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$)[A-Za-z0-9][A-Za-z0-9_-]{0,79}$')][string]$Label=$Mode,
 [ValidateRange(40,90)][int]$StopCelsius=82,
 [string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$Mode=$Mode.ToLowerInvariant()
$qcRoot=[System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if([string]::IsNullOrWhiteSpace($OutputDirectory)){$OutputDirectory=Join-Path $qcRoot 'results'}
$qcResults=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($qcResults) | Out-Null
$qcExe=Join-Path $qcRoot 'bin\api_stress.exe'
if(-not (Test-Path -LiteralPath $qcExe -PathType Leaf)){throw 'Build first: scripts\build.cmd'}
$qcSmi=(Get-Command nvidia-smi.exe -ErrorAction Stop).Source
$qcPrefix=Join-Path $qcResults $Label
$qcSuffixes=@('_metrics.csv','_stdout.txt','_stderr.txt','_telemetry.csv','_telemetry_stderr.txt','_session.json','.stop','.lock')
foreach($qcSuffix in $qcSuffixes){
 if(Test-Path -LiteralPath ($qcPrefix+$qcSuffix)){throw "Existing evidence will not be overwritten: $qcPrefix$qcSuffix. Use a new label or batch directory."}
}
# An exclusive new lock protects concurrent invocations using this script.
$qcLock=[System.IO.File]::Open(($qcPrefix+'.lock'),[System.IO.FileMode]::CreateNew,[System.IO.FileAccess]::Write,[System.IO.FileShare]::None)
$qcChild=$null;$qcMonitor=$null;$qcExit=$null;$qcReason='not started';$qcFailure=$null
$qcStarted=(Get-Date).ToString('o')
$qcClock=[System.Diagnostics.Stopwatch]::StartNew()
$qcArgs=@($Mode,'--seconds',[string]$Seconds,'--mib',[string]$MiB,'--csv',($qcPrefix+'_metrics.csv'),'--stop-file',($qcPrefix+'.stop'))
# Start-Process joins ArgumentList; quote each value to preserve paths with spaces.
function ConvertTo-QcNativeArgument([string]$Value){
 '"' + [regex]::Replace([regex]::Replace($Value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1') + '"'
}
$qcArgumentLine=($qcArgs | ForEach-Object {ConvertTo-QcNativeArgument $_}) -join ' '
function Get-QcTemperature {
 $qcText=& $qcSmi '-i' '0' '--query-gpu=temperature.gpu' '--format=csv,noheader,nounits' 2>&1
 if($LASTEXITCODE -ne 0){throw 'Temperature query failed.'}
 $qcTemperature=0
 if(-not [int]::TryParse([string]($qcText | Select-Object -First 1),[ref]$qcTemperature)){throw 'Temperature is unavailable.'}
 $qcTemperature
}
try {
 $qcInitialTemperature=Get-QcTemperature
 if($qcInitialTemperature -ge $StopCelsius){throw "Initial temperature is $qcInitialTemperature C; threshold is $StopCelsius C."}
 $qcQuery='timestamp,temperature.gpu,utilization.gpu,utilization.memory,memory.used,power.draw,clocks.current.graphics,clocks.current.memory,pstate'
 $qcMonitor=Start-Process -FilePath $qcSmi -ArgumentList @('-i','0',('--query-gpu='+$qcQuery),'--format=csv,nounits','-l','1') -WindowStyle Hidden -RedirectStandardOutput ($qcPrefix+'_telemetry.csv') -RedirectStandardError ($qcPrefix+'_telemetry_stderr.txt') -PassThru
 $qcChild=Start-Process -FilePath $qcExe -ArgumentList $qcArgumentLine -WorkingDirectory $qcRoot -WindowStyle Hidden -RedirectStandardOutput ($qcPrefix+'_stdout.txt') -RedirectStandardError ($qcPrefix+'_stderr.txt') -PassThru
 # Keep a handle open so Windows PowerShell 5.1 retains the process exit code.
 $null=$qcChild.Handle
 $qcReason='completed'
 while(-not $qcChild.HasExited){
  Start-Sleep -Milliseconds 1000
  $qcChild.Refresh()
  if($qcChild.HasExited){break}
  try {$qcTemperature=Get-QcTemperature}
  catch {$qcReason='temperature monitor unavailable';break}
  if($qcTemperature -ge $StopCelsius){$qcReason="temperature reached $qcTemperature C";break}
  if($qcMonitor.HasExited){$qcReason='telemetry process exited unexpectedly';break}
  if($qcClock.Elapsed.TotalSeconds -gt ($Seconds+120)){$qcReason='execution time exceeded duration plus 120 seconds';break}
 }
 if(-not $qcChild.HasExited){Stop-Process -Id $qcChild.Id -ErrorAction SilentlyContinue}
 $qcChild.WaitForExit();$qcChild.Refresh();$qcExit=$qcChild.ExitCode
}
catch {
 $qcFailure=$_
 if($qcReason -eq 'not started' -or $qcReason -eq 'completed'){$qcReason='monitor or launch error: '+$_.Exception.Message}
}
finally {
 if($qcChild -and -not $qcChild.HasExited){Stop-Process -Id $qcChild.Id -ErrorAction SilentlyContinue;$qcChild.WaitForExit();$qcChild.Refresh();$qcExit=$qcChild.ExitCode}
 if($qcMonitor -and -not $qcMonitor.HasExited){Stop-Process -Id $qcMonitor.Id -ErrorAction SilentlyContinue;$qcMonitor.WaitForExit()}
 $qcClock.Stop()
 $qcMeta=[ordered]@{label=$Label;mode=$Mode;requested_seconds=$Seconds;mib=$MiB;started=$qcStarted;finished=(Get-Date).ToString('o');exit_code=$qcExit;stop_reason=$qcReason;temperature_stop_c=$StopCelsius;executable=$qcExe;working_directory=$qcRoot;arguments=$qcArgs;sha256=(Get-FileHash -LiteralPath $qcExe -Algorithm SHA256).Hash.ToLowerInvariant()}
 $qcMeta | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($qcPrefix+'_session.json') -Encoding UTF8
 $qcLock.Dispose()
 Remove-Item -LiteralPath ($qcPrefix+'.lock') -ErrorAction SilentlyContinue
}
if(Test-Path -LiteralPath ($qcPrefix+'_stdout.txt')){Get-Content -LiteralPath ($qcPrefix+'_stdout.txt')}
if((Test-Path -LiteralPath ($qcPrefix+'_stderr.txt')) -and (Get-Item -LiteralPath ($qcPrefix+'_stderr.txt')).Length){Get-Content -LiteralPath ($qcPrefix+'_stderr.txt')}
if($qcFailure){throw $qcFailure}
if($qcExit -ne 0 -or $qcReason -ne 'completed'){throw "Run failed: $Label exit=$qcExit reason=$qcReason"}

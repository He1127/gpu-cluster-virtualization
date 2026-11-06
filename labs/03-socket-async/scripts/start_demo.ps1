param(
    [string]$PythonExe,
    [switch]$Full
)

$ErrorActionPreference = 'Stop'
$socketDemoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$socketDemoNetwork = Join-Path $socketDemoRoot 'bin\socket_lab.exe'
$socketDemoViewer = Join-Path $socketDemoRoot 'bin\result_viewer.exe'
$socketDemoSuite = Join-Path $PSScriptRoot 'run_suite.py'

function Test-SocketDemoPython {
    param([string]$Candidate)
    if (-not (Test-Path -LiteralPath $Candidate -PathType Leaf)) { return $false }
    # Skip the Windows Store alias, which may open a store page instead of Python.
    if ($Candidate -match '[\\/]Microsoft[\\/]WindowsApps[\\/]') { return $false }
    $socketDemoProbe = $null
    try {
        $socketDemoProbe = Start-Process -FilePath $Candidate -WindowStyle Hidden -PassThru -ArgumentList @(
            '-c', '"import sys;sys.exit(0 if sys.version_info >= (3,8) else 2)"'
        )
        $null = $socketDemoProbe.Handle
        if (-not $socketDemoProbe.WaitForExit(5000)) {
            $socketDemoProbe.Kill()
            $socketDemoProbe.WaitForExit()
            return $false
        }
        $socketDemoProbe.Refresh()
        return ($socketDemoProbe.ExitCode -eq 0)
    }
    catch { return $false }
    finally {
        if ($null -ne $socketDemoProbe) { $socketDemoProbe.Dispose() }
    }
}

function Get-SocketDemoPython {
    param([string]$Requested)
    if (-not [string]::IsNullOrWhiteSpace($Requested)) {
        $socketDemoRequestedPath = $Requested
        if (-not (Test-Path -LiteralPath $socketDemoRequestedPath -PathType Leaf)) {
            $socketDemoRequestedCommand = Get-Command -Name $Requested -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($null -ne $socketDemoRequestedCommand) { $socketDemoRequestedPath = $socketDemoRequestedCommand.Source }
        }
        if (Test-SocketDemoPython -Candidate $socketDemoRequestedPath) {
            return (Resolve-Path -LiteralPath $socketDemoRequestedPath).ProviderPath
        }
        throw 'The requested PythonExe is unavailable or is older than Python 3.8.'
    }
    $socketDemoCandidates = @()
    foreach ($socketDemoCommandName in @('python.exe', 'python3.exe')) {
        $socketDemoCommand = Get-Command -Name $socketDemoCommandName -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $socketDemoCommand) { $socketDemoCandidates += $socketDemoCommand.Source }
    }
    $socketDemoCandidates += Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
    foreach ($socketDemoCandidate in ($socketDemoCandidates | Select-Object -Unique)) {
        if (Test-SocketDemoPython -Candidate $socketDemoCandidate) {
            return (Resolve-Path -LiteralPath $socketDemoCandidate).ProviderPath
        }
    }
    throw 'Python 3.8 or later was not found. Install/configure Python, or pass -PythonExe with an existing interpreter path. No packages are installed by this launcher.'
}

function Invoke-SocketDemoBuild {
    param([string]$ScriptName)
    $socketDemoBuild = Join-Path $PSScriptRoot $ScriptName
    if (-not (Test-Path -LiteralPath $socketDemoBuild -PathType Leaf)) {
        throw ('Build script is missing: ' + $socketDemoBuild)
    }
    Push-Location -LiteralPath $socketDemoRoot
    try {
        & $socketDemoBuild
        if ($LASTEXITCODE -ne 0) {
            throw ('Build failed: ' + $ScriptName + '. Check the build directory logs.')
        }
    }
    finally { Pop-Location }
}

function ConvertTo-SocketDemoArgument {
    param([string]$Value)
    # Windows file names cannot contain a double quote. None of the generated
    # paths end in a slash, so ordinary double-quoted native arguments suffice.
    if ($Value.Contains('"')) { throw 'Unexpected double quote in a command argument.' }
    return ('"' + $Value + '"')
}

function Open-SocketDemoLog {
    param([string]$Mode, [string]$Path, [string]$Title)
    $socketDemoViewerArguments = $Mode + ' ' + (ConvertTo-SocketDemoArgument $Path) + ' --title ' + (ConvertTo-SocketDemoArgument $Title)
    Start-Process -FilePath $socketDemoViewer -WorkingDirectory $socketDemoRoot -WindowStyle Normal -ArgumentList $socketDemoViewerArguments | Out-Null
}

try {
    if (-not (Test-Path -LiteralPath $socketDemoSuite -PathType Leaf)) {
        throw ('Run suite is missing: ' + $socketDemoSuite)
    }
    if (-not (Test-Path -LiteralPath $socketDemoNetwork -PathType Leaf)) {
        Invoke-SocketDemoBuild -ScriptName 'build.cmd'
    }
    if (-not (Test-Path -LiteralPath $socketDemoViewer -PathType Leaf)) {
        Invoke-SocketDemoBuild -ScriptName 'build_viewer.cmd'
    }
    if (-not (Test-Path -LiteralPath $socketDemoNetwork -PathType Leaf) -or
        -not (Test-Path -LiteralPath $socketDemoViewer -PathType Leaf)) {
        throw 'A required executable is still missing after the build step.'
    }
    $socketDemoPython = Get-SocketDemoPython -Requested $PythonExe
    $socketDemoRuns = Join-Path $socketDemoRoot 'runs'
    [System.IO.Directory]::CreateDirectory($socketDemoRuns) | Out-Null
    do {
        $socketDemoName = 'demo_' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff')
        $socketDemoBatch = Join-Path $socketDemoRuns $socketDemoName
        $socketDemoStdout = Join-Path $socketDemoRuns ($socketDemoName + '_suite.log')
        $socketDemoStderr = Join-Path $socketDemoRuns ($socketDemoName + '_suite.err')
        $socketDemoCollision = (Test-Path -LiteralPath $socketDemoBatch) -or
            (Test-Path -LiteralPath $socketDemoStdout) -or
            (Test-Path -LiteralPath $socketDemoStderr)
        if ($socketDemoCollision) { Start-Sleep -Milliseconds 5 }
    } while ($socketDemoCollision)
    # Do not create Batch here: run_suite.py requires a completely new directory.
    $socketDemoTaskLogs = Join-Path $socketDemoBatch '01-task'
    Open-SocketDemoLog -Mode '--dir' -Path $socketDemoTaskLogs -Title 'Socket Lab 03 - coordinator and three workers'

    $socketDemoArguments = (ConvertTo-SocketDemoArgument $socketDemoSuite) + ' --out ' + (ConvertTo-SocketDemoArgument $socketDemoBatch)
    if ($Full) { $socketDemoArguments += ' --full' }
    $socketDemoProcess = $null
    try {
        $socketDemoProcess = Start-Process -FilePath $socketDemoPython -ArgumentList $socketDemoArguments -WorkingDirectory $socketDemoRoot -WindowStyle Hidden -RedirectStandardOutput $socketDemoStdout -RedirectStandardError $socketDemoStderr -PassThru
        $null = $socketDemoProcess.Handle
        $socketDemoProcess.WaitForExit()
        $socketDemoProcess.Refresh()
        $socketDemoExit = $socketDemoProcess.ExitCode
    }
    finally {
        if ($null -ne $socketDemoProcess) { $socketDemoProcess.Dispose() }
    }
    $socketDemoSummary = Join-Path $socketDemoBatch 'summary.log'
    if ($socketDemoExit -ne 0) {
        if ((Test-Path -LiteralPath $socketDemoStderr -PathType Leaf) -and
            (Get-Item -LiteralPath $socketDemoStderr).Length -gt 0) {
            Open-SocketDemoLog -Mode '--file' -Path $socketDemoStderr -Title 'Socket Lab 03 - actual error output'
        }
        else {
            Open-SocketDemoLog -Mode '--file' -Path $socketDemoStdout -Title 'Socket Lab 03 - incomplete run output'
        }
        throw ('The run did not pass. Exit=' + $socketDemoExit + '. Logs: ' + $socketDemoBatch)
    }
    if (-not (Test-Path -LiteralPath $socketDemoSummary -PathType Leaf)) {
        throw ('The process exited without its expected summary: ' + $socketDemoSummary)
    }
    Open-SocketDemoLog -Mode '--file' -Path $socketDemoSummary -Title 'Socket Lab 03 - measured results'
    Write-Output ('Demo complete. Actual results: ' + $socketDemoBatch)
    exit 0
}
catch {
    $socketDemoFailure = $_.Exception.Message
    try {
        Add-Type -AssemblyName System.Windows.Forms
        [System.Windows.Forms.MessageBox]::Show($socketDemoFailure, 'Socket Lab 03 launcher', 'OK', 'Error') | Out-Null
    }
    catch { [Console]::Error.WriteLine($socketDemoFailure) }
    exit 1
}

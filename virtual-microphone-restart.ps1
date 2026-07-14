param(
    [switch]$Restart
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$workingDirectory = $PSScriptRoot
$file = 'virtual-microphone.exe'
$config = 'virtual-microphone.ini'
$rnnoise = 'rnnoise_mono.dll'

foreach ($requiredFile in @($file, $config, $rnnoise)) {
    $path = Join-Path -Path $workingDirectory -ChildPath $requiredFile
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file not found: $path"
    }
}

function Start-VirtualMicrophone {
    $processPath = Join-Path -Path $workingDirectory -ChildPath $file
    $newProcess = Start-Process -WorkingDirectory $workingDirectory `
        -FilePath $processPath `
        -ArgumentList '--config virtual-microphone.ini' `
        -PassThru `
        -WindowStyle Hidden
    Write-Host "virtual-microphone started (PID: $($newProcess.Id))"
}

$processes = Get-Process -Name 'virtual-microphone' -ErrorAction SilentlyContinue
if ($processes) {
    if (-not $Restart) {
        $ids = $processes.Id -join ', '
        Write-Host "virtual-microphone is already running (PID: $ids). Use -Restart to replace it."
        exit 0
    }

    $processes | Stop-Process -ErrorAction Stop
    $processes | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
}

Start-VirtualMicrophone

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

function Restart-VirtualMicrophone {
    $processes = Get-Process -Name 'virtual-microphone' -ErrorAction SilentlyContinue
    if ($processes) {
        $processes | Stop-Process -ErrorAction Stop
        $processes | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
    }

    $processPath = Join-Path -Path $workingDirectory -ChildPath $file
    $newProcess = Start-Process -WorkingDirectory $workingDirectory `
        -FilePath $processPath `
        -ArgumentList '--config virtual-microphone.ini' `
        -PassThru `
        -WindowStyle Hidden
    Write-Host "virtual-microphone restarted (PID: $($newProcess.Id))"
}

if ($Restart) {
    Restart-VirtualMicrophone
} else {
    Write-Host 'Use -Restart to stop any existing virtual-microphone process and start a new one.'
}

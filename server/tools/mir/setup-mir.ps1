<#
 Installs the OPTIONAL LivePlay+ musical-analysis worker environment (Python + librosa).
 LivePlay works without it; BPM / key / energy / structure analysis simply stays unavailable.

 Usage:  powershell -ExecutionPolicy Bypass -File setup-mir.ps1 [-Target <dir>]
 Default target is %LOCALAPPDATA%\LivePlay\mir-env (writable even when LivePlay lives in Program Files).
 Afterwards set LIVEPLAY_MIR_PYTHON=<target>\Scripts\python.exe (setx), or leave the venv in
 server\tools\mir\.venv where a development build finds it automatically.
 Needs Python 3.10-3.13 on PATH and about 450 MB of disk. Licences: librosa ISC, numpy/scipy BSD, numba BSD.
#>
param([string]$Target = (Join-Path $env:LOCALAPPDATA 'LivePlay\mir-env'))
$ErrorActionPreference = 'Stop'
$requirements = Join-Path $PSScriptRoot 'requirements.txt'
python -m venv $Target
& (Join-Path $Target 'Scripts\python.exe') -m pip install --upgrade pip
& (Join-Path $Target 'Scripts\python.exe') -m pip install -r $requirements
Write-Host "Done. Set LIVEPLAY_MIR_PYTHON to $(Join-Path $Target 'Scripts\python.exe') and restart LivePlay."

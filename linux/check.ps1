# Checks what compiles under Linux, from Windows, with Docker Desktop.
#   powershell -File linux\check.ps1          (builds the image the first time, ~5 min)
#   powershell -File linux\check.ps1 -Full    (also a real CMake build, then runs the tests)
# The repository is mounted read-only; nothing is written to it.
param([switch]$Full)
$ErrorActionPreference = "Continue" # docker writes warnings on stderr; exit codes decide
$repo = Split-Path -Parent $PSScriptRoot
docker info 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "Docker ne répond pas : démarre Docker Desktop puis relance."; exit 2 }
docker build -q -t agentschat-linux-check "$repo\linux" | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "Construction de l'image Linux impossible."; exit 2 }
$mode = if ($Full) { "--full" } else { "" }
docker run --rm -v "${repo}:/src:ro" agentschat-linux-check bash /src/linux/check.sh /src $mode
exit $LASTEXITCODE

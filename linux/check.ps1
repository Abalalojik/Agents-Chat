# Checks what compiles under Linux, from Windows, with Docker Desktop.
#   powershell -File linux\check.ps1          (builds the image the first time, ~5 min)
# The repository is mounted read-only; nothing is written to it.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
docker info *> $null
if ($LASTEXITCODE -ne 0) { Write-Error "Docker ne répond pas : démarre Docker Desktop puis relance."; exit 2 }
docker build -q -t agentschat-linux-check "$repo\linux" | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "Construction de l'image Linux impossible."; exit 2 }
docker run --rm -v "${repo}:/src:ro" agentschat-linux-check bash /src/linux/check.sh /src
exit $LASTEXITCODE

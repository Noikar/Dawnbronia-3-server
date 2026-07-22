<#
.SYNOPSIS
  Push a zone's content into the running server and reload just that area.

.DESCRIPTION
  Zones are baked into the Docker image, so editing zones/<N>/ on disk does not
  change the running game. This copies the zone into the container's own
  filesystem and restarts that one area, which re-reads its .map/.chr/.itm on
  startup. The other areas keep running and their players never notice.

  Takes about 15 seconds. Only players inside area <N> are disconnected.

  NOTE: the copy lives in the container, not the image, so it is lost the next
  time the container is recreated. Run `docker compose build server` and
  `docker compose up -d` to make content permanent (that drops everyone).

  Do NOT try to bind-mount zone directories instead. The server is a 32-bit
  binary and 32-bit readdir() fails with EOVERFLOW on the large inode numbers
  Docker Desktop's mounts produce, so a mounted zone dir enumerates as empty
  and the area silently boots with no map at all. See docker-compose.yml.

.PARAMETER Area
  The area number to push and reload, e.g. 38.

.PARAMETER CopyOnly
  Copy the files in but skip the reload (the area keeps serving old content
  until it next restarts).

.PARAMETER Force
  Reload even if players are currently inside that area.

.EXAMPLE
  .\reload-zone.ps1 38
#>
param(
	[Parameter(Mandatory = $true, Position = 0)]
	[int]$Area,
	[switch]$CopyOnly,
	[switch]$Force
)

# Deliberately NOT "Stop". Windows PowerShell 5.1 wraps a native command's
# stderr in ErrorRecords, so anything docker or mysql writes there (the mysql
# password warning, the server's own log output) would abort the script even on
# a clean exit code. Every failure below is checked explicitly instead.
$ErrorActionPreference = "Continue"

# Docker Desktop does not put docker on PATH.
$env:PATH = "$env:ProgramFiles\Docker\Docker\resources\bin;$env:PATH"

$container = "astonia3-server"
$dbContainer = "astonia3-db"
$repo = Split-Path -Parent $MyInvocation.MyCommand.Path
$zoneDir = Join-Path $repo "zones\$Area"

if (-not (Test-Path $zoneDir)) {
	Write-Error "No zone directory at $zoneDir"
	exit 1
}

$maps = Get-ChildItem -Path $zoneDir -Filter *.map -ErrorAction SilentlyContinue
if (-not $maps) {
	Write-Error "$zoneDir has no .map file - the area would boot empty."
	exit 1
}

$running = docker ps --filter "name=$container" --format "{{.Names}}"
if ($running -ne $container) {
	Write-Error "Container $container is not running."
	exit 1
}

# Who is in this area right now? Only they get disconnected. The password goes
# through MYSQL_PWD rather than -p so mysql does not print its "password on the
# command line is insecure" warning over the script's output.
$occupants = docker exec -e MYSQL_PWD=astonia $dbContainer mysql -uroot -N -e `
	"SELECT name FROM merc.chars WHERE current_area=$Area AND name<>'';"
$occupants = @($occupants | Where-Object { $_ -and $_ -notmatch '^\s*$' })

if ($occupants.Count -gt 0 -and -not $CopyOnly -and -not $Force) {
	Write-Host "Players currently in area ${Area}: $($occupants -join ', ')" -ForegroundColor Yellow
	Write-Host "They will be disconnected. Re-run with -Force to proceed, or -CopyOnly to stage without reloading." -ForegroundColor Yellow
	exit 2
}

Write-Host "Copying zones/$Area into $container ..." -ForegroundColor Cyan
docker cp "$zoneDir/." "${container}:/server/zones/$Area/"
if (-not $?) { Write-Error "docker cp failed"; exit 1 }

# Editor backups are not content; keep the container's zone dir clean.
docker exec $container sh -c "rm -f /server/zones/$Area/*.bak-* 2>/dev/null; true" | Out-Null

if ($CopyOnly) {
	Write-Host "Copied. Skipping reload (-CopyOnly); area $Area still serves its old content." -ForegroundColor Green
	exit 0
}

# This is exactly what the in-game `/zone off <N>` / `/zone on <N>` commands do:
# the marker makes the area evacuate and exit, and removing it lets the
# supervisor (5s interval) respawn it against the content we just copied.
Write-Host "Reloading area $Area ..." -ForegroundColor Cyan
# Note when the reload began so the summary can use `docker logs --since` and
# report only this run. Counting lines instead is unreliable: stdout and stderr
# interleave differently between calls, so the offset drifts and the previous
# reload bleeds into the output.
$reloadStart = Get-Date
docker exec $container sh -c "echo 'ASTONIA_RUNTIME_OFFLINE reload-zone.ps1' > /server/zones/$Area/OFFLINE"

# Is the area's port bound? /proc/net/tcp lists addresses in hex, so match the
# port directly rather than piping through awk -- awk's $-variables and quotes
# do not survive PowerShell -> docker exec -> sh intact.
$portHex = "{0:X4}" -f (5555 + $Area)
function Test-AreaListening {
	$n = docker exec $container sh -c "grep -c ':$portHex ' /proc/net/tcp || true"
	return ([int]($n | Select-Object -First 1) -gt 0)
}

$stopped = $false
foreach ($i in 1..20) {
	Start-Sleep -Seconds 1
	if (-not (Test-AreaListening)) { $stopped = $true; break }
}
if (-not $stopped) {
	Write-Host "Area $Area did not shut down in time; removing the marker and leaving it alone." -ForegroundColor Yellow
	docker exec $container rm -f "/server/zones/$Area/OFFLINE"
	exit 1
}

docker exec $container rm -f "/server/zones/$Area/OFFLINE"

$back = $false
foreach ($i in 1..40) {
	Start-Sleep -Seconds 1
	if (Test-AreaListening) { $back = $true; break }
}

if (-not $back) {
	Write-Host "Area $Area did not come back. Check: docker logs $container" -ForegroundColor Red
	exit 1
}

# Only the lines this reload produced -- prove it re-read the zone's own files
# rather than merely restarting.
$window = [int]((Get-Date) - $reloadStart).TotalSeconds + 2
$fresh = docker logs --since "${window}s" $container

Write-Host "Area $Area reloaded and listening on port $((5555 + $Area))." -ForegroundColor Green

$read = $fresh | Select-String "Processing zone .*/zones/$Area/"
if ($read) {
	Write-Host "Re-read from disk:" -ForegroundColor Green
	$read | ForEach-Object { Write-Host "  $($_.Line.Trim())" }
} else {
	Write-Host "WARNING: this reload logged no 'Processing zone' lines for zones/$Area." -ForegroundColor Yellow
}

$errors = $fresh | Select-String "could not find item"
if ($errors) {
	Write-Host "Content errors reported:" -ForegroundColor Red
	$errors | Select-Object -First 5 | ForEach-Object { Write-Host "  $($_.Line.Trim())" }
}

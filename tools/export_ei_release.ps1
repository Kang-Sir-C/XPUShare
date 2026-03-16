param(
  [Parameter(Mandatory = $true)]
  [string]$Dest,

  # If set, removes Git metadata and large binaries in the exported tree.
  [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Ensure-Dir([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) {
    New-Item -ItemType Directory -Path $Path | Out-Null
  }
}

$src = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$dst = (Resolve-Path -LiteralPath $Dest -ErrorAction SilentlyContinue)
if (-not $dst) {
  $dst = $Dest
}

Write-Host "Exporting EI release from:" $src
Write-Host "To:" $dst

Ensure-Dir $dst

# Copy everything first (fast on Windows), then delete excluded paths.
robocopy $src $dst /E /NFL /NDL /NJH /NJS /NP | Out-Null

$exclude = @(
  # Hook implementation (keep private for EI release)
  "xhook",
  "xhook_修改之前",
  # Git metadata
  ".git",
  ".claude"
)

foreach ($rel in $exclude) {
  $p = Join-Path $dst $rel
  if (Test-Path -LiteralPath $p) {
    Write-Host "Removing:" $p
    Remove-Item -LiteralPath $p -Recurse -Force
  }
}

if ($Clean) {
  # Remove compiled libraries if present under other paths
  $binGlobs = @("*.so", "*.a", "*.o", "*.dll", "*.exe")
  foreach ($g in $binGlobs) {
    Get-ChildItem -Path $dst -Recurse -Force -Filter $g -ErrorAction SilentlyContinue | ForEach-Object {
      Write-Host "Removing binary:" $_.FullName
      Remove-Item -LiteralPath $_.FullName -Force
    }
  }
}

Write-Host "EI export done."


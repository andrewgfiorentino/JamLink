# SPDX-License-Identifier: GPL-3.0-or-later

param(
    [Parameter(Mandatory = $true)][string]$Updater,
    [Parameter(Mandatory = $true)][string]$TestRoot
)

$ErrorActionPreference = "Stop"
$TestRoot = [IO.Path]::GetFullPath($TestRoot)
$resolvedParent = (Resolve-Path -LiteralPath (Split-Path -Parent $TestRoot)).Path
$expectedRoot = [IO.Path]::GetFullPath(
    (Join-Path $resolvedParent (Split-Path -Leaf $TestRoot)))
if ($TestRoot -ne $expectedRoot) { throw "Unsafe updater test path" }
$target = Join-Path $TestRoot "JamLink-current"
$stage = Join-Path $TestRoot "JamLink-next"
if (Test-Path -LiteralPath $TestRoot) {
    Remove-Item -LiteralPath $TestRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $target, $stage | Out-Null
Set-Content -LiteralPath (Join-Path $target "JamLink.exe") -Value "old" -Encoding ascii
Set-Content -LiteralPath (Join-Path $target "LICENSE") -Value "license" -Encoding ascii
Set-Content -LiteralPath (Join-Path $stage "JamLink.exe") -Value "new" -Encoding ascii
Set-Content -LiteralPath (Join-Path $stage "LICENSE") -Value "license" -Encoding ascii
Set-Content -LiteralPath (Join-Path $stage "new-file.txt") -Value "present" -Encoding ascii

& $Updater --stage $stage --target $target --parent 0
if ($LASTEXITCODE -ne 0) { throw "Updater returned $LASTEXITCODE" }
if ((Get-Content -LiteralPath (Join-Path $target "JamLink.exe") -Raw).Trim() -ne "new") {
    throw "Staged package was not activated"
}
if (-not (Test-Path -LiteralPath (Join-Path $target "new-file.txt"))) {
    throw "Staged package content is incomplete"
}

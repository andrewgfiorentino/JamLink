# Copyright (c) 2026 Andrew Fiorentino
# SPDX-License-Identifier: GPL-3.0-or-later

param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

$sourceStatus = (& git -C $repositoryRoot status --porcelain --untracked-files=all) -join "`n"
if (-not [string]::IsNullOrWhiteSpace($sourceStatus)) {
    throw "Commit JamLink source changes before creating a distributable package"
}
$sourceCommit = (& git -C $repositoryRoot rev-parse --verify HEAD).Trim()
if ($sourceCommit -notmatch '^[0-9a-fA-F]{40}$') {
    throw "Could not determine the exact JamLink source commit"
}

$configurePreset = "windows-gui-vs2022"
$buildPreset = if ($Configuration -eq "Release") {
    "windows-gui-release"
} else {
    "windows-gui-debug"
}
& cmake --preset $configurePreset
if ($LASTEXITCODE -ne 0) {
    throw "JamLink configure failed with exit code $LASTEXITCODE"
}
& cmake --build --preset $buildPreset
if ($LASTEXITCODE -ne 0) {
    throw "JamLink build failed with exit code $LASTEXITCODE"
}
& ctest --test-dir (Join-Path $repositoryRoot "build\windows-gui-vs2022") `
    -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) {
    throw "JamLink tests failed with exit code $LASTEXITCODE"
}

$qtRoot = Join-Path $repositoryRoot ".qt\6.10.3\msvc2022_64"
$qtBin = Join-Path $qtRoot "bin"
$deployTool = Join-Path $qtBin "windeployqt.exe"
$qtPaths = Join-Path $qtBin "qtpaths.exe"
$binary = Join-Path $repositoryRoot (
    "build\windows-gui-vs2022\apps\desktop\{0}\JamLink.exe" -f $Configuration)
$updaterBinary = Join-Path $repositoryRoot (
    "build\windows-gui-vs2022\{0}\JamLinkUpdater.exe" -f $Configuration)
$distRoot = Join-Path $repositoryRoot "dist"
$cmakeCache = Join-Path $repositoryRoot "build\windows-gui-vs2022\CMakeCache.txt"
$versionMatch = Select-String -LiteralPath $cmakeCache `
    -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(\d+\.\d+\.\d+)$' | Select-Object -First 1
if ($null -eq $versionMatch) {
    throw "The configured JamLink version was not found in $cmakeCache"
}
$packageVersion = $versionMatch.Matches[0].Groups[1].Value
$packageName = "JamLink-$packageVersion-test-windows-x64"
$packageDirectory = Join-Path $distRoot $packageName
$archivePath = Join-Path $distRoot ($packageName + ".zip")
$archiveChecksumPath = $archivePath + ".sha256"

if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    throw "Build JamLink first: cmake --build --preset windows-gui-release"
}
if (-not (Test-Path -LiteralPath $updaterBinary -PathType Leaf)) {
    throw "Build the JamLink updater helper before packaging"
}
if (-not (Test-Path -LiteralPath $deployTool -PathType Leaf)) {
    throw "The pinned Qt 6.10.3 MSVC 2022 x64 kit is missing at $qtRoot"
}

New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
$resolvedDistRoot = (Resolve-Path -LiteralPath $distRoot).Path
$expectedPackageDirectory = Join-Path $resolvedDistRoot $packageName
if ($packageDirectory -ne $expectedPackageDirectory) {
    throw "Refusing to package outside the repository dist directory"
}
if (Test-Path -LiteralPath $packageDirectory) {
    Remove-Item -LiteralPath $packageDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDirectory | Out-Null
Copy-Item -LiteralPath $binary -Destination (Join-Path $packageDirectory "JamLink.exe")
Copy-Item -LiteralPath $updaterBinary `
    -Destination (Join-Path $packageDirectory "JamLinkUpdater.exe")

$env:PATH = $qtBin + ";" + $env:PATH
& $deployTool `
    --qtpaths $qtPaths `
    --dir $packageDirectory `
    --qmldir (Join-Path $repositoryRoot "apps\desktop\qml") `
    --release `
    --no-compiler-runtime `
    --skip-plugin-types qmltooling,generic `
    --no-translations `
    --no-system-d3d-compiler `
    --no-system-dxc-compiler `
    --no-ffmpeg `
    --no-quickcontrols2fluentwinui3styleimpl `
    --no-quickcontrols2fusion `
    --no-quickcontrols2fusionstyleimpl `
    --no-quickcontrols2imagine `
    --no-quickcontrols2imaginestyleimpl `
    --no-quickcontrols2material `
    --no-quickcontrols2materialstyleimpl `
    --no-quickcontrols2universal `
    --no-quickcontrols2universalstyleimpl `
    --no-quickcontrols2windowsstyleimpl `
    $binary
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

# The Qt Quick Controls import scanner deliberately reports every optional
# style. JamLink pins Basic before the QML engine starts, so carrying those
# unselected styles only bloats the tester and expands its notice surface.
$excludedStyleDirectories = @(
    "qml\QtQuick\Controls\FluentWinUI3",
    "qml\QtQuick\Controls\Fusion",
    "qml\QtQuick\Controls\Imagine",
    "qml\QtQuick\Controls\Material",
    "qml\QtQuick\Controls\Universal",
    "qml\QtQuick\Controls\Windows",
    "qml\QtQuick\NativeStyle"
)
$packagePrefix = [IO.Path]::GetFullPath($packageDirectory).TrimEnd("\") + "\"
foreach ($relativeStyle in $excludedStyleDirectories) {
    $styleDirectory = [IO.Path]::GetFullPath((Join-Path $packageDirectory $relativeStyle))
    if (-not $styleDirectory.StartsWith($packagePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a Qt style outside the package directory"
    }
    if (Test-Path -LiteralPath $styleDirectory -PathType Container) {
        Remove-Item -LiteralPath $styleDirectory -Recurse -Force
    }
}

# windeployqt cannot discover the compiler runtime when invoked outside a
# Visual Studio developer prompt. Locate the exact VS 2022 x64 redistributable
# used by the checked toolchain and deploy its allowed runtime files directly.
$vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vsWhere -PathType Leaf)) {
    throw "Visual Studio installer metadata was not found"
}
$visualStudioRoot = (& $vsWhere -latest -products * -version "[17.0,18.0)" `
    -property installationPath).Trim()
if ([string]::IsNullOrWhiteSpace($visualStudioRoot)) {
    throw "Visual Studio 2022 was not found"
}
$redistVersionRoot = Get-ChildItem -LiteralPath (Join-Path $visualStudioRoot "VC\Redist\MSVC") `
    -Directory | Where-Object { $_.Name -match '^\d' } |
    Sort-Object Name -Descending | Select-Object -First 1
$crtDirectory = Join-Path $redistVersionRoot.FullName "x64\Microsoft.VC143.CRT"
if (-not (Test-Path -LiteralPath $crtDirectory -PathType Container)) {
    throw "The Visual C++ x64 redistributable directory was not found"
}
Copy-Item -Path (Join-Path $crtDirectory "*.dll") -Destination $packageDirectory
$redistNotice = Join-Path $visualStudioRoot "Licenses\1033\Redist.txt"
if (Test-Path -LiteralPath $redistNotice -PathType Leaf) {
    Copy-Item -LiteralPath $redistNotice `
        -Destination (Join-Path $packageDirectory "MSVC_REDISTRIBUTABLE_LICENSE.txt")
}

$documents = @(
    "LICENSE",
    "NOTICE",
    "THIRD_PARTY_LICENSES.md",
    "TONIGHT_TEST.md",
    "SOURCE_AND_LICENSES.md"
)
foreach ($document in $documents) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot $document) -Destination $packageDirectory
}
Copy-Item -LiteralPath (Join-Path $repositoryRoot "third_party\material-design-icons\LICENSE") `
    -Destination (Join-Path $packageDirectory "MATERIAL_DESIGN_ICONS_LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $repositoryRoot "third_party\asio-sdk\LICENSE.txt") `
    -Destination (Join-Path $packageDirectory "ASIO_SDK_LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $repositoryRoot "third_party\asio-sdk\README.md") `
    -Destination (Join-Path $packageDirectory "ASIO_SDK_PROVENANCE.md")

$sourceArchive = Join-Path $packageDirectory "JamLink-$packageVersion-source.zip"
& git -C $repositoryRoot archive --format=zip --output=$sourceArchive HEAD
if ($LASTEXITCODE -ne 0) {
    throw "Could not create the exact JamLink corresponding-source archive"
}
Set-Content -LiteralPath (Join-Path $packageDirectory "SOURCE_COMMIT.txt") `
    -Value ($sourceCommit + "`n") -Encoding ascii

$manifestPath = Join-Path $packageDirectory "PACKAGE_MANIFEST.sha256"
$packageUri = [Uri]($packageDirectory.TrimEnd("\") + "\")
$manifestLines = Get-ChildItem -LiteralPath $packageDirectory -Recurse -File |
    Where-Object { $_.FullName -ne $manifestPath } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = [Uri]::UnescapeDataString(
            $packageUri.MakeRelativeUri([Uri]$_.FullName).ToString())
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $relative"
    }
Set-Content -LiteralPath $manifestPath -Value $manifestLines -Encoding utf8

if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}
if (Test-Path -LiteralPath $archiveChecksumPath) {
    Remove-Item -LiteralPath $archiveChecksumPath -Force
}
Compress-Archive -Path (Join-Path $packageDirectory "*") -DestinationPath $archivePath `
    -CompressionLevel Optimal

$archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath $archiveChecksumPath `
    -Value ($archiveHash + "  " + (Split-Path -Leaf $archivePath) + "`n") -Encoding ascii
Write-Output ("PACKAGE=" + $packageDirectory)
Write-Output ("ARCHIVE=" + $archivePath)
Write-Output ("CHECKSUM=" + $archiveChecksumPath)
Write-Output ("SHA256=" + $archiveHash)

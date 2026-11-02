# Builds SVGBlast (Release x64), then the release files in Installer\Output:
#   SVGBlast-<version>-Setup.exe     installer
#   SVGBlast-<version>-Portable.exe  the app itself, runs without installing
# The version comes from the exe's version info (SVGBlastProj.rc).
# Needs Visual Studio 2026 (v145 toolset) and Inno Setup 6 (winget install JRSoftware.InnoSetup).

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -prerelease -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found' }

& $msbuild "$root\SVGBlastProj\SVGBlastProj.sln" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$exe = "$root\SVGBlastProj\x64\Release\SVGBlast.exe"
$info = (Get-Item $exe).VersionInfo
$version = "$($info.ProductMajorPart).$($info.ProductMinorPart)"

$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
          "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) not found' }

& $iscc /Q "/DAppVersion=$version" "$PSScriptRoot\SVGBlast.iss"
if ($LASTEXITCODE -ne 0) { throw 'Installer build failed' }

Copy-Item $exe "$PSScriptRoot\Output\SVGBlast-$version-Portable.exe" -Force

Get-ChildItem "$PSScriptRoot\Output\SVGBlast-$version-*" | Format-Table Name, Length -AutoSize

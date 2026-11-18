# Packs a Windows build into a zip: the program, its symbols (crash call stacks in the session
# logs), the DLLs next to it, the license and a short readme.
#   powershell -ExecutionPolicy Bypass -File tools\make_release_win.ps1 [-BuildDir build\windows] [-Out build\RedTwo-windows-x64.zip]
# The program holds the recompiled game: the zip is for the person whose disc it was built from.
param(
    [string]$BuildDir = "",
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $root "build\windows" }
if (-not $Out) { $Out = Join-Path $root "build\RedTwo-windows-x64.zip" }
$stage = Join-Path $root "build\release-windows\Red Two"
if (Test-Path (Split-Path $stage)) { Remove-Item -Recurse -Force (Split-Path $stage) }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item (Join-Path $BuildDir "red_two.exe"), (Join-Path $BuildDir "red_two.pdb") $stage
Get-ChildItem $BuildDir -Filter *.dll | Copy-Item -Destination $stage
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
python (Join-Path $root "tools\third_party_notices.py") $BuildDir (Join-Path $stage "THIRD_PARTY_NOTICES.txt")
if ($LASTEXITCODE) { throw "third_party_notices.py failed" }
@"
Red Two - Star Wars: Rogue Squadron II - Rogue Leader, recompiled for PC

Requirements
  64-bit Windows 10 or 11, a CPU with AVX2 (Intel Haswell / AMD Zen or newer), a graphics card
  with Direct3D 12 or Vulkan.

Your own disc image
  Red Two needs your Rogue Leader disc (USA, GSWE64, revision 0) as .iso, .gcm, .rvz or .wia
  (Dolphin's formats). Put it next to red_two.exe (any file name), or in %APPDATA%\Red Two\,
  or start red_two.exe --disc "<path to the image>".

Playing
  F1 or the controller's Back/View button opens the menu: display, controls, cheats and the
  game's passcode unlocks, RetroAchievements (log in on the ACHIEVEMENTS page), mods, reset
  and quit (first page). Save states: Back + RB / Back + LB (F5 / F8).

Where things are
  Memory card: saves\GSWE64_A.raw next to red_two.exe (the working folder).
  Settings, save states, shader cache, session logs: %APPDATA%\Red Two\.
"@ | Set-Content -Encoding utf8 (Join-Path $stage "README.txt")

New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($Out))) | Out-Null
if (Test-Path $Out) { Remove-Item -Force $Out }
Compress-Archive -Path $stage -DestinationPath $Out -CompressionLevel Optimal
"{0}: {1:N0} MB" -f $Out, ((Get-Item $Out).Length / 1MB)

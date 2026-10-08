# Installs MidiHands for Ableton Live on Windows. Run in PowerShell:
#   irm https://github.com/HarzerHeribert/midihands_m4l/releases/latest/download/install.ps1 | iex
# or from an unzipped release folder:  powershell -ExecutionPolicy Bypass -File .\install.ps1
#
# Options (when run as a script): -Version X.Y.Z, -Yes (don't ask), -WaitForLive (wait until
# Live has quit; the device's Update button uses it, since Windows cannot replace files Live
# has loaded).
#
# What it does: copies the Max package to Documents\Max 9\Packages\midihands and the devices
# to your User Library (MidiHands in Presets\MIDI Effects\Max MIDI Effect, MidiHands Audio in
# Presets\Audio Effects\Max Audio Effect). Nothing else.
param(
  [string]$Version = "",
  [switch]$Yes,
  [switch]$WaitForLive
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is much faster without the progress bar

$Repo = "HarzerHeribert/midihands_m4l"
$Asset = "MidiHands-Windows.zip"
# The release build writes its own version here, so this script installs the release it came with.
$ReleaseVersion = "@RELEASE_VERSION@"
if (-not $Version -and $ReleaseVersion -notlike "@*") { $Version = $ReleaseVersion }

# Errors end the install with a message, but never close the window (`exit` would, under iex).
function Fail($message) { throw "MidiHands: $message" }

function Install-MidiHands {
# --- checks ------------------------------------------------------------------------------
if ([Environment]::OSVersion.Version.Major -lt 10) { Fail "MidiHands needs Windows 10 or newer." }
if (-not [Environment]::Is64BitOperatingSystem) { Fail "MidiHands needs 64-bit Windows." }

$documents = [Environment]::GetFolderPath("MyDocuments")
$packageDest = Join-Path $documents "Max 9\Packages\midihands"

# Live's User Library: from the newest Live 12 preferences, else the default place.
$userLibrary = Join-Path $documents "Ableton\User Library"
$prefs = Get-ChildItem -Path (Join-Path $env:APPDATA "Ableton") -Directory -Filter "Live 12*" -ErrorAction SilentlyContinue |
  Sort-Object Name -Descending | Select-Object -First 1
if ($prefs) {
  $cfg = Join-Path $prefs.FullName "Preferences\Library.cfg"
  if (Test-Path $cfg) {
    $text = Get-Content $cfg -Raw
    $block = [regex]::Match($text, "<UserLibrary>(.*?)</UserLibrary>", "Singleline").Groups[1].Value
    $path = [regex]::Match($block, '<ProjectPath Value="([^"]*)"').Groups[1].Value
    $name = [regex]::Match($block, '<ProjectName Value="([^"]*)"').Groups[1].Value
    if ($path -and $name -and (Test-Path (Join-Path $path $name))) { $userLibrary = Join-Path $path $name }
  }
}
$deviceDir = Join-Path $userLibrary "Presets\MIDI Effects\Max MIDI Effect"
$audioDir = Join-Path $userLibrary "Presets\Audio Effects\Max Audio Effect"

# --- get the files -----------------------------------------------------------------------
$here = if ($PSScriptRoot) { $PSScriptRoot } else { "" }
$work = $null
if ($here -and (Test-Path (Join-Path $here "midihands\externals\mh.hands.mxe64")) -and (Test-Path (Join-Path $here "MidiHands.amxd"))) {
  $src = $here
} else {
  $base = if ($Version) { "https://github.com/$Repo/releases/download/v$Version" } else { "https://github.com/$Repo/releases/latest/download" }
  if ($env:MIDIHANDS_RELEASE_URL) { $base = $env:MIDIHANDS_RELEASE_URL }
  $work = Join-Path ([IO.Path]::GetTempPath()) ("midihands-" + [Guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Path $work | Out-Null
  Write-Host "Downloading MidiHands $(if ($Version) { $Version } else { '(latest)' })..."
  try {
    Invoke-WebRequest -UseBasicParsing -Uri "$base/$Asset" -OutFile (Join-Path $work $Asset)
    Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS" -OutFile (Join-Path $work "SHA256SUMS")
  } catch { Fail "could not download $base/$Asset ($($_.Exception.Message))" }
  $line = Get-Content (Join-Path $work "SHA256SUMS") | Where-Object { $_ -match "\s\*?$([regex]::Escape($Asset))$" } | Select-Object -First 1
  $expected = if ($line) { ($line -split "\s+")[0].ToLower() } else { "" }
  $actual = (Get-FileHash -Algorithm SHA256 (Join-Path $work $Asset)).Hash.ToLower()
  if (-not $expected -or $expected -ne $actual) { Fail "the download does not match its checksum." }
  Expand-Archive -Path (Join-Path $work $Asset) -DestinationPath (Join-Path $work "unzipped")
  $src = Get-ChildItem (Join-Path $work "unzipped") -Directory | Select-Object -First 1 -ExpandProperty FullName
  if (-not (Test-Path (Join-Path $src "midihands"))) { Fail "the download does not contain the MidiHands package." }
}

$newVersion = ((Get-Content (Join-Path $src "midihands\package-info.json") -Raw) | ConvertFrom-Json).version
$oldVersion = ""
if (Test-Path (Join-Path $packageDest "package-info.json")) {
  $oldVersion = ((Get-Content (Join-Path $packageDest "package-info.json") -Raw) | ConvertFrom-Json).version
}

if (-not $Yes -and [Environment]::UserInteractive -and -not $WaitForLive) {
  Write-Host "Install MidiHands $newVersion$(if ($oldVersion) { " (replacing $oldVersion)" }):"
  Write-Host "  Max package: $packageDest"
  Write-Host "  Devices:     $deviceDir\MidiHands.amxd"
  Write-Host "               $audioDir\MidiHands Audio.amxd"
  $answer = Read-Host "Continue? [Y/n]"
  if ($answer -match "^[nN]") { Write-Host "Nothing changed."; return }
}

# --- install -------------------------------------------------------------------------------
# Live keeps the externals loaded, and Windows cannot replace loaded files.
$live = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like "Ableton Live*" }
if ($live) {
  if ($WaitForLive) {
    Write-Host "MidiHands $newVersion is ready. Quit Ableton Live to finish the update (this window waits)..."
    while (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like "Ableton Live*" }) { Start-Sleep -Seconds 1 }
    Start-Sleep -Seconds 2
  } else {
    Fail "please quit Ableton Live first (it keeps MidiHands' files in use), then run this again."
  }
}

New-Item -ItemType Directory -Force -Path (Split-Path $packageDest), $deviceDir, $audioDir | Out-Null
if (Test-Path $packageDest) {
  try { Remove-Item -Recurse -Force $packageDest } catch { Fail "could not replace $packageDest. Is Live still running? ($($_.Exception.Message))" }
}
Copy-Item -Recurse -Path (Join-Path $src "midihands") -Destination $packageDest
Copy-Item -Force -Path (Join-Path $src "MidiHands.amxd") -Destination (Join-Path $deviceDir "MidiHands.amxd")
Copy-Item -Force -Path (Join-Path $src "MidiHands Audio.amxd") -Destination (Join-Path $audioDir "MidiHands Audio.amxd")
# Downloaded files carry a "from the internet" mark; Live loads the externals without it.
Get-ChildItem -Recurse -File $packageDest | Unblock-File
Unblock-File (Join-Path $deviceDir "MidiHands.amxd"), (Join-Path $audioDir "MidiHands Audio.amxd")

if ($work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }

Write-Host "MidiHands $newVersion installed." -ForegroundColor Green
Write-Host "In Live's browser: User Library > Presets > MIDI Effects > Max MIDI Effect > MidiHands"
Write-Host "(or search for MidiHands), drop it on a MIDI track before an instrument, and switch the camera on."
Write-Host "For effects that follow the music: MidiHands Audio (Audio Effects > Max Audio Effect) on any track."
if ($WaitForLive) { Write-Host "You can start Live again."; Start-Sleep -Seconds 5 }
}

try {
  Install-MidiHands
} catch {
  Write-Host $_.Exception.Message -ForegroundColor Red
  if ($WaitForLive) { Start-Sleep -Seconds 30 }   # the Update button's window: leave time to read it
}

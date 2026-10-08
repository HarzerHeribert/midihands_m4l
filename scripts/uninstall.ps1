# Removes MidiHands on Windows: the Max package and the devices in your User Library.
# Live Sets that use MidiHands keep their settings but show the device as missing.
$ErrorActionPreference = "Stop"
$documents = [Environment]::GetFolderPath("MyDocuments")
$packageDest = Join-Path $documents "Max 9\Packages\midihands"
$userLibrary = Join-Path $documents "Ableton\User Library"
$prefs = Get-ChildItem -Path (Join-Path $env:APPDATA "Ableton") -Directory -Filter "Live 12*" -ErrorAction SilentlyContinue |
  Sort-Object Name -Descending | Select-Object -First 1
if ($prefs) {
  $cfg = Join-Path $prefs.FullName "Preferences\Library.cfg"
  if (Test-Path $cfg) {
    $block = [regex]::Match((Get-Content $cfg -Raw), "<UserLibrary>(.*?)</UserLibrary>", "Singleline").Groups[1].Value
    $path = [regex]::Match($block, '<ProjectPath Value="([^"]*)"').Groups[1].Value
    $name = [regex]::Match($block, '<ProjectName Value="([^"]*)"').Groups[1].Value
    if ($path -and $name -and (Test-Path (Join-Path $path $name))) { $userLibrary = Join-Path $path $name }
  }
}
if (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like "Ableton Live*" }) {
  Write-Host "Please quit Ableton Live first." -ForegroundColor Red; exit 1
}
Remove-Item -Recurse -Force $packageDest -ErrorAction SilentlyContinue
Remove-Item -Force (Join-Path $userLibrary "Presets\MIDI Effects\Max MIDI Effect\MidiHands.amxd") -ErrorAction SilentlyContinue
Remove-Item -Force (Join-Path $userLibrary "Presets\Audio Effects\Max Audio Effect\MidiHands Audio.amxd") -ErrorAction SilentlyContinue
Write-Host "MidiHands removed."

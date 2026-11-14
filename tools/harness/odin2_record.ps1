param([ValidateSet('start', 'stop')][string]$Action = 'start', [string]$Name = '')
# Record the Odin 2's screen with scrcpy (Windows side; WSL has no USB).
#
#   & tools\harness\odin2_record.ps1 start [-Name ki_x]   # prints the .mkv path
#   & tools\harness\odin2_record.ps1 stop                  # stops it, prints the path
#
# The recording is 60 fps (the game runs 30, so frames come in pairs), no
# audio, 40 Mbit/s. Stopping kills scrcpy, so the file has no seek index:
# remux it before pulling frames:
#   w -- nix shell nixpkgs#ffmpeg nixpkgs#python3Packages.pillow -c \
#     python3 tools/harness/video_frames.py remux /mnt/c/Users/user/Videos/<name>.mkv
$env:ADB = 'C:\Users\user\AppData\Local\Android\Sdk\platform-tools\adb.exe'
$state = Join-Path $env:TEMP 'odin2_record.txt'
if ($Action -eq 'start') {
  $scrcpy = (Get-Command scrcpy.exe -ErrorAction SilentlyContinue).Source
  if (-not $scrcpy) {
    $scrcpy = Get-ChildItem 'C:\Program Files\WinGet\Packages' -Recurse -Filter scrcpy.exe -ErrorAction SilentlyContinue |
      Select-Object -First 1 -ExpandProperty FullName
  }
  if (-not $scrcpy) { Write-Error 'scrcpy.exe not found'; exit 1 }
  if (-not $Name) { $Name = 'odin2_' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
  $out = "C:\Users\user\Videos\$Name.mkv"
  # A leftover scrcpy (an earlier stop, a crash) keeps a new one from ever
  # writing its file (2026-11-13: the user was waiting on a paused rail
  # while nothing recorded). Clear them first.
  Get-Process scrcpy -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep 1
  $p = Start-Process -FilePath $scrcpy -PassThru -WindowStyle Hidden -ArgumentList @(
    '--no-window', '--no-audio', "--record=$out", '--video-bit-rate=40M', '--max-fps=60')
  "$($p.Id) $out" | Set-Content $state
  # Only report success once the file exists and has data.
  for ($i = 0; $i -lt 15; $i++) {
    Start-Sleep 1
    if ($p.HasExited) { Write-Error "scrcpy exited (code $($p.ExitCode))"; exit 1 }
    $f = Get-Item $out -ErrorAction SilentlyContinue
    if ($f -and $f.Length -gt 0) { "RECORDING $out"; exit 0 }
  }
  Write-Error "scrcpy is running but $out has no data after 15 s"
  exit 1
} else {
  if (-not (Test-Path $state)) { Write-Error 'no recording started'; exit 1 }
  $id, $out = (Get-Content $state) -split ' ', 2
  Stop-Process -Id $id -ErrorAction SilentlyContinue
  Get-Process scrcpy -ErrorAction SilentlyContinue | Stop-Process -Force
  Remove-Item $state
  Start-Sleep 1
  $out
}

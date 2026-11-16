param([ValidateSet('show', 'hide')][string]$Action = 'show', [switch]$NoGamepad, [switch]$NoAudio)
# Show the Odin 2's screen on this PC in a scrcpy window (Windows side; WSL
# has no USB), for steps that need the user while they're at the PC. The
# Odin 2's counterpart of the Windows build's `window show = true`.
#
#   & tools\harness\odin2_screen.ps1 show   # opens the window, prints its pid
#   & tools\harness\odin2_screen.ps1 hide   # closes it
#
# A controller plugged into this PC drives the device: --gamepad=uhid makes
# it a physical HID gamepad on the device (Linux UHID), while the scrcpy
# window has focus. Keyboard and mouse go through as usual. -NoGamepad
# turns that off; -NoAudio keeps the sound on the device.
#
# odin2_record.ps1 start closes every scrcpy, this one included; record
# first, then show (both can run at once).
$env:ADB = 'C:\Users\user\AppData\Local\Android\Sdk\platform-tools\adb.exe'
$state = Join-Path $env:TEMP 'odin2_screen.txt'
if ($Action -eq 'show') {
  if (Test-Path $state) {
    $old = Get-Process -Id (Get-Content $state) -ErrorAction SilentlyContinue
    if ($old) { "already shown (pid $($old.Id))"; exit 0 }
    Remove-Item $state
  }
  $scrcpy = (Get-Command scrcpy.exe -ErrorAction SilentlyContinue).Source
  if (-not $scrcpy) {
    $scrcpy = Get-ChildItem 'C:\Program Files\WinGet\Packages' -Recurse -Filter scrcpy.exe -ErrorAction SilentlyContinue |
      Select-Object -First 1 -ExpandProperty FullName
  }
  if (-not $scrcpy) { Write-Error 'scrcpy.exe not found'; exit 1 }
  # Start-Process joins the list with spaces: quote values that have one.
  $argv = @('--window-title="Odin 2"', '--stay-awake', '--max-fps=60')
  if (-not $NoGamepad) { $argv += '--gamepad=uhid' }
  if ($NoAudio) { $argv += '--no-audio' }
  $p = Start-Process -FilePath $scrcpy -PassThru -ArgumentList $argv
  Start-Sleep 3
  if ($p.HasExited) { Write-Error "scrcpy exited (code $($p.ExitCode))"; exit 1 }
  "$($p.Id)" | Set-Content $state
  "SHOWN pid $($p.Id)"
} else {
  if (-not (Test-Path $state)) { 'not shown'; exit 0 }
  Stop-Process -Id (Get-Content $state) -ErrorAction SilentlyContinue
  Remove-Item $state
  'HIDDEN'
}

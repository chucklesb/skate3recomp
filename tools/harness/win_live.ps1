param([string[]]$Extra = @(), [string]$Backend = 'vulkan', [string]$Stage = 'C:\Users\user\skate3-ab\release')
# Start the staged Windows build in harness live mode (signed in, demo path)
# and print the new live run folder. Run it from PowerShell, not from WSL:
# WSL kills Windows processes it started once its command returns, so
# `run_scenario.py live --target windows` from WSL dies at once.
#
#   powershell -File tools\harness\win_live.ps1 -Extra '--native_render_pass_census=true'
#
# Then drive it from the Bash tool:
#   w -- bash .claude/skills/harness/rs.sh send --target windows query what=state
$prev = Get-Content "$Stage\harness\current_run.txt" -ErrorAction SilentlyContinue
$gameArgs = @("--gpu_backend=$Backend", '--game_data_root=C:\Users\user\skate3-ab\game',
  '--user_data_root=C:\Users\user\skate3-ab\userdata', '--skate3_harness_out=harness',
  '--skate3_harness=true', '--skate3_demo_path=true', '--skate3_demo_path_signed_in=true') + $Extra
Start-Process -FilePath "$Stage\skate3.exe" -WorkingDirectory $Stage -ArgumentList $gameArgs
for ($i = 0; $i -lt 30; $i++) {
  Start-Sleep 1
  $cur = Get-Content "$Stage\harness\current_run.txt" -ErrorAction SilentlyContinue
  if ($cur -and $cur -ne $prev) { $cur; exit 0 }
}
Write-Error 'no new live run folder after 30 s'
exit 1

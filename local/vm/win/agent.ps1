# The job runner of the Windows benchmark guest (local/vm/win.sh),
# adapted from another project of the author.
#
# Started at logon in the desktop session of the build user. Commands that
# come over SSH run in a service session with no desktop: the kiosk window,
# Windows Sandbox's window, PrintWindow screenshots and DPAPI with the user's
# keys all need the logged-on session instead — the one powershell.exe had
# under WSL. So `win` in winbuilder/config.sh drops a script into jobs\ and
# this loop runs it here.
#
#   jobs\<id>.ps1  — written by the container (renamed into place: complete)
#   jobs\<id>.out  — stdout and stderr, written while the job runs
#   jobs\<id>.rc   — the exit code, written last (renamed into place)
param([string]$Run)

$jobs = 'C:\nvwin\jobs'

if ($Run) {
  # One job: its own process, so a long build never holds up the loop.
  # WaitForExit, not -Wait: Start-Process -Wait waits for every descendant,
  # and a build leaves some running (VS telemetry, vctip.exe) — the job never
  # ended. The handle is touched first so ExitCode is kept.
  $p = Start-Process cmd -ArgumentList "/c powershell -NoProfile -ExecutionPolicy Bypass -File $Run.run.ps1 > $Run.out 2>&1" `
    -WindowStyle Hidden -PassThru
  $null = $p.Handle
  $p.WaitForExit()
  [IO.File]::WriteAllText("$Run.rc.tmp", "$($p.ExitCode)")
  Move-Item "$Run.rc.tmp" "$Run.rc" -Force
  exit 0
}

New-Item -ItemType Directory -Force $jobs | Out-Null
# Jobs of a previous session never finish now; their writer gave up on them.
Get-ChildItem $jobs -File | Remove-Item -Force -ErrorAction SilentlyContinue
while ($true) {
  foreach ($f in Get-ChildItem $jobs -Filter *.ps1 | Where-Object { $_.Name -notlike '*.run.ps1' }) {
    $base = Join-Path $jobs $f.BaseName
    try { Rename-Item $f.FullName "$base.run.ps1" -ErrorAction Stop } catch { continue }
    Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Run', $base `
      -WindowStyle Hidden
  }
  Start-Sleep -Milliseconds 200
}

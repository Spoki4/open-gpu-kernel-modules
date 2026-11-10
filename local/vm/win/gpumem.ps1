# Dedicated VRAM per process every 2 s (Windows' "GPU Process Memory"
# counters: what WDDM keeps resident for each process), for winbench.py:
# time, pid, name, MB into C:\nvwin\out\gpumem.csv until gpumem.stop exists.
$out = 'C:\nvwin\out\gpumem.csv'
'time,pid,name,dedicated_mb,shared_mb' | Set-Content $out
while (-not (Test-Path 'C:\nvwin\out\gpumem.stop')) {
  $t = (Get-Date).ToString('HH:mm:ss.fff')
  $s = (Get-Counter '\GPU Process Memory(*)\Dedicated Usage', '\GPU Process Memory(*)\Shared Usage' -ErrorAction SilentlyContinue).CounterSamples
  $by = @{}
  foreach ($x in $s) {
    if ($x.InstanceName -notmatch '^pid_(\d+)_') { continue }
    $p = [int]$Matches[1]
    if (-not $by[$p]) { $by[$p] = @{ d = 0; s = 0 } }
    if ($x.Path -like '*dedicated usage') { $by[$p].d += $x.CookedValue } else { $by[$p].s += $x.CookedValue }
  }
  $lines = foreach ($p in $by.Keys) {
    $d = [math]::Round($by[$p].d / 1MB); $sh = [math]::Round($by[$p].s / 1MB)
    if ($d -lt 32 -and $sh -lt 32) { continue }
    $n = (Get-Process -Id $p -ErrorAction SilentlyContinue).ProcessName
    "$t,$p,$n,$d,$sh"
  }
  if ($lines) { $lines | Add-Content $out }
  Start-Sleep -Milliseconds 1000
}

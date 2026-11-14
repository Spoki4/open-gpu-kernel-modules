# Policy simulator

Runs the driver's eviction policy without a GPU: the same
`src/nvidia/src/kernel/mem_mgr/vidmem_evict_policy.c` the driver builds,
linked against a memory model (`sim.c`) instead of RM. A policy change can be
tried on recorded sessions (gamerec) and hand-written scenarios in a second,
before it goes to the test VM and the host.

```
make                       # ./sim
./run-tests.sh [-v]        # every scenario in scenarios/
./sim [-v] [-cmp PID[,PID]] [-set KNOB=VALUE]... SCENARIO
./replay.py RECORDING [--from HH:MM[:SS]] [--to HH:MM[:SS]] [--expect "HH:MM:SS METRIC PID OP VALUE"]... [-o FILE]
```

`-v` prints every move (who, how much, why, for whom, how long) and the
policy's own log lines; `-cmp PID` prints recorded vs simulated memory of a
process once a minute (replays only). At the end: per process what sat in
sysmem while it was focused / visible (MB*s), moves out and back, failed
restores, ping-pong (out and back within 4 s), denials, and how long its own
allocations waited for moves.

## How the driver and the simulator share the policy

`vidmem_evict_policy.c` decides; the platform walks clients, moves memory and
reports free memory through the `vepPlat*()` functions declared in
`vidmem_evict_policy.h`: `vidmem_evict.c` in the driver, `sim.c` here. The
driver's entry points (`vidmemEvictMakeRoom`, `...WantBorn`,
`...KeepFgHeadroom`, `...BackgroundCushion`, `...MakeRoomCompositor`, the
policy tick) are thin wrappers around `vep*()` cores; the simulator calls the
same cores in the order `vidmemConstruct()` does (video_mem.c, no reserve).
Built with `-DVIDMEM_EVICT_SIM` the policy takes `NV_PRINTF`, `portMem*` and
the clock from `vidmem_evict_sim.h`.

## The model

- `cap`: vidmem for processes (free + the sum of per-process vidmem; replays
  take it from the recording at every step, so the driver's own memory such
  as scanout buffers and contexts follows the recording).
- Processes hold allocations in vidmem or evicted (moved out by the driver).
  An allocation the driver turns away counts as a denial (number, MB) and is
  not kept: the app falls back to sysmem by itself, and a recording does not
  show that memory either.
- Evict moves movable allocations in creation order up to the budget the
  policy set; restore brings back each evicted allocation that fits.
- A move takes 1 ms + size at 3.5 GB/s (`copy_gbs`) and the clock runs on
  meanwhile, as in the driver (the policy takes `nowNs` once per step).
- Activity (idle seconds, engine samples) and window hints are inputs, as
  the driver would have sampled them.
- With `set layout 1` allocations have places: each holds whole 2 MB chunks,
  the lowest free ones. Background compaction then runs as in the driver:
  the same window search (`vepCompactWindow()` in the policy file), whole
  allocations moved out of the window, the owner's piece size and the step's
  total, no new chunks in the window or in the run that is kept. `mix` lays
  several processes' allocations out interleaved.
- Not modelled: fragmentation without the layout (`frag` scales free 2 MB
  chunks, default none), 64 KB pages inside a chunk, contiguous windows for
  requests (always found, layout or not), per-allocation hot or cold (access
  counters), sizes of real allocations (a process grows in `chunk=` MB
  pieces), apps reacting to their budget.

## Scenarios

One command per line, `#` starts a comment. Times are seconds from the start.

| command | |
|---|---|
| `cap MB` | vidmem for processes |
| `set KNOB VALUE` | `fg_headroom`, `bg_cushion`, `auto`, `low`, `high` (policy registry knobs), `fg_own` (problem #7, 1/0), `copy_gbs`, `frag`, `longest` (MB, longest free run: a bigger contiguous allocation comes back from sysmem only into a window the policy had cleared; 0: no limit), `autotick` (policy step every 2 s, default 1), `hints`, `layout` (1: allocations have places), `compact` (MB, the free run compaction keeps), `compact_big` (0: compaction moves allocations of 32 MB at most, as before problem #17) |
| `proc PID NAME [old] [never] [exempt] [chunk=MB]` | a process; `old`: seen long before the start; kwin_wayland/Xwayland are the compositor |
| `hint PID f\|v\|h\|- [inh] [other] [full] [launched]` | window hint (focused, visible, hidden, none), inherited from a parent (of another app), has a fullscreen window, started by Steam/GameMode |
| `act PID IDLE_S\|- SAMPLES` | GPU activity: seconds since its last work (`-` never), engine samples in the last ~10 s |
| `alloc PID MB [n=COUNT] [fixed] [contig]` | allocations through the driver's path; `fixed`: not movable |
| `free PID MB` | frees the newest first |
| `target PID MB ELIG [spread=S]` | grow or shrink to MB in total (ELIG of it movable), at once or spread over S seconds |
| `place PID VRAM RAM ELIG` | initial state without the allocation path |
| `mix n=COUNT PID MB [PID MB]...` | COUNT rounds of one allocation each, without the allocation path: interleaved in the layout |
| `comp MB [contig]` | an allocation of the compositor side no process holds (makes room, is not kept) |
| `at T` | run until T (steps every 2 s with autotick) |
| `step` | a policy step now (what a hint change triggers) |
| `mark TEXT` | a line in the output |
| `expect METRIC PID\|- OP VALUE` | a check: `vid`, `evicted`, `sys` (= evicted), `evicts`, `moved_out` (MB copied out in all), `restores`, `restorefails`, `windows` (contiguous windows cleared for it), `pingpong`, `bounces` and `bounce_mb` (the driver's own count: moves undone within a minute, MB copied for nothing), `denied`, `denied_mb`, `born`, `stallmax`, `sysfg_mbs`, `free`, `longest` (MB, the longest free run), `compact_steps`, `compact_idle` (steps that moved / found nothing), `compact_pause` (ms, the longest pause a step gave the process), `compact_moved`; OP `<= >= < > ==` |
| `rec PID VRAM RAM` | recorded values for `-cmp` (replays) |
| `clock UNIX_S` | wall clock at time 0 (output shows HH:MM:SS) |

`run-tests.sh`: a scenario whose first lines say `# expect-fail: ...`
documents a known bug; its checks say what should happen and fail until the
fix is in. When such a scenario starts passing, the script says so (remove
the line).

## Replays

`replay.py` turns a gamerec recording (`nvtrace/trace.txt`: the per-process
state of every policy step) into a scenario: hints and activity before each
step, each process's growth spread over the time to the next step,
compositor requests of untracked processes, the recording's marks, and the
recorded vram/ram for `-cmp`. `--expect` adds checks at wall-clock times, so
a clip of a session with the behaviour it should have had is a test.

How close it gets (20261106_112014, CoD, 1 h 40 min in 0.05 s): footprints
match by construction; what is moved out follows the recording in the big
events (11:27 CoD loses 564 MB, recorded 494; 11:51-12:05 CoD 1034 MB stuck,
recorded 989, back at 12:05 in both; ghostty out and back every 2 s through
"visible again" as in the journal), drifts in fast switching (12:22-12:23:
Chrome had 56 MB out, the simulator 428 MB) because real allocation sizes
are not known.

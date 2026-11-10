# Notes for agents

Personal fork of NVIDIA open-gpu-kernel-modules with local patches (VRAM
reserve for the compositor, vidmem census, eviction). Not for upstream. The
user writes in Russian; answer in Russian.

- Start with `local/HANDOFF.md`: project history, rules the user set, and at
  the top the test VM, which is how driver builds are tested now (no host
  reboots: the RTX 3080 Ti belongs to a QEMU guest, `local/vm/vm.sh`).
- User-facing docs and commands: `local/README.md`.
- Policy changes (who is moved out, when it comes back) are tried first in
  the simulator, no GPU needed: `local/sim/` (run-tests.sh, replay.py for
  gamerec recordings), the same vidmem_evict_policy.c the driver builds.
- Tasks: `local/BACKLOG.md` (in Russian, for the user to browse) is the task
  tracker: the status column of its summary is the only place a status lives
  (открыта → в работе → сим ✓ → VM ✓ → на хосте → сделано), each worked-on
  item has a dated «Ход» line, finished items move to «Сделано» with the
  commit. When a problem is found or work on one moves, update it together
  with HANDOFF.

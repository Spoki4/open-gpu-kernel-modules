# Notes for agents

Personal fork of NVIDIA open-gpu-kernel-modules with local patches (vidmem
eviction, compaction, window-hint policy). Not for upstream. The repository
is public: no account names, ids, addresses or host names in files. The user
writes in Russian; answer in Russian.

- What exists and how to run it: `local/README.md` (design, layout, build,
  install, tests, the test VM, the numbers code comments cite).
- Policy changes (who is moved out, when it comes back) are tried first in
  the simulator, no GPU needed: `local/sim/` (run-tests.sh, replay.py for
  gamerec recordings), the same vidmem_evict_policy.c the driver builds.
- Then on a GPU that is not the desktop in use: the test VM
  (`local/vm/vmtest.sh`, `fulltest.sh`; the host must be in vm mode) or a
  rented machine (`local/cloud/`). Tests live in `local/tests/`.

Rules the user set:

- Root steps on the host are the user's: `install.sh`, `vm/mode.sh`,
  `vm/setup.sh`, reboots, passwords. Give the command, do not work around a
  denied sudo.
- Never run `vm.sh input on` (it takes the user's keyboard and mouse).
- Never hard-reset or kill the guest unless it does not answer at all: a
  forced end with its GPU driver running can wedge the GPU until a host power
  cycle.
- Long runs only under `local/watchdog.sh`, with a monitor that reports no
  progress and failures, not only the end.
- A fix is done right rather than fast; trade-offs are decided by the
  responsiveness of the focused window; a game matters more than a desktop
  app. One commit per task; what fails is reverted.

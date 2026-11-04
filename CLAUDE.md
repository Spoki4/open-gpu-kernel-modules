# Notes for agents

Personal fork of NVIDIA open-gpu-kernel-modules with local patches (VRAM
reserve for the compositor, vidmem census, eviction). Not for upstream. The
user writes in Russian; answer in Russian.

- Start with `local/HANDOFF.md`: project history, rules the user set, and at
  the top the test VM, which is how driver builds are tested now (no host
  reboots: the RTX 3080 Ti belongs to a QEMU guest, `local/vm/vm.sh`).
- User-facing docs and commands: `local/README.md`.

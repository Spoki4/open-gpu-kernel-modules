# The policy simulator: vidmem_evict_policy.c (the driver's own file) + sim.c.
REPO   := ../..
POLICY := $(REPO)/src/nvidia/src/kernel/mem_mgr
CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -DVIDMEM_EVICT_SIM \
          -I. -I$(POLICY) -I$(REPO)/src/common/sdk/nvidia/inc -I$(REPO)/src/nvidia/inc/kernel

sim: sim.c $(POLICY)/vidmem_evict_policy.c $(POLICY)/vidmem_evict_policy.h vidmem_evict_sim.h
	$(CC) $(CFLAGS) -o $@ sim.c $(POLICY)/vidmem_evict_policy.c

# sim-base: the simulator with the policy of a git revision (BASE, default
# HEAD), to compare a change in the working tree against it:
#   make base [BASE=rev]; ./sim-base SCENARIO vs ./sim SCENARIO
BASE ?= HEAD
base: sim.c vidmem_evict_sim.h
	mkdir -p .base
	git -C $(REPO) show $(BASE):src/nvidia/src/kernel/mem_mgr/vidmem_evict_policy.c > .base/vidmem_evict_policy.c
	git -C $(REPO) show $(BASE):src/nvidia/src/kernel/mem_mgr/vidmem_evict_policy.h > .base/vidmem_evict_policy.h
	$(CC) $(filter-out -I$(POLICY),$(CFLAGS)) -I.base -o sim-base sim.c .base/vidmem_evict_policy.c

test: sim
	./run-tests.sh

clean:
	rm -rf sim sim-base .base

.PHONY: test clean base

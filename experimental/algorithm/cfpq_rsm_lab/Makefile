# Stable human entry point; Python CLI also exposes --help.
VARIANT ?= baseline
VARIANTS ?= baseline,p1_review,p2_review,p3_review,p4_review
RECIPE ?=
SIZES ?=
RUN ?=
LABEL ?=
DRY_RUN ?= 0
ARGS = $(if $(RECIPE),--recipe $(RECIPE)) $(if $(SIZES),--sizes $(SIZES)) $(if $(LABEL),--label $(LABEL)) $(if $(filter 1,$(DRY_RUN)),--dry-run)
.PHONY: help list verify check correctness compare profile plot tooling-tests status status-check
help:
	@python3 lab.py --help
	@printf '\nExamples (run from this directory):\n  make status\n  make status-check\n  make list\n  make verify\n  make tooling-tests\n  make check VARIANT=all\n  make correctness VARIANT=p2\n  make compare RECIPE=smoke\n  make compare RECIPE=full SIZES=100 DRY_RUN=1\n  make profile VARIANT=p4\n  make plot RUN=/absolute/path/to/runs/date_label\n'
list:
	@python3 lab.py list
verify:
	@python3 lab.py verify
check correctness:
	@python3 lab.py $@ --variant $(VARIANT) $(ARGS)
compare:
	@python3 lab.py compare --variants $(VARIANTS) $(ARGS)
profile:
	@python3 lab.py profile --variant $(VARIANT) $(ARGS)
plot:
	@python3 lab.py plot $(if $(RUN),--run $(RUN)) $(ARGS)
tooling-tests:
	@python3 lab.py tooling-tests $(ARGS)

status status-check:
	@python3 lab.py $@

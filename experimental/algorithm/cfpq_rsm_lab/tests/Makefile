# cfpq_rsm_lab/tests/Makefile: isolated candidate build for DS-00 regressions
#
# Every binary here links exactly one CFPQ RSM translation unit (the current
# production source by default, or a CANDIDATE/R2 override), not the installed
# liblagraphx. The LAGraph/GraphBLAS shared
# libraries come from the checkout build tree so the headers and libraries are
# the same revision.
#
# Usage:
#   make check            # deterministic regressions + 200 randomized cases
#   make check-cases N=500
#   make existing         # existing experimental/test/test_CFPQ_RSM.c on the candidate
#   make production       # same regressions on the production source
#   make alloc            # allocation-failure sweep on the candidate
#   make asan             # sanitizer build of the candidate + fast regressions
#
# Override LAGRAPH, BUILD, GRAPHBLAS_LIBDIR, GRAPHBLAS_INCDIR as needed.

LAB        := ..
LAGRAPH    ?= ../../../..
BUILD      ?= $(LAGRAPH)/build
GRAPHBLAS_LIBDIR ?= /home/hollow/repo/GraphBLAS/build
GRAPHBLAS_INCDIR ?= /usr/local/include/suitesparse
CC         ?= cc
CFLAGS     ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter
INCLUDES   := -I$(LAGRAPH)/include -I$(LAGRAPH)/src/utility -I$(LAGRAPH)/src/test/include \
              -I$(GRAPHBLAS_INCDIR)
LDPATHS    := -L$(BUILD)/src -L$(BUILD)/src/test -L$(GRAPHBLAS_LIBDIR)
RPATHS     := -Wl,-rpath,$(abspath $(BUILD)/src) -Wl,-rpath,$(abspath $(BUILD)/src/test) \
              -Wl,-rpath,$(GRAPHBLAS_LIBDIR)
LDLIBS     := -llagraph -llagraphtest -lgraphblas -lm

PARENT     := $(LAB)/../LAGraph_CFPQ_RSM.c
CANDIDATE  ?= $(PARENT)
R2         ?= $(PARENT)
TEST_SRC   := test_ds00.c oracle.c

BIN_DIR    := ../build/tests

.PHONY: all check check-cases existing production alloc asan clean dirs

all: dirs candidate_correctness_r2 existing_candidate_r2

dirs:
	@mkdir -p $(BIN_DIR)

# Candidate override retained for historical experiments.
candidate_correctness: dirs $(CANDIDATE) $(TEST_SRC) oracle.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BIN_DIR)/$@ $(CANDIDATE) $(TEST_SRC) $(LDPATHS) $(RPATHS) $(LDLIBS)

# Current production source unless R2 selects a frozen reference/variant.
candidate_correctness_r2: dirs $(R2) $(TEST_SRC) oracle.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BIN_DIR)/$@ $(R2) $(TEST_SRC) $(LDPATHS) $(RPATHS) $(LDLIBS)

production_control: dirs $(PARENT) $(TEST_SRC) oracle.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BIN_DIR)/$@ $(PARENT) $(TEST_SRC) $(LDPATHS) $(RPATHS) $(LDLIBS)

existing_candidate: dirs $(CANDIDATE) $(LAGRAPH)/experimental/test/test_CFPQ_RSM.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BIN_DIR)/$@ $(CANDIDATE) $(LAGRAPH)/experimental/test/test_CFPQ_RSM.c $(LDPATHS) $(RPATHS) $(LDLIBS)

existing_candidate_r2: dirs $(R2) $(LAGRAPH)/experimental/test/test_CFPQ_RSM.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $(BIN_DIR)/$@ $(R2) $(LAGRAPH)/experimental/test/test_CFPQ_RSM.c $(LDPATHS) $(RPATHS) $(LDLIBS)

# sanitizer build: the candidate and the test driver are instrumented; the
# LAGraph/GraphBLAS libraries are not, so sanitizer reports inside them are
# outside this harness's coverage.
candidate_asan: dirs $(CANDIDATE) $(TEST_SRC) oracle.h
	$(CC) -O1 -g -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
	    $(INCLUDES) -o $(BIN_DIR)/$@ $(CANDIDATE) $(TEST_SRC) $(LDPATHS) $(RPATHS) $(LDLIBS)

# Generate the profiling source from the same selected input; never rely on a
# missing or stale variants/correctness_profile.c snapshot.
candidate_profile_r2: dirs $(R2) make_profile_variant.py $(LAB)/variants/cfpq_profile.h $(TEST_SRC) oracle.h
	python3 make_profile_variant.py $(R2) $(BIN_DIR)/correctness_profile.c
	$(CC) $(CFLAGS) -DCFPQ_RSM_PROFILE $(INCLUDES) -I$(LAB)/variants -o $(BIN_DIR)/$@ $(BIN_DIR)/correctness_profile.c $(TEST_SRC) $(LDPATHS) $(RPATHS) $(LDLIBS)

candidate_asan_r2: dirs $(R2) $(TEST_SRC) oracle.h
	$(CC) -O1 -g -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer \
	    $(INCLUDES) -o $(BIN_DIR)/$@ $(R2) $(TEST_SRC) $(LDPATHS) $(RPATHS) $(LDLIBS)

check: candidate_correctness_r2
	$(BIN_DIR)/candidate_correctness_r2

check-cases: candidate_correctness_r2
	$(BIN_DIR)/candidate_correctness_r2 --cases $(N)

existing: existing_candidate_r2
	$(BIN_DIR)/existing_candidate_r2

production: production_control
	$(BIN_DIR)/production_control --no-random

alloc: candidate_correctness_r2
	$(BIN_DIR)/candidate_correctness_r2 --alloc-sweep

# full sanitizer check: both sanitizers AND leak detection, all 200 randomized
# cases plus the deterministic suite.  The GraphBLAS/LAGraph shared libraries are
# not instrumented, so reports inside them are outside this harness's coverage.
asan: candidate_asan_r2
	ASAN_OPTIONS=detect_leaks=1 $(BIN_DIR)/candidate_asan_r2

# fast iteration mode: fewer randomized cases and no leak detection, not the
# coverage claimed for `make asan`
asan-fast: candidate_asan_r2
	ASAN_OPTIONS=detect_leaks=0 $(BIN_DIR)/candidate_asan_r2 --cases 20

clean:
	rm -f $(BIN_DIR)/candidate_correctness $(BIN_DIR)/candidate_correctness_r2 \
	      $(BIN_DIR)/production_control $(BIN_DIR)/existing_candidate \
	      $(BIN_DIR)/existing_candidate_r2 $(BIN_DIR)/candidate_asan $(BIN_DIR)/candidate_asan_r2 \
	      $(BIN_DIR)/candidate_profile_r2

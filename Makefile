# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The fuzz harnesses of reac-repacer (fuzz/README.md). The daemon and its tests
# build with meson (BUILDING.md); this Makefile holds only the fuzzing targets, and
# everything it builds lands in build-fuzz/.
#
#   make fuzz                   build every harness (clang, libFuzzer, ASan, UBSan)
#   make fuzz-run FUZZ_TIME=60  fuzz each for the budget; red on any crash, the
#                               crashing input left in fuzz/findings/
#   make fuzz-sabotage          every patch of fuzz/sabotage/ found by its named
#                               seed, in the check its header names
#   make fuzz-records           every check's own record, alone, makes that check
#                               abort on its seed; red on a check with no record and
#                               no not-a-property line
#   make fuzz-coverage          the coverage witness: reader lines held to
#                               fuzz/coverage-floor.txt, every check reached
#   make fuzz-static            each harness linked fully static with a plain
#                               driver, over its corpus
#   make fuzz-corpus            write fuzz/corpus/ again (fuzz/gen-corpus.c)
#   make fuzz-corpus-check      red when the committed corpus is not what the
#                               generator writes
#
# The readers call three things of libreac (reac_frame_clean_len,
# reac_frame_is_reac, and reac_braid_pos of its headers): its src/reac.c is built
# in, from a checkout at LIBREAC_DIR, which `make fuzz-libreac` clones at
# LIBREAC_REF when it is absent.

FUZZ_CC        ?= clang
FUZZ_TIME      ?= 60
LIBREAC_REF    ?= v1.8.0
LIBREAC_DIR    ?= subprojects/libreac
LLVM_COV       ?= llvm-cov
LLVM_PROFDATA  ?= llvm-profdata
LLVM_READELF   ?= llvm-readelf
FUZZ_BUILD     := build-fuzz
FUZZ_HARNESSES := wire egress lock uci
FUZZ_SAN       := -fsanitize=address,undefined -fno-sanitize-recover=all
FUZZ_WARN      := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror
FUZZ_CFLAGS    := -O1 -g -fno-omit-frame-pointer $(FUZZ_WARN)
FUZZ_INC       := -Iinclude -Ifuzz -I$(LIBREAC_DIR)/include
# the readers each harness drives, and libreac's file they call into
FUZZ_READERS   := src/repace_wire.c src/repace_conf.c $(LIBREAC_DIR)/src/reac.c
FUZZ_DEPS      := fuzz/fuzz.h include/repace_wire.h include/repace_conf.h $(FUZZ_READERS)

fuzz-libreac: $(LIBREAC_DIR)/src/reac.c
$(LIBREAC_DIR)/src/reac.c:
	git clone -q --depth 1 --branch $(LIBREAC_REF) https://github.com/FreeREAC/libreac $(LIBREAC_DIR)

$(FUZZ_BUILD)/fuzz_%: fuzz/fuzz_%.c $(FUZZ_DEPS)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SAN) -fsanitize=fuzzer $(FUZZ_INC) -o $@ $< $(FUZZ_READERS)

fuzz: $(FUZZ_HARNESSES:%=$(FUZZ_BUILD)/fuzz_%)

fuzz-run: fuzz
	FUZZ_BUILD=$(FUZZ_BUILD) FUZZ_TIME=$(FUZZ_TIME) fuzz/run.sh $(FUZZ_HARNESSES)

fuzz-sabotage:
	FUZZ_BUILD=$(FUZZ_BUILD) MAKE="$(MAKE)" fuzz/sabotage.sh

# each record built plain and native with fuzz/standalone.c, no sanitizer, as OSS
# Verify's coverage build is
fuzz-records: fuzz-libreac
	FUZZ_BUILD=$(FUZZ_BUILD) FUZZ_CC=$(FUZZ_CC) LIBREAC_DIR=$(LIBREAC_DIR) fuzz/records.sh $(FUZZ_HARNESSES)

# the coverage witness: the same harnesses without sanitizers, instrumented for
# llvm-cov, each run once over its seed corpus
FUZZ_COV_BUILD := $(FUZZ_BUILD)/cov
FUZZ_COV_FLAGS := -O0 -g -fprofile-instr-generate -fcoverage-mapping $(FUZZ_WARN)

$(FUZZ_COV_BUILD)/fuzz_%: fuzz/fuzz_%.c $(FUZZ_DEPS)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(FUZZ_COV_FLAGS) -fsanitize=fuzzer $(FUZZ_INC) -o $@ $< $(FUZZ_READERS)

fuzz-coverage: $(FUZZ_HARNESSES:%=$(FUZZ_COV_BUILD)/fuzz_%)
	@set -e; for h in $(FUZZ_HARNESSES); do \
	  rm -f $(FUZZ_COV_BUILD)/$$h.profraw; \
	  LLVM_PROFILE_FILE=$(FUZZ_COV_BUILD)/$$h.profraw $(FUZZ_COV_BUILD)/fuzz_$$h -runs=0 fuzz/corpus/$$h/ \
	      >$(FUZZ_COV_BUILD)/$$h.log 2>&1 || \
	    { cat $(FUZZ_COV_BUILD)/$$h.log; echo "fuzz-coverage: fuzz_$$h failed on its own corpus"; exit 1; }; \
	  $(LLVM_PROFDATA) merge -sparse $(FUZZ_COV_BUILD)/$$h.profraw -o $(FUZZ_COV_BUILD)/$$h.profdata; \
	done
	@LLVM_COV=$(LLVM_COV) fuzz/coverage.sh $(FUZZ_COV_BUILD) $(FUZZ_HARNESSES)

# the same harnesses driven by fuzz/standalone.c instead of libFuzzer and linked
# fully static, each run over its corpus. Needs the static libc (glibc-static)
FUZZ_STATIC_BUILD := $(FUZZ_BUILD)/static

$(FUZZ_STATIC_BUILD)/fuzz_%: fuzz/fuzz_%.c fuzz/standalone.c $(FUZZ_DEPS)
	@mkdir -p $(@D)
	$(FUZZ_CC) -O2 -g $(FUZZ_WARN) -static $(FUZZ_INC) -o $@ $< fuzz/standalone.c $(FUZZ_READERS)

fuzz-static: $(FUZZ_HARNESSES:%=$(FUZZ_STATIC_BUILD)/fuzz_%)
	@set -e; for h in $(FUZZ_HARNESSES); do \
	  ! $(LLVM_READELF) -l $(FUZZ_STATIC_BUILD)/fuzz_$$h | grep -qE 'INTERP|DYNAMIC' || \
	    { echo "fuzz-static: fuzz_$$h is not statically linked"; exit 1; }; \
	  echo "fuzz_$$h, static: $$($(FUZZ_STATIC_BUILD)/fuzz_$$h fuzz/corpus/$$h/*)"; done

# the seed corpus is written by fuzz/gen-corpus.c, from the frame law and the lock
# and uci grammars, never from a network or a capture, and committed
$(FUZZ_BUILD)/gen-corpus: fuzz/gen-corpus.c
	@mkdir -p $(@D)
	$(FUZZ_CC) -O1 -g $(FUZZ_WARN) -o $@ fuzz/gen-corpus.c

fuzz-corpus: $(FUZZ_BUILD)/gen-corpus
	rm -rf fuzz/corpus
	$(FUZZ_BUILD)/gen-corpus fuzz/corpus

fuzz-corpus-check: $(FUZZ_BUILD)/gen-corpus
	@set -e; tmp=$$(mktemp -d); trap 'rm -rf $$tmp' EXIT; \
	$(FUZZ_BUILD)/gen-corpus $$tmp/corpus; diff -r $$tmp/corpus fuzz/corpus; echo "the seed corpus is current"

.PHONY: fuzz fuzz-libreac fuzz-run fuzz-sabotage fuzz-records fuzz-coverage fuzz-static fuzz-corpus \
        fuzz-corpus-check

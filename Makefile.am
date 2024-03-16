# SPDX-License-Identifier: Apache-2.0
# Author: Robert Balas <balasr@iis.ee.ethz.ch>
SUBDIRS = lib/riscv-binutils-gdb/bfd lib/riscv-binutils-gdb/opcodes lib/riscv-binutils-gdb/libiberty lib/riscv-binutils-gdb/zlib

dist_doc_DATA = README.org
bin_PROGRAMS = trdb
noinst_PROGRAMS = tests benchmarks

TRDB_LINKER_INCLUDES = -Ilib/riscv-binutils-gdb/include -Ilib/riscv-binutils-gdb/bfd
TRDB_ALL_LINKER_FLAGS = -Llib/riscv-binutils-gdb/opcodes -Llib/riscv-binutils-gdb/bfd -Llib/riscv-binutils-gdb/libiberty -Llib/riscv-binutils-gdb/zlib
TRDB_ALL_LINKER_LIBS= -lbfd -lopcodes -liberty -lz -ldl -lc

# TRDB CLI tool
trdb_SOURCES = src/trace_debugger.c src/utils.c src/serialize.c \
	src/error.c src/disassembly.c src/trdb.c

trdb_LDFLAGS = $(TRDB_ALL_LINKER_FLAGS)
trdb_LDADD = $(TRDB_ALL_LINKER_LIBS)

# Tests
tests_SOURCES = src/trace_debugger.c src/utils.c src/serialize.c \
	src/error.c src/disassembly.c test/tests.c
tests_LDFLAGS = $(TRDB_ALL_LINKER_FLAGS)
tests_LDADD = $(TRDB_ALL_LINKER_LIBS)

TESTS=tests
LOG_DRIVER=./tests

# Benchmarks
benchmarks_SOURCES = src/trace_debugger.c src/utils.c src/serialize.c \
	src/error.c src/disassembly.c benchmark/benchmarks.c
benchmarks_LDFLAGS = $(TRDB_ALL_LINKER_FLAGS)
benchmarks_LDADD = $(TRDB_ALL_LINKER_LIBS)

# Dynamic library
lib_LTLIBRARIES = libtrdb.la
libtrdb_la_SOURCES = src/trace_debugger.c src/utils.c src/serialize.c \
	src/error.c src/disassembly.c dpi/trdb_sv.c
libtrdb_la_LDFLAGS = $(TRDB_ALL_LINKER_FLAGS)
libtrdb_la_LIBADD  = $(TRDB_ALL_LINKER_LIBS)

include_HEADERS = include/disassembly.h include/serialize.h include/trace_debugger.h

AM_CFLAGS = -std=gnu11 -Wall -Wextra -Werror=format-security -Wno-missing-field-initializers -Wno-unused-function -Wno-missing-braces -fdiagnostics-color
AM_CPPFLAGS = -D_GNU_SOURCE -Iinclude -Iinternal $(TRDB_LINKER_INCLUDES) -D_GLIBCXX_ASSERTIONS

# workaround for shared source files
# https://www.gnu.org/software/automake/manual/html_node/Objects-created-both-with-libtool-and-without.html
trdb_CFLAGS = $(AM_CFLAGS)
tests_CFLAGS = $(AM_CFLAGS)
benchmarks_CFLAGS = $(AM_CFLAGS)

doxygen-doc: doxyfile
	doxygen doxyfile

# patched spike to produce traces (32-bit and 64-bit)
define spike_template =

# we use sed to cutoff the internal boot sequence of spike. Alternatively we
# could add this code to the binary but this is easier.
spike-traces-$(1): riscv-tests-$(1)/benchmarks/build.ok spike-$(1)
	mkdir -p riscv-traces-$(1)
	for benchmark in riscv-tests-$(1)/benchmarks/*.riscv; do \
		./trdb-spike-$(1) \
			--ust-trace=riscv-traces-$(1)/$$$$(basename $$$$benchmark).cvs \
			$$$$benchmark; \
		sed -i 2,6d riscv-traces-$(1)/$$$$(basename $$$$benchmark).cvs; \
		cp $$$$benchmark riscv-traces-$(1)/$$$$(basename $$$$benchmark); \
	done

spike-$(1): riscv-isa-sim-$(1)/build.ok riscv-fesvr/build.ok

riscv-isa-sim-$(1)/build.ok: riscv-fesvr/build.ok
	rm -rf riscv-isa-sim-$(1)
	git clone https://github.com/pulp-platform/riscv-isa-sim riscv-isa-sim-$(1) -b spike_trace_path
	cd riscv-isa-sim-$(1) && \
		LDFLAGS="-L../riscv-fesvr" ./configure --with-isa=rv$(1)imc
	cd riscv-isa-sim-$(1) && \
		ln -s ../riscv-fesvr/fesvr . && \
		$(MAKE) && \
		touch build.ok
	cd ..
	echo "#!/usr/bin/env bash" > trdb-spike-$(1)
	echo "LD_LIBRARY_PATH=./riscv-isa-sim-$(1):./riscv-fesvr ./riscv-isa-sim-$(1)/spike \"\$$$$@\"" \
		>> trdb-spike-$(1)
	chmod u+x trdb-spike-$(1)

test-trdb-$(1):
	echo "#!/usr/bin/env bash" > trdb-spike-$(1)
	echo "LD_LIBRARY_PATH=./riscv-isa-sim-$(1):./riscv-fesvr ./riscv-isa-sim-$(1)/spike \"\$$$$@\"" \
		>> trdb-spike-$(1)
	chmod u+x trdb-spike-$(1)

# riscv-benchmark-tests
# have your $RISCV point to your compiler
riscv-tests-$(1)/benchmarks/build.ok:
	rm -rf riscv-tests-$(1)
	git clone https://github.com/riscv/riscv-tests/ --recursive riscv-tests-$(1)
	cd riscv-tests-$(1) && git checkout ea85805 --recurse-submodules
	cd riscv-tests-$(1) && ./configure --with-xlen=$(1)
	cd riscv-tests-$(1) && $(MAKE) benchmarks RISCV_GCC_OPTS="-DPREALLOCATE=1 -mcmodel=medany -static -std=gnu99 -O2 -ffast-math -fno-common -fno-builtin-printf -fno-tree-loop-distribute-patterns -march=rv$(1)gcv -mabi=ilp32"
	cd riscv-tests-$(1) && touch build.ok
endef


$(eval $(call spike_template,32))
$(eval $(call spike_template,64))


riscv-fesvr/build.ok:
	rm -rf riscv-fesvr
	git clone https://github.com/riscv/riscv-fesvr.git riscv-fesvr
	cd riscv-fesvr && ./configure && $(MAKE) && touch build.ok

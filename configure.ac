dnl SPDX-License-Identifier: Apache-2.0
dnl Author: Robert Balas <balasr@iis.ee.ethz.ch>
AC_INIT([trdb], [0.2], [balasr@iis.ee.ethz.ch])
AM_INIT_AUTOMAKE([-Wall foreign subdir-objects])
AC_PROG_CC
AC_PROG_RANLIB
AM_PROG_AR
LT_INIT
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
 Makefile
])
AX_SUBDIRS_CONFIGURE([lib/riscv-binutils-gdb/bfd], [--enable-targets=riscv32, --with-pic])
AX_SUBDIRS_CONFIGURE([lib/riscv-binutils-gdb/opcodes], [--enable-targets=riscv32, --with-pic])
AX_SUBDIRS_CONFIGURE([lib/riscv-binutils-gdb/libiberty], [], [[CFLAGS=-O2 -g -fpic]])
AX_SUBDIRS_CONFIGURE([lib/riscv-binutils-gdb/zlib], [], [[CFLAGS=-O2 -g -fpic]])
AC_OUTPUT

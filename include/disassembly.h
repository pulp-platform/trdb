/*
 * trdb - Trace Debugger Software for the PULP platform
 *
 * Copyright (C) 2024 Robert Balas
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file disassembly.h
 * @author Robert Balas (balasr@student.ethz.ch)
 * @date 25 Aug 2018
 * @brief Collection of disassembly routines using libopcdes and libbfd
 */

#ifndef __DISASSEMBLY_H__
#define __DISASSEMBLY_H__

#include <inttypes.h>
#include <stdbool.h>

#include "config.h"
#include "bfd.h"
#include "demangle.h"
#include "dis-asm.h"
#include "trace_debugger.h"

/**
 * Used to capture all information and functions needed to disassemble.
 */
struct disassembler_unit {
    disassembler_ftype disassemble_fn; /**< does the actual disassembly */
    struct disassemble_info *dinfo;    /**< context for disassembly */
};

#define TRDB_NO_ALIASES 1
#define TRDB_PREFIX_ADDRESSES 2
#define TRDB_DO_DEMANGLE 4
#define TRDB_DISPLAY_FILE_OFFSETS 8
#define TRDB_LINE_NUMBERS 16
#define TRDB_SOURCE_CODE 32
#define TRDB_FUNCTION_CONTEXT 64
#define TRDB_INLINES 128

struct trdb_ctx;

/**
 * Store disassembly configuration and context.
 */
struct trdb_disasm_aux {
    /* current disassembly context */
    bfd *abfd;
    asection *sec;
    bfd_boolean require_sec;

    arelent **dynrelbuf;
    long dynrelcount;

    arelent *reloc;

    /* symbols extracted from bfd */
    asymbol **symbols;
    long symcount;
    asymbol **dynamic_symbols;
    long dynsymcount;
    asymbol *synthethic_symbols;
    long synthcount;
    asymbol **sorted_symbols;
    long sorted_symcount;

    uint32_t config;       /**< stores the below settings as bitfield */
    bool no_aliases;       /**< set to true to always diassemble to most general
                              representation */
    bool prefix_addresses; /**< different address format */
    bool do_demangle;      /**< demangle symbols using bfd */
    bool display_file_offsets;  /**< display file offsets for displayed symbols
                                 */
    bool with_line_numbers;     /**< periodically show line numbers mixed with
                                   assembly */
    bool with_source_code;      /**< show source code mixed with assembly */
    bool with_function_context; /**< show when instruction lands on symbol value
                                   of a function */
    bool unwind_inlines;        /**< Print all inlines for source line */
};

/* The number of zeroes we want to see before we start skipping them. The number
 * is arbitrarily chosen.
 */

#define DEFAULT_SKIP_ZEROES 8

/* The number of zeroes to skip at the end of a section. If the number of zeroes
 * at the end is between SKIP_ZEROES_AT_END and SKIP_ZEROES, they will be
 * disassembled. If there are fewer than SKIP_ZEROES_AT_END, they will be
 * skipped. This is a heuristic attempt to avoid disassembling zeroes inserted
 * by section alignment.
 */

#define DEFAULT_SKIP_ZEROES_AT_END 3

/**
 * Initialize disassemble_info from libopcodes with hardcoded values for the
 * PULP platform.
 * @param dinfo filled with information of the PULP platform
 */
void trdb_init_disassemble_info_for_pulp(struct disassemble_info *dinfo);

/**
 * Initialize disassembler_unit with settings for the PULP platform.
 * @param dunit filled with information for PULP
 * @param options configure the formatting behaviour or NULL
 * @return 0 on success, -1 otherwise
 */
int trdb_init_disassembler_unit_for_pulp(struct disassembler_unit *dunit,
                                         char *options);

/**
 * Initialize disassemble_info from libopcodes by grabbing data out of @p abfd.
 *
 * The options string can be non-@c NULL to pass disassembler specific settings
 * to libopcodes. Currently supported is "no-aliases", which disassembles common
 * abbreviations for certain instructions.
 *
 * @param dinfo filled with information from @p abfd
 * @param abfd the bfd representing the binary
 * @param options disassembly options passed to libopcodes
 */
void trdb_init_disassemble_info_from_bfd(struct disassemble_info *dinfo,
                                         bfd *abfd, char *options);

/**
 * Initialize disassembler_unit.
 *
 * A side from calling init_disassemble_info_from_bfd(), it also sets
 * #disassembler_unit.disassemble_fn to the architecture matching @p abfd.
 *
 * @param dunit filled with information from @p abfd
 * @param abfd the bfd representing the binary
 * @param options disassembly options passed to libopcodes
 * @return 0 on success, a negative error code otherwise
 * @return -trdb_invalid if @p dunit or #disassembler_unit.dinfo in @p
 * dunit is NULL
 * @return -trdb_arch_support if architecture is not supported
 */
int trdb_init_disassembler_unit(struct disassembler_unit *dunit, bfd *abfd,
                                char *options);

/**
 * Initialize disassembler_unit and its containing disassemble_info from
 * libopcodes by grabbing meta data out of @p abfd. Furthermore it allocates
 * internal structures to allow #disassembler_unit.disassemble_fn in @p
 * dunit to resolve addresses to nearest symbols. TODO: trdb_ctx sets demangle,
 * disassembly options and more.
 *
 * @param c the trace debugger context containing settings
 * @param abfd the bfd representing the binary
 * @param dunit filled with information from @p abfd
 * @return 0 on success, a negative error code otherwise
 * @return -trdb_invalid if @p c, @p abfd or @p dunit is NULL
 * @return -trdb_nomem if out of memory
 * @return -trdb_arch_support if architecture is not supported
 */
int trdb_alloc_dinfo_with_bfd(struct trdb_ctx *c, bfd *abfd,
                              struct disassembler_unit *dunit);
/**
 * Free the memory allocated to @p abfd and @p dunit by a call to
 * trdb_alloc_dinfo_with_bfd().
 *
 * @param c the trace debugger context containing settings
 * @param abfd the bfd representing the binary
 * @param dunit filled with information from @p abfd
 */
void trdb_free_dinfo_with_bfd(struct trdb_ctx *c, bfd *abfd,
                              struct disassembler_unit *dunit);

/**
 * Configure the disassembler with flags. E.g. set @p settings to
 * TRDB_SOURCE_CODE | TRDB_LINE_NUMBERS to show the source code and file line
 * numbers mixed with disassembly.
 *
 * @param dunit the disassembler
 * @param settings configuration flags which can be set as described
 */
void trdb_set_disassembly_conf(struct disassembler_unit *dunit,
                               uint32_t settings);

/**
 * Read the disassembler configuration flags. Test for enabled features by
 * and'ing the returned value with e.g. TRDB_SOURCE_CODE.
 *
 * @param dunit the disassembler whose settings should be read
 * @param conf written with the disassembly configuration if successfull
 * @return 0 on success, a negative error code otherwise
 * @return -trdb_invalid if trdb_disasm_aux in @p dunit is NULL
 */
int trdb_get_disassembly_conf(struct disassembler_unit *dunit, uint32_t *conf);

/**
 * A #print_address_func used in disassemble_info, set by
 * trdb_alloc_dinfo_with_bfd(), which resolve addresses to symbols. This
 * callback is only well defined if its disassemble_info struct was initialized
 * using trdb_alloc_dinfo_with_bfd().
 *
 * This is a callback function, if registered, called by libopcodes to custom
 * format addresses. It can also be abused to print other information.
 *
 * @param vma the virtual memory address where this instruction is located at
 * @param inf context of disassembly
 */
void trdb_print_address(bfd_vma vma, struct disassemble_info *inf);

/**
 * A #symbol_at_address function used in disassemble_info, set by
 * trdb_alloc_dinfo_with_bfd().
 *
 * This is a callback function, which gives libopcodes information about
 * specific addresses. Determines if the given address has a symbol associated
 * with it.
 *
 * @param vma the virtual memory address where this instruction is located at
 * @param inf context of disassembly
 * @return 1 if there is a symbol at @p vma, 0 otherwise
 */
int trdb_symbol_at_address(bfd_vma vma, struct disassemble_info *inf);

/**
 *  Print the bfd section header to stdout.
 *
 * @param abfd the bfd representing the binary
 * @param section which section to print
 * @param ignored is ignored and only for compatiblity
 */
void trdb_dump_section_header(bfd *abfd, asection *section, void *ignored);

/**
 * Print bfd architecuture information to stdout.
 *
 * @param abfd the bfd representing the binary
 */
void trdb_dump_bin_info(bfd *abfd);

/**
 * Print all section names of bfd to stdout.
 *
 * @param abfd the bfd representing the binary
 */
void trdb_dump_section_names(bfd *abfd);

/**
 * Print all supported targets of the used libopcodes to stdout.
 */
void trdb_dump_target_list(void);

/**
 * Default #print_address_func used in disassemble_info, set by
 * init_disassemble_info_for_pulp() or init_disassemble_info_from_bfd().
 *
 * This is a callback function, if registered, called by libopcodes to custom
 * format addresses. It can also be abused to print other information.
 *
 * @param vma the virtual memory address where this instruction is located at
 * @param dinfo context of disassembly
 */
void trdb_riscv32_print_address(bfd_vma vma, struct disassemble_info *dinfo);

/**
 * Return if vma is contained in the given section of abfd.
 *
 * @param abfd the bfd representing the binary
 * @param section the section to test against
 * @param vma the virtual memory address to test for
 * @return whether @p vma is in contained in @p section
 */
bool trdb_vma_in_section(bfd *abfd, asection *section, bfd_vma vma);

/**
 * Return the section of @p abfd that contains the given @p vma or @c NULL.
 *
 * @param abfd the bfd representing the binary
 * @param vma the virtual memory address to
 * @return the section which contains @p vma or @c NULL
 */
asection *trdb_get_section_for_vma(bfd *abfd, bfd_vma vma);

/**
 * Disassemble the given asection and print it calling fprintf_func.
 *
 * @p inf must point to a disassembler_unit.
 *
 * @param abfd the bfd representing the binary
 * @param section the section to disassemble
 */
void trdb_disassemble_section(bfd *abfd, asection *section, void *inf);

/**
 * Disassemble @p len bytes of data pointed to by @p data with the given
 * @p dunit and print it calling fprintf_func.
 *
 * @param len number of bytes in @p data
 * @param data raw data block to disassemble
 * @param dunit disassembly context
 */
void trdb_disassemble_block(size_t len, bfd_byte *data,
                            struct disassembler_unit *dunit);

/**
 * Disassemble the given instr with the pretended address and disassembler_unit
 * and print it calling fprintf_func.
 *
 * @param instr the raw instruction value up to 64 bits
 * @param addr the address where the instruction is located at
 * @param dunit disassembly context
 */
void trdb_disassemble_single_instruction(insn_t instr, addr_t addr,
                                         struct disassembler_unit *dunit);

/**
 * Disassemble the given instr with the pretended address and print it calling
 * fprintf_func. Will configure a disassembler with default settings for the
 * PULP platform. This function is slow since we create a dissasembler for each
 * call. It is to be used sparingly for single instructions.
 *
 * @param instr the raw instruction value up to 32 bits
 * @param addr the address where the instruction is located at
 */
void trdb_disassemble_single_instruction_slow(insn_t instr, addr_t addr);

/**
 * Disassemble the given instruction, indicated by its address @p addr and the
 * bfd @p abfd which contains it, using the settings in @p dunit and print it by
 * calling fprintf_func.
 *
 * @param c the trace debugger context
 * @param abfd the bfd which contains the instruction
 * @param addr the address of the instruction
 * @param dunit disassembly context
 */
void trdb_disassemble_instruction_with_bfd(struct trdb_ctx *c, bfd *abfd,
                                           bfd_vma addr,
                                           struct disassembler_unit *dunit);

#endif

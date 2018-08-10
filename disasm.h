/*
 * trdb - Trace Debugger Software for the PULP platform
 *
 * Copyright (C) 2018 ETH Zurich and University of Bologna
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

/*
 * Author: Robert Balas (balasr@student.ethz.ch)
 * Description: Collection of disassembler routines using libopcodes and libbfd
 */


#ifndef __DISASM_H__
#define __DISASM_H__

#define PACKAGE "foo" /* quick hack for bfd if not using autotools */
#include <stdbool.h>
#include <inttypes.h>
#include "bfd.h"
#include "dis-asm.h"

/* TODO: berkley */
#define DECLARE_INSN(code, match, mask)                                        \
    static const uint32_t match_##code = match;                                \
    static const uint32_t mask_##code = mask;                                  \
    static bool is_##code##_instr(long instr)                                  \
    {                                                                          \
        return (instr & mask) == match;                                        \
    }

#include "riscv_encoding.h"
#undef DECLARE_INSN

struct disassembler_unit {
    disassembler_ftype disassemble_fn;
    struct disassemble_info *dinfo;
};


static inline unsigned int riscv_instr_len(uint64_t instr)
{
    if ((instr & 0x3) != 0x3) /* RVC.  */
        return 2;
    if ((instr & 0x1f) != 0x1f) /* Base ISA and extensions in 32-bit space.  */
        return 4;
    if ((instr & 0x3f) == 0x1f) /* 48-bit extensions.  */
        return 6;
    if ((instr & 0x7f) == 0x3f) /* 64-bit extensions.  */
        return 8;
    /* Longer instructions not supported at the moment.  */
    return 2;
}

void init_disassemble_info_for_pulp(struct disassemble_info *dinfo);

int init_disassemble_info_from_bfd(struct disassemble_info *dinfo, bfd *abfd,
                                   char *options);

int init_disassembler_unit(struct disassembler_unit *dunit, bfd *abfd,
                           char *options);

void dump_section_header(bfd *, asection *, void *);

void dump_bin_info(bfd *);

void riscv32_print_address(bfd_vma, struct disassemble_info *);

asymbol *find_symbol_at_address(bfd_vma, struct disassemble_info *);

void dump_section_names(bfd *);

void dump_target_list();

bool vma_in_section(bfd *abfd, asection *section, bfd_vma vma);

asection *get_section_for_vma(bfd *abfd, bfd_vma vma);

void disassemble_section(bfd *, asection *, void *);

void disassemble_block(bfd_byte *, size_t, struct disassembler_unit *);

void disassemble_single_instruction(uint32_t, uint32_t,
                                    struct disassembler_unit *);

#endif

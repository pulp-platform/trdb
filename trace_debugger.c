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

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <endian.h> /* Non-standard (posix), use <arpa/inet.h> instead? */
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <stdarg.h>
#include "trace_debugger.h"
#include "disassembly.h"
#include "list.h"
#include "util.h"

/* Configuration state of the trace debugger, used to guide the compression and
 * decompression routine
 */
struct trdb_config {
    /* TODO: inspect full-address, iaddress-lsb-p, implicit-except, set-trace */
    uint64_t resync_max;
    bool full_address;
    bool iaddress_lsb_p;
    bool implicit_except;
    bool set_trace;
    /* set to true to always diassemble to most general representation */
    bool no_aliases;
    /* decoder can assume interrupts are nested properly */
    bool assume_nested_interrupts;
};
static struct trdb_config conf = {0};

/* Records the state of the CPU. The compression routine looks at a sequence of
 * recorded or streamed instructions (struct tr_instr) to figure out the
 * entries of this struct. Based on those it decides when to emit packets.
 */
struct trdb_state {

    bool halt;           /* TODO: update this */
    bool unhalted;       /* TODO: handle halt? */
    bool context_change; /* TODO: ?? */

    bool qualified;
    bool unqualified;
    bool exception;
    bool unpred_disc; /* unpredicted discontinuity*/
    bool emitted_exception_sync;

    uint32_t privilege;
    bool privilege_change;

    uint32_t instr;
    uint32_t addr;
    bool compressed;
};

static struct trdb_state lastc = {0};
static struct trdb_state thisc = {0};
static struct trdb_state nextc = {0};

/* Current state and context of the decompression. */
struct trdb_comp_state {
    struct trdb_state *lastc;
    struct trdb_state *thisc;
    struct trdb_state *nextc;
};

/* Current state of the cpu during decompression. Allows one to
 * precisely emit a sequence  tr_instr. Handles the exception stack
 * (well here we assume that programs actually do nested exception
 * handling properly) and hardware loops (only the TODO: immediate
 * version for now).
 */
/* TODO: Note on hw loops interrupted by an interrupt: look at after
 * the interrupt how many instructions of the loop are executed to
 * figure out in which loop number we got interrupted
 */
struct trdb_dec_state {
    /* absolute loop addresses */
    uint32_t lp_start0;
    uint32_t lp_end0;
    uint32_t lp_cnt0;
    uint32_t lp_start1;
    uint32_t lp_end1;
    uint32_t lp_cnt1;
    /* nested interrupt stacks for each privilege mode*/
    /* uint32_t m_exc_stack; */
    /* uint32_t s_exc_stack; */
    /* uint32_t u_exc_stack; */
    /* record current privilege level */
    uint32_t privilege : PRIVLEN;
};


/* TODO: interrupt stack (nested interrupts) for decompression? */

/* Responsible to hold current branch state, that is the sequence of taken/not
 * taken branches so far. The bits field keeps track of that by setting the
 * cnt'th bit to 1 or 0 for a taken or not taken branch respectively. There can
 * be at most 31 entries in the branch map. A full branch map has the full flag
 * set.
 */
struct branch_map_state {
    bool full;
    uint32_t bits;
    uint32_t cnt;
};

static struct branch_map_state branch_map = {0};

/* We don't want to record all the time and this struct is used to indicate when
 * we do.
 */
struct filter_state {
    /* TODO: look at those variables */
    uint64_t resync_cnt;
    bool resync_pend;
    /* uint32_t resync_nh = 0;  */
};

static struct filter_state filter = {0};


static bool is_branch(uint32_t instr)
{
    /* static bool is_##code##_instr(long insn) */
    /* TODO: add rvc instr */
    assert((instr & 3) == 3);
    bool is_riscv_branch = is_beq_instr(instr) || is_bne_instr(instr)
                           || is_blt_instr(instr) || is_bge_instr(instr)
                           || is_bltu_instr(instr) || is_bgeu_instr(instr);
    bool is_pulp_branch = is_p_bneimm_instr(instr) || is_p_beqimm_instr(instr);
    return is_riscv_branch || is_pulp_branch;
    /* auipc */
}


static bool branch_taken(bool before_compressed, uint32_t addr_before,
                         uint32_t addr_after)
{
    /* can this cause issues with RVC + degenerate jump (+2)? -> yes*/
    /* TODO: this definitely doens't work for 64 bit instructions */
    /* since we have already decompressed instructions, but still compressed
     * addresses we need this additional flag to tell us what the isntruction
     * originally was. So we can't tell by looking at the lower two bits of
     * instr.
     */
    return before_compressed ? !(addr_before + 2 == addr_after)
                             : !(addr_before + 4 == addr_after);
}


/* Some jumps can't be predicted i.e. the jump address can only be figured out
 * at runtime. That happens e.g. if the target address depends on some register
 * entries. For plain RISC-V I this is just the jalr instruction. For the PULP
 * extensions, the hardware loop instruction have to be considered too.
 */
static bool is_unpred_discontinuity(uint32_t instr)
{
    /* Should also work just fine with rvc instructions */
    return is_jalr_instr(instr) || is_mret_instr(instr) || is_sret_instr(instr)
           || is_uret_instr(instr);
}


/* Just crash and error if we hit one of those */
static bool is_unsupported(uint32_t instr)
{
    return is_lp_setup_instr(instr) || is_lp_counti_instr(instr)
           || is_lp_count_instr(instr) || is_lp_endi_instr(instr)
           || is_lp_starti_instr(instr) || is_lp_setupi_instr(instr);
}


void trdb_init()
{
    conf = (struct trdb_config){
        .resync_max = UINT64_MAX, .full_address = true, .no_aliases = true};
    lastc = (struct trdb_state){0};
    thisc = (struct trdb_state){0};
    nextc = (struct trdb_state){0};
    branch_map = (struct branch_map_state){0};
    filter = (struct filter_state){0};
}


void trdb_close()
{
}


struct trdb_config *trdb_alloc_config()
{
    struct trdb_config *config = malloc(sizeof(*config));
    if (!config) {
        perror("malloc");
        return NULL;
    }
    *config = (struct trdb_config){
        .resync_max = UINT64_MAX, .full_address = true, .no_aliases = true};
    return config;
}


void trdb_free_config(struct trdb_config *config)
{
    free(config);
}


struct trdb_comp_state *trdb_alloc_state()
{
    struct trdb_comp_state *state = malloc(sizeof(*state));
    struct trdb_state *nextc = malloc(sizeof(*nextc));
    struct trdb_state *thisc = malloc(sizeof(*thisc));
    struct trdb_state *lastc = malloc(sizeof(*lastc));
    if (!state || !nextc || !thisc || !lastc) {
        perror("malloc");
        return NULL;
    }
    *state = (struct trdb_comp_state){0};
    *lastc = (struct trdb_state){0};
    *thisc = (struct trdb_state){0};
    *nextc = (struct trdb_state){0};
    state->lastc = lastc;
    state->thisc = thisc;
    state->nextc = nextc;
    return state;
}


void trdb_free_state(struct trdb_comp_state *state)
{
    /* lets not dereference a potential nullpointer */
    if (!state)
        return;
    free(state->lastc);
    free(state->thisc);
    free(state->nextc);
    free(state);
}


/* int trdb_compress_trace_single(struct trdb_comp_state *state, */
/*                                struct trdb_config *config, */
/*                                struct list_head *packet_list, */
/*                                struct tr_instr* instr) */
/* { */
/*     if (!state || !config || !packet_list || !instr_list) { */
/*         LOG_ERR("Passed a nullpointer, invalid argument\n"); */
/*         return -1; */
/*     } */

/*     bool full_address = config->full_address; */
/*     struct trdb_state *lastc = state->lastc; */
/*     struct trdb_state *thisc = state->thisc; */
/*     struct trdb_state *nextc = state->nextc; */

/*     /\* for each cycle *\/ */
/*     for (size_t i = 0; i < len; i++) { */
/*         thisc.halt = false; */
/*         /\* test for qualification by filtering *\/ */
/*         thisc.qualified = true; /\* TODO: implement filtering logic *\/ */
/*         nextc.qualified = true; */

/*         thisc.unqualified = !thisc.qualified; */
/*         nextc.unqualified = !nextc.qualified; */

/*         /\* Update state TODO: maybe just ignore last sample instead?*\/ */
/*         thisc.exception = instrs[i].exception; */
/*         nextc.exception = i < len ? instrs[i + 1].exception :
 * thisc.exception; */

/*         thisc.unpred_disc = is_unpred_discontinuity(instrs[i].instr); */
/*         nextc.unpred_disc = */
/*             i < len ? is_unpred_discontinuity(instrs[i + 1].instr) : false;
 */

/*         thisc.privilege = instrs[i].priv; */
/*         nextc.privilege = i < len ? instrs[i + 1].priv : thisc.privilege; */

/*         thisc.privilege_change = (thisc.privilege != lastc.privilege); */
/*         nextc.privilege_change = (thisc.privilege != nextc.privilege); */
/*         /\* TODO: clean this up, proper initial state per round required *\/
 */
/*         thisc.emitted_exception_sync = false; */

/*         bool firstc_qualified = !lastc.qualified && thisc.qualified; */

/*         /\* Start of one cycle *\/ */
/*         if (!thisc.qualified) { */
/*             /\* check if we even need to record anything *\/ */
/*             lastc = thisc; */
/*             continue; /\* end of cycle *\/ */
/*         } */

/*         if (is_unsupported(instrs[i].instr)) { */
/*             LOG_ERR("Instruction is not supported for compression: 0x%"
 * PRIx32 */
/*                     " at addr: 0x%" PRIx32 "\n", */
/*                     instrs[i].instr, instrs[i].iaddr); */
/*             goto fail; */
/*         } */

/*         if (filter.resync_cnt++ == conf.resync_max) { */
/*             filter.resync_pend = true; */
/*             filter.resync_cnt = 0; */
/*         } */

/*         if (is_branch(instrs[i].instr)) { */
/*             /\* update branch map *\/ */
/*             /\* in hardware maybe mask and compare is better ? *\/ */
/*             if ((i + 1 < len) && branch_taken(instrs[i], instrs[i + 1])) */
/*                 branch_map.bits = branch_map.bits | (1u << branch_map.cnt);
 */
/*             branch_map.cnt++; */
/*             if (branch_map.cnt == 31) { */
/*                 branch_map.full = true; */
/*             } */
/*         } */
/*         /\* We trace the packet before the trapped instruction and the */
/*          * first one of the exception handler */
/*          *\/ */
/*         if (lastc.exception) { */
/*             /\* Send te_inst: */
/*              * format 3 */
/*              * subformat 1 (exception -> all fields present) */
/*              * resync_pend = 0 */
/*              *\/ */
/*             ALLOC_INIT_PACKET(tr); */
/*             tr->format = F_SYNC; /\* sync *\/ */
/*             tr->subformat = 1;   /\* exception *\/ */
/*             tr->context = 0;     /\* TODO: what comes here? *\/ */
/*             tr->privilege = instrs[i].priv; */
/*             /\* TODO: actually we should clear the branch map if we really */
/*              * fill out this entry, else we have recorded twice (in the next
 */
/*              * packet) */
/*              *\/ */
/*             if (is_branch(instrs[i].instr) */
/*                 && !branch_taken(instrs[i], instrs[i + 1])) */
/*                 tr->branch = 1; */
/*             else */
/*                 tr->branch = 0; */

/*             tr->address = instrs[i].iaddr; */
/*             /\* With this packet we record last cycles exception */
/*              * information. It's not possible for (i==0 && */
/*              * lastc_exception) to be true since it takes one cycle */
/*              * for lastc_exception to change */
/*              *\/ */
/*             assert(i != 0); */
/*             tr->ecause = instrs[i - 1].cause; */
/*             tr->interrupt = instrs[i - 1].interrupt; */
/*             tr->tval = instrs[i - 1].tval; */
/*             list_add(&tr->list, packet_list); */

/*             thisc.emitted_exception_sync = true; */
/*             filter.resync_pend = false; /\* TODO: how to handle this *\/ */
/*             /\* end of cycle *\/ */

/*         } else if (lastc.emitted_exception_sync) { */
/*             /\* First we assume that the vector table entry is a jump. Since
 * that */
/*              * entry can change during runtime, we need to emit the jump */
/*              * destination address, which is the second instruction of the
 * trap */
/*              * handler. This a bit hacky and made to work for the PULP. If */
/*              * someone puts something else than a jump instruction there then
 */
/*              * all bets are off. This is a custom change. */
/*              * TODO: merge this with lastc.unpred_discon */
/*              *\/ */
/*             /\* Send te_inst: */
/*              * format 0/1/2 */
/*              *\/ */
/*             ALLOC_INIT_PACKET(tr); */
/*             /\* TODO: for now only full address *\/ */
/*             if (!full_address) { */
/*                 LOG_ERR("full_address false: Not implemented yet\n"); */
/*                 goto fail; */
/*             } */
/*             if (branch_map.cnt == 0) { */
/*                 tr->format = F_ADDR_ONLY; */
/*                 tr->address = full_address ? instrs[i].iaddr : 0; */

/*                 assert(branch_map.bits == 0); */
/*             } else { */
/*                 tr->format = full_address ? F_BRANCH_FULL : F_BRANCH_DIFF; */
/*                 tr->branches = branch_map.cnt; */
/*                 tr->branch_map = branch_map.bits; */
/*                 tr->address = full_address ? instrs[i].iaddr : 0; */

/*                 branch_map = (struct branch_map_state){0}; */
/*             } */
/*             list_add(&tr->list, packet_list); */

/*             /\* end of cycle *\/ */

/*         } else if (firstc_qualified || thisc.unhalted ||
 * thisc.privilege_change */
/*                    || (filter.resync_pend && branch_map.cnt == 0)) { */

/*             /\* Start packet *\/ */
/*             /\* Send te_inst: */
/*              * format 3 */
/*              * subformat 0 (start, no ecause, interrupt and tval) */
/*              * resync_pend = 0 */
/*              *\/ */
/*             ALLOC_INIT_PACKET(tr); */
/*             tr->format = F_SYNC; /\* sync *\/ */
/*             tr->subformat = 0;   /\* start *\/ */
/*             tr->context = 0;     /\* TODO: what comes here? *\/ */
/*             tr->privilege = instrs[i].priv; */
/*             if (is_branch(instrs[i].instr) */
/*                 && !branch_taken(instrs[i], instrs[i + 1])) */
/*                 tr->branch = 1; */
/*             else */
/*                 tr->branch = 0; */
/*             tr->address = instrs[i].iaddr; */
/*             list_add(&tr->list, packet_list); */

/*             filter.resync_pend = false; */
/*             /\* end of cycle *\/ */

/*         } else if (lastc.unpred_disc) { */
/*             /\* Send te_inst: */
/*              * format 0/1/2 */
/*              *\/ */
/*             ALLOC_INIT_PACKET(tr); */
/*             /\* TODO: for now only full address *\/ */
/*             if (!full_address) { */
/*                 LOG_ERR("full_address false: Not implemented yet\n"); */
/*                 goto fail; */
/*             } */
/*             if (branch_map.cnt == 0) { */
/*                 tr->format = F_ADDR_ONLY; */
/*                 tr->address = full_address ? instrs[i].iaddr : 0; */

/*                 assert(branch_map.bits == 0); */
/*             } else { */
/*                 tr->format = full_address ? F_BRANCH_FULL : F_BRANCH_DIFF; */
/*                 tr->branches = branch_map.cnt; */
/*                 tr->branch_map = branch_map.bits; */
/*                 tr->address = instrs[i].iaddr; */

/*                 branch_map = (struct branch_map_state){0}; */
/*             } */
/*             list_add(&tr->list, packet_list); */

/*             /\* end of cycle *\/ */

/*         } else if (filter.resync_pend && branch_map.cnt > 0) { */
/*             /\* we treat resync_pend && branches == 0 before *\/ */
/*             /\* Send te_inst: */
/*              * format 0/1/2 */
/*              *\/ */
/*             ALLOC_INIT_PACKET(tr); */
/*             /\* TODO: for now only full address *\/ */
/*             if (!full_address) { */
/*                 LOG_ERR("full_address false: Not implemented yet\n"); */
/*                 goto fail; */
/*             } */
/*             tr->format = full_address ? F_BRANCH_FULL : F_BRANCH_DIFF; */
/*             tr->branches = branch_map.cnt; */
/*             tr->branch_map = branch_map.bits; */
/*             tr->address = full_address ? instrs[i].iaddr : 0; */
/*             list_add(&tr->list, packet_list); */

/*             branch_map = (struct branch_map_state){0}; */
/*             /\* end of cycle *\/ */

/*         } else if (nextc.halt || nextc.exception || nextc.privilege_change */
/*                    || nextc.unqualified) { */
/*             /\* Send te_inst: */
/*              * format 0/1/2 */
/*              *\/ */
/*             ALLOC_INIT_PACKET(tr); */
/*             /\* TODO: for now only full address *\/ */
/*             if (!full_address) { */
/*                 LOG_ERR("full_address false: Not implemented yet\n"); */
/*                 goto fail; */
/*             } */
/*             if (branch_map.cnt == 0) { */
/*                 tr->format = F_ADDR_ONLY; */
/*                 tr->address = full_address ? instrs[i].iaddr : 0; */

/*                 assert(branch_map.bits == 0); */
/*             } else { */
/*                 tr->format = full_address ? F_BRANCH_FULL : F_BRANCH_DIFF; */
/*                 tr->branches = branch_map.cnt; */
/*                 tr->branch_map = branch_map.bits; */
/*                 tr->address = full_address ? instrs[i].iaddr : 0; */

/*                 branch_map = (struct branch_map_state){0}; */
/*             } */
/*             list_add(&tr->list, packet_list); */

/*             /\* end of cycle *\/ */

/*         } else if (branch_map.full) { */
/*             /\* Send te_inst: */
/*              * format 0 */
/*              * no address */
/*              *\/ */
/*             assert(branch_map.cnt == 31); */
/*             ALLOC_INIT_PACKET(tr); */
/*             tr->format = F_BRANCH_FULL; */
/*             tr->branches = branch_map.cnt; */
/*             tr->branch_map = branch_map.bits; */
/*             /\* tr->address  TODO: no address, study explanation *\/ */
/*             list_add(&tr->list, packet_list); */

/*             branch_map = (struct branch_map_state){0}; */
/*             /\* end of cycle *\/ */

/*         } else if (thisc.context_change) { */
/*             /\* TODO: don't understand how to detect context change *\/ */
/*             /\* Send te_inst: */
/*              * format 3 */
/*              * subformat 2 */
/*              *\/ */
/*             ALLOC_INIT_PACKET(tr); */
/*             tr->format = F_SYNC; */
/*             tr->subformat = 2; */
/*             tr->context = 0; /\* TODO: what comes here? *\/ */
/*             tr->privilege = instrs[i].priv; */
/*             /\* tr->branch *\/ */
/*             /\* tr->address *\/ */
/*             /\* tr->ecause *\/ */
/*             /\* tr->interrupt *\/ */
/*             /\* tr->tval *\/ */
/*             list_add(&tr->list, packet_list); */
/*         } */

/*         /\* update last cycle state *\/ */
/*         lastc = thisc; */
/*     } */
/*     return packet_list; */
/* fail: */
/* fail_malloc: */
/*     trdb_free_packet_list(packet_list); */
/*     return NULL; */
/* } */


struct list_head *trdb_compress_trace(struct list_head *packet_list, size_t len,
                                      struct tr_instr instrs[len])
{
    bool full_address = conf.full_address;

    /* for each cycle */
    for (size_t i = 0; i < len; i++) {
        thisc.halt = false;
        /* test for qualification by filtering */
        /* TODO: implement filtering logic */

        thisc.qualified = true;
        nextc.qualified = true;

        thisc.unqualified = !thisc.qualified;
        nextc.unqualified = !nextc.qualified;

        /* Update state TODO: maybe just ignore last sample instead?*/
        thisc.exception = instrs[i].exception;
        nextc.exception = i < len ? instrs[i + 1].exception : thisc.exception;

        thisc.unpred_disc = is_unpred_discontinuity(instrs[i].instr);
        nextc.unpred_disc =
            i < len ? is_unpred_discontinuity(instrs[i + 1].instr) : false;

        thisc.privilege = instrs[i].priv;
        nextc.privilege = i < len ? instrs[i + 1].priv : thisc.privilege;

        thisc.privilege_change = (thisc.privilege != lastc.privilege);
        nextc.privilege_change = (thisc.privilege != nextc.privilege);
        /* TODO: clean this up, proper initial state per round required */
        thisc.emitted_exception_sync = false;

        bool firstc_qualified = !lastc.qualified && thisc.qualified;

        /* Start of one cycle */
        if (!thisc.qualified) {
            /* check if we even need to record anything */
            lastc = thisc;
            thisc = nextc;
            continue; /* end of cycle */
        }

        if (is_unsupported(instrs[i].instr)) {
            LOG_ERR("Instruction is not supported for compression: 0x%" PRIx32
                    " at addr: 0x%" PRIx32 "\n",
                    instrs[i].instr, instrs[i].iaddr);
            goto fail;
        }

        if (filter.resync_cnt++ == conf.resync_max) {
            filter.resync_pend = true;
            filter.resync_cnt = 0;
        }

        if (is_branch(instrs[i].instr)) {
            /* update branch map */
            /* in hardware maybe mask and compare is better ? */
            if ((i + 1 < len)
                && branch_taken(instrs[i].compressed, instrs[i].iaddr,
                                instrs[i + 1].iaddr))
                branch_map.bits = branch_map.bits | (1u << branch_map.cnt);
            branch_map.cnt++;
            if (branch_map.cnt == 31) {
                branch_map.full = true;
            }
        }
        /* We trace the packet before the trapped instruction and the
         * first one of the exception handler
         */
        if (lastc.exception) {
            /* Send te_inst:
             * format 3
             * subformat 1 (exception -> all fields present)
             * resync_pend = 0
             */
            ALLOC_INIT_PACKET(tr);
            tr->format = F_SYNC; /* sync */
            tr->subformat = 1;   /* exception */
            tr->context = 0;     /* TODO: what comes here? */
            tr->privilege = instrs[i].priv;
            /* TODO: actually we should clear the branch map if we really
             * fill out this entry, else we have recorded twice (in the next
             * packet)
             */
            if (is_branch(instrs[i].instr)
                && !branch_taken(instrs[i].compressed, instrs[i].iaddr,
                                 instrs[i + 1].iaddr))
                tr->branch = 1;
            else
                tr->branch = 0;

            tr->address = instrs[i].iaddr;
            /* With this packet we record last cycles exception
             * information. It's not possible for (i==0 &&
             * lastc_exception) to be true since it takes one cycle
             * for lastc_exception to change
             */
            assert(i != 0);
            tr->ecause = instrs[i - 1].cause;
            tr->interrupt = instrs[i - 1].interrupt;
            tr->tval = instrs[i - 1].tval;
            list_add(&tr->list, packet_list);

            thisc.emitted_exception_sync = true;
            filter.resync_pend = false; /* TODO: how to handle this */
            /* end of cycle */

        } else if (lastc.emitted_exception_sync) {
            /* First we assume that the vector table entry is a jump. Since that
             * entry can change during runtime, we need to emit the jump
             * destination address, which is the second instruction of the trap
             * handler. This a bit hacky and made to work for the PULP. If
             * someone puts something else than a jump instruction there then
             * all bets are off. This is a custom change.
             * TODO: merge this with lastc.unpred_discon
             */
            /* Send te_inst:
             * format 0/1/2
             */
            ALLOC_INIT_PACKET(tr);
            /* TODO: for now only full address */
            if (!full_address) {
                LOG_ERR("full_address false: Not implemented yet\n");
                goto fail;
            }
            if (branch_map.cnt == 0) {
                tr->format = F_ADDR_ONLY;
                tr->address = full_address ? instrs[i].iaddr : 0;

                assert(branch_map.bits == 0);
            } else {
                tr->format = full_address ? F_BRANCH_FULL : F_BRANCH_DIFF;
                tr->branches = branch_map.cnt;
                tr->branch_map = branch_map.bits;
                tr->address = full_address ? instrs[i].iaddr : 0;

                branch_map = (struct branch_map_state){0};
            }
            list_add(&tr->list, packet_list);

            /* end of cycle */

        } else if (firstc_qualified || thisc.unhalted || thisc.privilege_change
                   || (filter.resync_pend && branch_map.cnt == 0)) {

            /* Start packet */
            /* Send te_inst:
             * format 3
             * subformat 0 (start, no ecause, interrupt and tval)
             * resync_pend = 0
             */
            ALLOC_INIT_PACKET(tr);
            tr->format = F_SYNC; /* sync */
            tr->subformat = 0;   /* start */
            tr->context = 0;     /* TODO: what comes here? */
            tr->privilege = instrs[i].priv;
            if (is_branch(instrs[i].instr)
                && !branch_taken(instrs[i].compressed, instrs[i].iaddr,
                                 instrs[i + 1].iaddr))
                tr->branch = 1;
            else
                tr->branch = 0;
            tr->address = instrs[i].iaddr;
            list_add(&tr->list, packet_list);

            filter.resync_pend = false;
            /* end of cycle */

        } else if (lastc.unpred_disc) {
            /* Send te_inst:
             * format 0/1/2
             */
            ALLOC_INIT_PACKET(tr);
            /* TODO: for now only full address */
            if (!full_address) {
                LOG_ERR("full_address false: Not implemented yet\n");
                goto fail;
            }
            if (branch_map.cnt == 0) {
                tr->format = F_ADDR_ONLY;
                tr->address = full_address ? instrs[i].iaddr : 0;

                assert(branch_map.bits == 0);
            } else {
                tr->format = full_address ? F_BRANCH_FULL : F_BRANCH_DIFF;
                tr->branches = branch_map.cnt;
                tr->branch_map = branch_map.bits;
                tr->address = instrs[i].iaddr;

                branch_map = (struct branch_map_state){0};
            }
            list_add(&tr->list, packet_list);

            /* end of cycle */

        } else if (filter.resync_pend && branch_map.cnt > 0) {
            /* we treat resync_pend && branches == 0 before */
            /* Send te_inst:
             * format 0/1/2
             */
            ALLOC_INIT_PACKET(tr);
            /* TODO: for now only full address */
            if (!full_address) {
                LOG_ERR("full_address false: Not implemented yet\n");
                goto fail;
            }
            tr->format = full_address ? F_BRANCH_FULL : F_BRANCH_DIFF;
            tr->branches = branch_map.cnt;
            tr->branch_map = branch_map.bits;
            tr->address = full_address ? instrs[i].iaddr : 0;
            list_add(&tr->list, packet_list);

            branch_map = (struct branch_map_state){0};
            /* end of cycle */

        } else if (nextc.halt || nextc.exception || nextc.privilege_change
                   || nextc.unqualified) {
            /* Send te_inst:
             * format 0/1/2
             */
            ALLOC_INIT_PACKET(tr);
            /* TODO: for now only full address */
            if (!full_address) {
                LOG_ERR("full_address false: Not implemented yet\n");
                goto fail;
            }
            if (branch_map.cnt == 0) {
                tr->format = F_ADDR_ONLY;
                tr->address = full_address ? instrs[i].iaddr : 0;

                assert(branch_map.bits == 0);
            } else {
                tr->format = full_address ? F_BRANCH_FULL : F_BRANCH_DIFF;
                tr->branches = branch_map.cnt;
                tr->branch_map = branch_map.bits;
                tr->address = full_address ? instrs[i].iaddr : 0;

                branch_map = (struct branch_map_state){0};
            }
            list_add(&tr->list, packet_list);

            /* end of cycle */

        } else if (branch_map.full) {
            /* Send te_inst:
             * format 0
             * no address
             */
            assert(branch_map.cnt == 31);
            ALLOC_INIT_PACKET(tr);
            tr->format = F_BRANCH_FULL;
            tr->branches = branch_map.cnt;
            tr->branch_map = branch_map.bits;
            /* tr->address  TODO: no address, study explanation */
            list_add(&tr->list, packet_list);

            branch_map = (struct branch_map_state){0};
            /* end of cycle */

        } else if (thisc.context_change) {
            /* TODO: don't understand how to detect context change */
            /* Send te_inst:
             * format 3
             * subformat 2
             */
            ALLOC_INIT_PACKET(tr);
            tr->format = F_SYNC;
            tr->subformat = 2;
            tr->context = 0; /* TODO: what comes here? */
            tr->privilege = instrs[i].priv;
            /* tr->branch */
            /* tr->address */
            /* tr->ecause */
            /* tr->interrupt */
            /* tr->tval */
            list_add(&tr->list, packet_list);
        }

        /* update last cycle state */
        lastc = thisc;
        thisc = nextc;
    }
    return packet_list;
fail:
fail_malloc:
    trdb_free_packet_list(packet_list);
    return NULL;
}


static int disassemble_at_pc(bfd_vma pc, struct tr_instr *instr,
                             struct disassembler_unit *dunit, int *status)
{
    *status = 0;
    /* make sure start froma a clean slate */
    *instr = (struct tr_instr){0};

    struct disassemble_info *dinfo = dunit->dinfo;
    /* Important to set for internal calls to fprintf */
    dinfo->stream = instr;

    /* print instr address */
    (*dinfo->fprintf_func)(instr, "0x%016jx  ", (uintmax_t)pc);

    /* check if insn_info works */
    dinfo->insn_info_valid = 0;

    int instr_size = (*dunit->disassemble_fn)(pc, dinfo);
    (*dinfo->fprintf_func)(instr, "\n");
    if (instr_size <= 0) {
        LOG_ERR("Encountered instruction with %d bytes, stopping\n",
                instr_size);
        *status = -1;
        return 0;
    }
    if (!dinfo->insn_info_valid) {
        LOG_ERR("Encountered invalid instruction info\n");
        *status = -1;
        return 0;
    }

    instr->iaddr = pc;
    /* TODO: add more data */
    /* instr->instr = */
    /* instr->compressed */
    /* instr->priv = 0 */
    return instr_size;
}


static int build_instr_fprintf(void *stream, const char *format, ...)
{
    struct tr_instr *instr = stream;
    char tmp[INSTR_STR_LEN];
    /* ugly hack around libopcodes fprintf-only output */
    va_list args;
    va_start(args, format);
    int rv = vsnprintf(tmp, INSTR_STR_LEN - 1, format, args);
    if (rv >= INSTR_STR_LEN) {
        LOG_ERR("build_instr_fprintf output truncated, adjust buffer size\n");
    }
    if (rv < 0) {
        LOG_ERR("Encountered an error in vsnprintf\n");
    }
    va_end(args);
    if (strlen(instr->str) + rv + 1 > INSTR_STR_LEN) {
        LOG_ERR("Can't append to buffer, truncating string\n");
        strncat(instr->str, tmp, INSTR_STR_LEN - 1 - strlen(instr->str));
    } else {
        strncat(instr->str, tmp, rv);
    }
    /* TODO: make this maybe a debug print */
    /* printf("%s", instr->str); */

    return rv;
}


static void free_section_for_debugging(struct disassemble_info *dinfo)
{
    if (!dinfo)
        return;
    free(dinfo->buffer);
    dinfo->buffer_vma = 0;
    dinfo->buffer_length = 0;
    dinfo->section = NULL;
}


static int alloc_section_for_debugging(bfd *abfd, asection *section,
                                       struct disassemble_info *dinfo)
{
    if (!dinfo || !section) {
        LOG_ERR("disassemble_info or asection is NULL\n");
        return -1;
    }

    bfd_size_type section_size = bfd_get_section_size(section);

    bfd_byte *section_data = malloc(section_size);
    if (!section_data) {
        perror("malloc");
        return -1;
    }

    if (!bfd_get_section_contents(abfd, section, section_data, 0,
                                  section_size)) {
        bfd_perror("bfd_get_section_contents");
        free(section_data);
        return -1;
    }

    dinfo->buffer = section_data;
    dinfo->buffer_vma = section->vma;
    dinfo->buffer_length = section_size;
    dinfo->section = section;
    return 0;
}


static int read_memory_at_pc(bfd_vma pc, uint64_t *instr, unsigned int len,
                             struct disassemble_info *dinfo)
{
    /* return (*dinfo->read_memory_func)(pc, myaddr, len, dinfo); */
    bfd_byte packet[2];
    *instr = 0;
    bfd_vma n;
    int status;

    /* Instructions are a sequence of 2-byte packets in little-endian order.  */
    for (n = 0; n < sizeof(uint64_t) && n < riscv_instr_len(*instr); n += 2) {
        status = (*dinfo->read_memory_func)(pc + n, packet, 2, dinfo);
        if (status != 0) {
            /* Don't fail just because we fell off the end.  */
            if (n > 0)
                break;
            (*dinfo->memory_error_func)(status, pc, dinfo);
            return status;
        }

        (*instr) |= ((uint64_t)bfd_getl16(packet)) << (8 * n);
    }
    return status;
}


static int add_to_trace(struct list_head *instr_list, struct tr_instr *instr)
{
    struct tr_instr *add = malloc(sizeof(*add));
    if (!add) {
        perror("malloc");
        return -1;
    }
    memcpy(add, instr, sizeof(*add));
    list_add(&add->list, instr_list);
    return 0;
}


static bfd_vma advance_pc(bfd_vma pc, int step, struct trdb_dec_state state)
{
    return pc + step;
}


struct list_head *trdb_decompress_trace(bfd *abfd,
                                        struct list_head *packet_list,
                                        struct list_head *instr_list)
{
    /* We assume our hw block in the pulp generated little endian
     * addresses, thus we should make sure that before we interact
     * with bfd addresses to convert this foreign format to the local
     * host format
     */
    /* TODO: supports only statically linked elf executables */
    /* TODO: interrupt/exception support: mret, sret, uret and similar */
    /* TODO: nested interrupt support */
    /* TODO: boot code hack */

    /* find section belonging to start_address */
    bfd_vma start_address = abfd->start_address;
    asection *section = get_section_for_vma(abfd, start_address);
    if (!section) {
        LOG_ERR("VMA not pointing to any section\n");
        goto fail;
    }
    LOG_INFO("Section of start_address:%s\n", section->name);

    struct disassembler_unit dunit = {0};
    struct disassemble_info dinfo = {0};
    struct trdb_dec_state decompression_context = {0};
    struct tr_instr dis_instr = {0};

    dunit.dinfo = &dinfo;
    init_disassembler_unit(&dunit, abfd, conf.no_aliases ? "no-aliases" : NULL);
    /* advanced fprintf output handling */
    dunit.dinfo->fprintf_func = build_instr_fprintf;
    /* dunit.dinfo->stream = &instr; */

    /* Alloc and config section data for disassembler */
    bfd_vma stop_offset = bfd_get_section_size(section) / dinfo.octets_per_byte;

    if (alloc_section_for_debugging(abfd, section, &dinfo)) {
        LOG_ERR("Failed alloc_section_for_debugging\n");
        goto fail;
    }
    bfd_vma pc = start_address; /* TODO: well we get a sync packet anyway... */

    struct branch_map_state branch_map = {0};
    /* we need to be able to look at two packets to make the right
     * decompression decision
     */
    struct tr_packet *last_packet = NULL;
    struct tr_packet *packet = NULL;
    struct tr_packet *next_packet = NULL;

    struct list_head *p;

    list_for_each_prev(p, packet_list)
    {
        packet = list_entry(p, typeof(*packet), list);
        /* this is a bit ugly, we determine the next packet if it exists. Also
         * we have to iterate the list in reverse order
         */
        if (p->prev != packet_list) {
            next_packet = list_entry(p->prev, typeof(*next_packet), list);
        } else {
            next_packet = NULL;
        }
        if (p->next != packet_list) {
            last_packet = list_entry(p->next, typeof(*last_packet), list);
        } else {
            last_packet = NULL;
        }

        /* Sometimes we leave the current section (e.g. changing from
         * the .start to the .text section), so let's load the
         * appropriate section and remove the old one
         */
        if (pc >= section->vma + stop_offset || pc < section->vma) {
            free_section_for_debugging(&dinfo);

            section = get_section_for_vma(abfd, pc);
            if (!section) {
                LOG_ERR("VMA (PC) not pointing to any section\n");
                goto fail;
            }
            stop_offset = bfd_get_section_size(section) / dinfo.octets_per_byte;

            if (alloc_section_for_debugging(abfd, section, &dinfo)) {
                LOG_ERR("Failed alloc_section_for_debugging\n");
                goto fail;
            }
            LOG_INFO("Switched to section:%s\n", section->name);
        }

        switch (packet->format) {
        case F_BRANCH_FULL:
        case F_BRANCH_DIFF:
            branch_map.cnt = packet->branches;
            branch_map.bits = packet->branch_map;
            branch_map.full = packet->branches == 31;
            break;
        case F_SYNC:
            break;
        case F_ADDR_ONLY:
            break;
        default:
            LOG_ERR("Unimplemented\n");
            goto fail;
        }

        /* TODO: change this behaviour */
        if (TRDB_TRACE)
            trdb_print_packet(stdout, packet);
        if (last_packet && last_packet->format == F_SYNC
            && last_packet->subformat == SF_EXCEPTION) {
            /* exceptions come with three packets: flush, F_SYNC, and
             * F_ADDR_ONLY/F_BRANCH_*. The last packet is kinda of a hack to
             * bridge over the vector table jump instruction (which can change
             * at runtime)
             */
            assert(packet->format == F_BRANCH_FULL
                   || packet->format == F_ADDR_ONLY
                   || packet->format == F_BRANCH_DIFF);
            LOG_INFO("Jumping over vector table\n");

            pc = packet->address;

            /* since we are abruptly changing the pc we have to check if we
             * leave the section before we can disassemble. This is really ugly
             * TODO: fix
             */
            if (pc >= section->vma + stop_offset || pc < section->vma) {
                free_section_for_debugging(&dinfo);

                section = get_section_for_vma(abfd, pc);
                if (!section) {
                    LOG_ERR("VMA (PC) not pointing to any section\n");
                    goto fail;
                }
                stop_offset =
                    bfd_get_section_size(section) / dinfo.octets_per_byte;

                if (alloc_section_for_debugging(abfd, section, &dinfo)) {
                    LOG_ERR("Failed alloc_section_for_debugging\n");
                    goto fail;
                }
                LOG_INFO("Switched to section:%s\n", section->name);
            }

            int status = 0;
            int size = disassemble_at_pc(pc, &dis_instr, &dunit, &status);
            if (status != 0)
                goto fail;

            uint64_t instr_at_pc = 0;
            if (read_memory_at_pc(pc, &instr_at_pc, size, &dinfo)) {
                LOG_ERR("Reading instr at pc failed\n");
                goto fail;
            }

            add_to_trace(instr_list, &dis_instr);

            pc += size; /* normal case */

            switch (dinfo.insn_type) {
            case dis_nonbranch:
                /* TODO: we need this hack since {m,s,u} ret are not classified
                 */
                if (!is_unpred_discontinuity(instr_at_pc)) {
                    break;
                }
                LOG_INFO("Detected mret, uret or sret\n");
            case dis_jsr:    /* There is not real difference ... */
            case dis_branch: /* ... between those two */
                if (dinfo.target == 0) {
                    LOG_ERR(
                        "First instruction after vector table is unpredictable discontinuity\n");
                    LOG_ERR("TODO: fix this case by looking at next packet\n");
                    goto fail;
                }
                pc = dinfo.target;
                break;

            case dis_condbranch:
                if (dinfo.target == 0)
                    LOG_ERR("Can't predict the jump target, never happens\n");
                if (packet->branches != 1)
                    LOG_ERR("Wrong number of branch entries\n");
                if (packet->branch_map & 1) {
                    LOG_ERR(
                        "Doing a branch in the first intruction after vector table jump\n");
                    pc = dinfo.target;
                }
                break;

            case dis_dref: /* TODO: is this useful? */
            case dis_dref2:
            case dis_condjsr:
            case dis_noninsn:
                LOG_ERR("Invalid insn_type: %d\n", dinfo.insn_type);
                goto fail;
            }
        } else if (packet->format == F_BRANCH_FULL) {
            /* TODO: not true for discontinuous before last branch*/
            /* bool sync_consumed = branch_map.cnt == 31; */
            /* We have to find the instruction where we can apply the address
             * information to. This might either be a discontinuity infromation
             * or a sync up address. The search_discontinuity
             * variable tells us which one is it
             */
            bool hit_address = false; /* TODO: for now we don't allow
                                       * resync packets, see todo.org
                                       */
            /* We don't need to care about the address field if the branch map
             * is full,(except if full and instr before last branch is
             * discontinuity)
             */
            bool hit_discontinuity = branch_map.full;

            bool search_discontinuity = true;
            if (next_packet == NULL
                || (next_packet->format == F_SYNC
                    && (next_packet->subformat == SF_START
                        || next_packet->subformat == SF_EXCEPTION))) {
                /* advance until address */
                LOG_INFO("Searching for address\n");
                search_discontinuity = false;
            } else {
                /* advance until unpredictable discontinuity */
                LOG_INFO("Searching for discontinuity\n");
                search_discontinuity = true;
            }

            while (
                !(branch_map.cnt == 0 && (hit_discontinuity || hit_address))) {

                int status = 0;
                int size = disassemble_at_pc(pc, &dis_instr, &dunit, &status);
                if (status != 0)
                    goto fail;

                uint64_t instr_at_pc = 0;
                if (read_memory_at_pc(pc, &instr_at_pc, size, &dinfo)) {
                    LOG_ERR("Reading instr at pc failed\n");
                    goto fail;
                }

                if (!search_discontinuity && branch_map.cnt == 0
                    && pc == packet->address)
                    hit_address = true;

                add_to_trace(instr_list, &dis_instr);

                /* advance pc */
                pc += size;

                /* we hit a conditional branch, follow or not and update map */
                switch (dinfo.insn_type) {
                case dis_nonbranch:
                    /* TODO: we need this hack since {m,s,u} ret are not
                     * classified
                     */
                    if (!is_unpred_discontinuity(instr_at_pc)) {
                        break;
                    }
                    LOG_INFO("Detected mret, uret or sret\n");
                case dis_jsr:    /* There is not real difference ... */
                case dis_branch: /* ... between those two */
                    /* we know that this instruction must have its jump target
                     * encoded in the binary else we would have gotten a
                     * non-predictable discontinuity packet. If
                     * branch_map.cnt == 0 + jump target unknown and we are here
                     * then we know that its actually a branch_map
                     * flush + discontinuity packet.
                     */
                    if (branch_map.cnt > 1 && dinfo.target == 0)
                        LOG_ERR(
                            "Can't predict the jump target, never happens\n");

                    if (branch_map.cnt == 1 && dinfo.target == 0) {
                        if (!branch_map.full) {
                            LOG_INFO(
                                "We hit the not-full branch_map + address edge case, (branch following discontinuity is included in this packet)\n");
                            if (!search_discontinuity)
                                LOG_ERR(
                                    "Not searching for discontinuity but hit not-full branch + address edge case\n");
                            /* TODO: should I poison addr? */
                            pc = packet->address;
                        } else {
                            LOG_INFO(
                                "We hit the full branch_map + address edge case\n");
                            pc = packet->address;
                        }
                        hit_discontinuity = true;
                    } else if (branch_map.cnt > 0 || dinfo.target != 0) {
                        /* we should not hit unpredictable
                         * discontinuities */
                        pc = dinfo.target;
                    } else {
                        /* we finally hit a jump with unknown  destination,
                         * thus the information in this packet  is used up
                         */
                        if (!search_discontinuity)
                            LOG_ERR(
                                "We were searching for an address and not a discontinuity\n");
                        pc = packet->address;
                        hit_discontinuity = true;
                        LOG_INFO("Found the discontinuity\n");
                    }
                    break;

                case dis_condbranch:
                    /* this case allows us to exhaust the branch bits */
                    {
                        /* 32 would be undefined */
                        bool branch_taken = branch_map.bits & 1;
                        branch_map.bits >>= 1;
                        branch_map.cnt--;
                        if (dinfo.target == 0)
                            LOG_ERR(
                                "Can't predict the jump target, never happens\n");
                        if (branch_taken)
                            pc = dinfo.target;
                        break;
                    }
                case dis_dref:
                    /* LOG_ERR("Don't know what to do with this type\n"); */
                    break;

                case dis_dref2:
                case dis_condjsr:
                case dis_noninsn:
                    LOG_ERR("Invalid insn_type: %d\n", dinfo.insn_type);
                    goto fail;
                }
            }
        } else if (packet->format == F_BRANCH_DIFF) {
            LOG_ERR("F_BRANCH_DIFF decoding: not implemented yet\n");

        } else if (packet->format == F_SYNC) {
            /* Sync pc. As in todo.org described this doesn't work for resync
             * (empty branch map) and context change. For now we dont allow both
             * situations
             */
            decompression_context.privilege = packet->privilege;
            pc = packet->address;

            /* since we are abruptly changing the pc we have to check if we
             * leave the section before we can disassemble. This is really ugly
             * TODO: fix
             */
            if (pc >= section->vma + stop_offset || pc < section->vma) {
                free_section_for_debugging(&dinfo);

                section = get_section_for_vma(abfd, pc);
                if (!section) {
                    LOG_ERR("VMA (PC) not pointing to any section\n");
                    goto fail;
                }
                stop_offset =
                    bfd_get_section_size(section) / dinfo.octets_per_byte;

                if (alloc_section_for_debugging(abfd, section, &dinfo)) {
                    LOG_ERR("Failed alloc_section_for_debugging\n");
                    goto fail;
                }
                LOG_INFO("Switched to section:%s\n", section->name);
            }

            int status = 0;
            int size = disassemble_at_pc(pc, &dis_instr, &dunit, &status);
            if (status != 0)
                goto fail;

            uint64_t instr_at_pc = 0;
            if (read_memory_at_pc(pc, &instr_at_pc, size, &dinfo)) {
                LOG_ERR("Reading instr at pc failed\n");
                goto fail;
            }

            add_to_trace(instr_list, &dis_instr);

            pc += size;

            switch (dinfo.insn_type) {
            case dis_nonbranch:
                /* TODO: we need this hack since {m,s,u} ret are not
                 * classified
                 */
                if (!is_unpred_discontinuity(instr_at_pc)) {
                    break;
                }
                LOG_INFO("Detected mret, uret or sret\n");
            case dis_jsr:    /* There is not real difference ... */
            case dis_branch: /* ... between those two */
                if (dinfo.target == 0)
                    LOG_ERR("Can't predict the jump target, never happens\n");
                pc = dinfo.target;
                break;

            case dis_condbranch:
                if (dinfo.target == 0)
                    LOG_ERR("Can't predict the jump target, never happens\n");
                if (packet->branch == 0) {
                    LOG_ERR("Doing a branch from a F_SYNC packet\n");
                    pc = dinfo.target;
                }
                break;
            case dis_dref: /* TODO: is this useful? */
                break;
            case dis_dref2:
            case dis_condjsr:
            case dis_noninsn:
                LOG_ERR("Invalid insn_type: %d\n", dinfo.insn_type);
                goto fail;
            }
        } else if (packet->format == F_ADDR_ONLY) {
            /* We have to find the instruction where we can apply the address
             * information to. This might either be a discontinuity infromation
             * or a sync up address. So we stop either at the given address or
             * use it on a discontinuity, whichever comes first
             *
             * TODO: we don't allow resync packets for now, that means
             * we won't have issues distinguishing between address
             * sync and unpredictable discontinuities. We can
             * distinguish the case of advance to next address with
             * advance to unpredictable discontinuitiy by inspecting
             * the next packet. The events halt and unqualified happen
             * if we don't have any more packets (next_packet == NULL,
             * TODO: make this better). Exception and privilege change
             * events always happen in packet pairs, which, again, we
             * can figure out by looking at next_packet. If any test
             * so far failed, then we know, since we are disallowing
             * resync packets for now, that we are dealing with a
             * unpredictable discontinuity.
             */
            bool hit_address = false;
            bool hit_discontinuity = false;

            bool search_discontinuity = true;
            if (next_packet == NULL
                || (next_packet->format == F_SYNC
                    && (next_packet->subformat == SF_START
                        || next_packet->subformat == SF_EXCEPTION))) {
                /* advance until address */
                LOG_INFO("Searching for address\n");
                search_discontinuity = false;
            } else {
                /* advance until unpredictable discontinuity */
                LOG_INFO("Searching for discontinuity\n");
                search_discontinuity = true;
            }

            while (!(hit_address || hit_discontinuity)) {
                int status = 0;
                int size = disassemble_at_pc(pc, &dis_instr, &dunit, &status);
                if (status != 0)
                    goto fail;

                /* TODO: fix */
                if (!conf.full_address) {
                    LOG_ERR("full_address false: not implemented yet\n");
                    goto fail;
                }

                if (!search_discontinuity && pc == packet->address)
                    hit_address = true;

                uint64_t instr_at_pc = 0;
                if (read_memory_at_pc(pc, &instr_at_pc, size, &dinfo)) {
                    LOG_ERR("Reading instr at pc failed\n");
                    goto fail;
                }

                add_to_trace(instr_list, &dis_instr);

                /* advance pc */
                pc += size;

                switch (dinfo.insn_type) {
                case dis_nonbranch:
                    /* TODO: we need this hack since {m,s,u} ret are not
                     * classified
                     */
                    if (!is_unpred_discontinuity(instr_at_pc)) {
                        break;
                    }
                    LOG_INFO("Detected mret, uret or sret\n");
                case dis_jsr:    /* There is not real difference ... */
                case dis_branch: /* ... between those two */
                    if (dinfo.target) {
                        pc = dinfo.target;
                    } else {
                        if (!search_discontinuity)
                            LOG_ERR(
                                "We were searching for an address and not a discontinuity\n");
                        LOG_INFO("Found the discontinuity\n");
                        pc = packet->address;
                        hit_discontinuity = true;
                    }
                    break;

                case dis_condbranch:
                    LOG_ERR(
                        "We shouldn't hit conditional branches with F_ADDRESS_ONLY\n");
                    break;

                case dis_dref: /* TODO: is this useful? */
                    break;
                case dis_dref2:
                case dis_condjsr:
                case dis_noninsn:
                    LOG_ERR("Invalid insn_type: %d\n", dinfo.insn_type);
                    goto fail;
                }
            }
        }
    }

fail:
    free_section_for_debugging(&dinfo);
    return NULL;
}


static uint32_t branch_map_len(uint32_t branches)
{
    if (branches <= 1) {
        return 1;
    } else if (branches <= 9) {
        return 9;
    } else if (branches <= 25) {
        return 25;
    } else if (branches <= 31) {
        return 31;
    }
    return -1;
}

/* It's pretty annoying to pack our packets tightly, since our packet members
 * are not 8 bit aligned. We assure that each packet is smaller that 128 bits,
 * pack all packet members into a 128 bit integer and read it out to bytes
 * through the union. Since uint8_t doesn't count as a char the compiler assumes
 * a uint8_t pointer can't alias a __uint128_t, thus the union. Or we could use
 * -fno-strict-aliasing.
 */
union pack {
    __uint128_t bits; /* non-portable gcc stuff. TODO: fix */
    uint8_t bin[16];  /* since uint8_t =/= char strict aliasing might
                       * mess your shit up. Careful this is also
                       * endian dependent
                       */
} data = {0};


/* bin must be an all zeros array */
static int serialize_packet(struct tr_packet *packet, size_t *bitcnt,
                            uint8_t align, uint8_t bin[])
{
    if (packet->msg_type != 0x2) {
        LOG_ERR("trace packet message type not supported: %d\n",
                packet->msg_type);
        return -1;
    }
    if (!conf.full_address) {
        LOG_ERR("full_address false: not implemented yet\n");
        return -1;
    }
    if (align >= 8) {
        LOG_ERR("bad alignment value: %" PRId8 "\n", align);
        return -1;
    }
    switch (packet->format) {
    case F_BRANCH_FULL: {
        uint32_t len = branch_map_len(packet->branches);

        /* different non-portable way to calculate same packed bits.
         * For some additional assurance. Machine must be little
         * endian (or add the htole128 function)
         */
        assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
        /* we need enough space to do the packing it in uint128 */
        assert(128 > XLEN + 9 + 31 + 8);
        /* TODO: assert branch map to overfull */
        /* TODO: branch map full logic handling */
        /* TODO: for now we put the address always */
        data.bits = 0;
        data.bits = (packet->msg_type) | (packet->format << 2)
                    | (packet->branches << 4);
        data.bits |= ((__uint128_t)packet->branch_map & MASK_FROM(len)) << 9;
        data.bits |= ((__uint128_t)packet->address << (9 + len));
        data.bits <<= align;

        *bitcnt = 9 + len + XLEN;
        memcpy(bin, data.bin, (*bitcnt + align) / 8 + 1);

        for (size_t j = 0; j < ((*bitcnt + align) / 8 + 1); j++) {
            /* printf("0x%" PRIx8 "\n", data.bin[j]); */
        }
        return 0;
    }

    case F_BRANCH_DIFF:
        LOG_ERR("F_BRANCH_DIFF serialize_packet not implemented yet\n");
        *bitcnt = 0;
        return -1;

    case F_ADDR_ONLY:
        assert(128 > XLEN + 4 + 8);
        data.bits = packet->msg_type | (packet->format << 2)
                    | ((__uint128_t)packet->address << 4);
        data.bits <<= align;

        *bitcnt = XLEN + 4;
        memcpy(bin, data.bin, (*bitcnt + align) / 8 + 1);
        return 0;

    case F_SYNC:
        assert(PRIVLEN == 3);
        /* check for enough space to the packing */
        assert(128 > 2 + 2 + 2 + PRIVLEN + 1 + XLEN + CAUSELEN + 1 + XLEN);
        /* TODO: for now we ignore the context field since we have
         *  only one hart
         */

        data.bits = 0;
        /* common part to all sub formats */
        data.bits = packet->msg_type | (packet->format << 2)
                    | (packet->subformat << 4) | (packet->privilege << 6);
        *bitcnt = 6 + PRIVLEN;

        switch (packet->subformat) {
        case SF_START:
            data.bits |= (packet->branch << (6 + PRIVLEN))
                         | ((__uint128_t)packet->address << (7 + PRIVLEN));
            *bitcnt += 1 + XLEN;
            break;

        case SF_EXCEPTION:
            data.bits |= (packet->branch << (6 + PRIVLEN))
                         | ((__uint128_t)packet->address << (7 + PRIVLEN))
                         | ((__uint128_t)packet->ecause << (7 + PRIVLEN + XLEN))
                         | ((__uint128_t)packet->interrupt
                            << (7 + PRIVLEN + XLEN + CAUSELEN))
                         | ((__uint128_t)packet->tval
                            << (8 + PRIVLEN + XLEN + CAUSELEN));
            *bitcnt += 1 + XLEN + CAUSELEN + 1 + XLEN;
            break;

        case SF_CONTEXT:
            /* TODO: we still ignore the context field */
            break;
        }

        data.bits <<= align;
        memcpy(bin, data.bin, (*bitcnt + align) / 8 + 1);
        return 0;
    }
    return -1;
}


int trdb_write_packets(const char *path, struct list_head *packet_list)
{
    int status = 0;
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror("trdb_write_packets");
        status = -1;
        goto fail;
    }

    uint8_t bin[sizeof(struct tr_packet)] = {0};
    size_t bitcnt = 0;
    uint32_t alignment = 0;
    uint8_t carry = 0;
    size_t good = 0;
    size_t rest = 0;

    struct tr_packet *packet;
    /* TODO: do we need the rever version? I think we do*/
    list_for_each_entry(packet, packet_list, list)
    {
        if (serialize_packet(packet, &bitcnt, alignment, bin)) {
            status = -1;
            goto fail;
        }
        for (size_t i = 0; i < (bitcnt + alignment) / 8 + 1; i++) {
            /* printf("DUMP: %" PRIx8 "\n", bin[i]); */
        }
        /* stitch two consecutive packets together */
        bin[0] |= carry;
        rest = (bitcnt + alignment) % 8;
        /* printf("REST: %zu\n", rest); */
        good = (bitcnt + alignment) / 8;
        /* write as many bytes as we can i.e. withouth the potentially
         * intersecting ones
         */
        if (fwrite(bin, 1, good, fp) != good) {
            perror("fwrite");
            status = -1;
            goto fail;
        }
        /* we keep that for the next packet */
        carry = bin[good] & MASK_FROM(rest);
        alignment = rest;
    }
    /* done, write remaining carry */
    if (!fwrite(&bin[good], 1, 1, fp)) {
        perror("fwrite");
        status = -1;
    }
fail:
    if (fp)
        fclose(fp);
    return status;
}


/* TODO: this double pointer mess is a bit ugly. Maybe use the list.h anyway?*/
size_t trdb_stimuli_to_trace(const char *path, struct tr_instr **samples,
                             int *status)
{
    *status = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror("fopen");
        *status = -1;
        goto fail;
    }
    size_t scnt = 0;

    int ret = 0;
    int valid = 0;
    int exception = 0;
    int interrupt = 0;
    uint32_t cause = 0;
    uint32_t tval = 0;
    uint32_t priv = 0;
    uint32_t iaddr = 0;
    uint32_t instr = 0;
    int compressed = 0;

    size_t size = 128;
    *samples = malloc(size * sizeof(**samples));
    while (
        (ret = fscanf(fp,
                      "valid= %d exception= %d interrupt= %d cause= %" SCNx32
                      " tval= %" SCNx32 " priv= %" SCNx32
                      " compressed= %d addr= %" SCNx32 " instr= %" SCNx32 " \n",
                      &valid, &exception, &interrupt, &cause, &tval, &priv,
                      &compressed, &iaddr, &instr))
        != EOF) {
        if (!valid) {
            continue;
        }
        if (scnt >= size) {
            size = 2 * size;
            struct tr_instr *tmp = realloc(*samples, size * sizeof(**samples));
            if (!tmp) {
                perror("realloc");
                *status = -1;
                goto fail;
            }
            *samples = tmp;
        }
        (*samples)[scnt] = (struct tr_instr){0};
        (*samples)[scnt].exception = exception;
        (*samples)[scnt].interrupt = interrupt;
        (*samples)[scnt].cause = cause;
        (*samples)[scnt].tval = tval;
        (*samples)[scnt].priv = priv;
        (*samples)[scnt].iaddr = iaddr;
        (*samples)[scnt].instr = instr;
        (*samples)[scnt].compressed = compressed;
        scnt++;
    }
    /* initialize the remaining unitialized memory */
    memset((*samples) + scnt, 0, size - scnt);

    if (ferror(fp)) {
        perror("fscanf");
        *status = -1;
        goto fail;
    }

    if (fp)
        fclose(fp);
    return scnt;
fail:
    if (fp)
        fclose(fp);
    free(*samples);
    *samples = NULL;
    return 0;
}


void trdb_disassemble_trace(size_t len, struct tr_instr trace[len],
                            struct disassembler_unit *dunit)
{
    struct disassemble_info *dinfo = dunit->dinfo;
    for (size_t i = 0; i < len; i++) {
        (*dinfo->fprintf_func)(dinfo->stream, "0x%016jx  ",
                               (uintmax_t)trace[i].iaddr);
        (*dinfo->fprintf_func)(dinfo->stream, "0x%08jx  ",
                               (uintmax_t)trace[i].instr);
        (*dinfo->fprintf_func)(dinfo->stream, "%s",
                               trace[i].exception ? "TRAP!  " : "");
        disassemble_single_instruction(trace[i].instr, trace[i].iaddr, dunit);
    }
}


void trdb_dump_packet_list(FILE *stream, struct list_head *packet_list)
{
    struct tr_packet *packet;
    list_for_each_entry_reverse(packet, packet_list, list)
    {
        trdb_print_packet(stream, packet);
    }
}


void trdb_print_packet(FILE *stream, struct tr_packet *packet)
{
    switch (packet->format) {
    case F_BRANCH_FULL:
    case F_BRANCH_DIFF:
        fprintf(stream, "PACKET ");
        packet->format == F_BRANCH_FULL ? fprintf(stream, "0: F_BRANCH_FULL\n")
                                        : fprintf(stream, "1: F_BRANCH_DIFF\n");
        fprintf(stream, "    branches  : %" PRIu32 "\n", packet->branches);
        fprintf(stream, "    branch_map: 0x%" PRIx32 "\n", packet->branch_map);
        fprintf(stream, "    address   : 0x%" PRIx32 "\n", packet->address);
        /* TODO: that special full branch map behaviour */
        break;

    case F_ADDR_ONLY:
        fprintf(stream, "PACKET 2: F_ADDR_ONLY\n");
        fprintf(stream, "    address   : 0x%" PRIx32 "\n", packet->address);
        break;
    case F_SYNC:
        fprintf(stream, "PACKET 3: F_SYNC\n");
        const char *subf[4];
        subf[0] = "SF_START";
        subf[1] = "SF_EXCEPTION";
        subf[2] = "SF_CONTEXT";
        subf[3] = "RESERVED";
        fprintf(stream, "    subformat : %s\n", subf[packet->subformat]);

        switch (packet->subformat) {
        case SF_CONTEXT:
            /* TODO fix this */
            fprintf(stream, "    context   :\n");
            fprintf(stream, "    privilege : 0x%" PRIx32 "\n",
                    packet->privilege);
        case SF_START:
            fprintf(stream, "    branch    : %s\n",
                    packet->branch ? "true" : "false");
            fprintf(stream, "    address   : 0x%" PRIx32 "\n", packet->address);
        case SF_EXCEPTION:
            fprintf(stream, "    ecause    : 0x%" PRIx32 "\n", packet->ecause);
            fprintf(stream, "    interrupt : %s\n",
                    packet->interrupt ? "true" : "false");
            fprintf(stream, "    tval      : 0x%" PRIx32 "\n", packet->tval);
        }
        break;
    }
}


void trdb_free_packet_list(struct list_head *packet_list)
{
    struct tr_packet *packet;
    struct tr_packet *packet_next;
    list_for_each_entry_safe(packet, packet_next, packet_list, list)
    {
        free(packet);
    }
}


void trdb_free_instr_list(struct list_head *instr_list)
{
    struct tr_instr *instr;
    struct tr_instr *instr_next;
    list_for_each_entry_safe(instr, instr_next, instr_list, list)
    {
        free(instr);
    }
}

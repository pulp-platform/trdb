/*
 * trdb - Trace Debugger Software for the PULP platform
 *
 * Copyright (C) 2018 Robert Balas
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

/* Notes: No hardware loop support. Special additional packet for exceptions
 * because vector table can be thought of as self-modifying code. Ingress
 * interface is slightly different for RI5CY than according to the standard
 * (always decompressed instruction + whether-it-was-compressed bit instead of
 * the instruction in its original form since it fits better to RI5CY's
 * pipeline).
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <endian.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <sys/queue.h>
#include "trace_debugger.h"
#include "trdb_private.h"
#include "disassembly.h"
#include "disassembly_private.h"
#include "serialize.h"
#include "kvec.h"
#include "utils.h"

struct disassembler_unit;

struct trdb_config;
struct trdb_state;
struct branch_map_state;
struct filter_state;

/* Configuration state of the trace debugger, used to guide the compression and
 * decompression routine
 */
struct trdb_config {
    /* addressing mode */
    bool arch64;

    /* TODO: Unused, inspect iaddress-lsb-p, implicit-except,
     * set-trace
     */
    uint64_t resync_max;
    /* bool iaddress_lsb_p; */
    /* bool implicit_except; */
    /* bool set_trace; */

    /* collect as much information as possible, but slower execution */
    bool full_statistics;

    /* set to true to always use absolute addresses in packets, this is used for
     * decompression and compression
     */
    bool full_address;
    /* do the sign extension of addresses like PULP. Only relevant if
     * full_address = false
     */
    bool use_pulp_sext;
    /* Don't regard ret's as unpredictable discontinuity */
    bool implicit_ret;
    /* Use additional packets to jump over vector table, a hack for PULP */
    bool pulp_vector_table_packet;
    /* whether we compress full branch maps */
    bool compress_full_branch_map;
};

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

    struct tr_instr instr;
};

/* Responsible to hold current branch state, that is the sequence of taken/not
 * taken branches so far. The bits field keeps track of that by setting the
 * cnt'th bit to 0 or 1 for a taken or not taken branch respectively. There can
 * be at most 31 entries in the branch map. A full branch map has the full flag
 * set.
 */
struct branch_map_state {
    bool full;
    uint32_t bits;
    uint32_t cnt;
};

/* We don't want to record all the time and this struct is used to indicate when
 * we do.
 */
struct filter_state {

    /* if we should output periodic timestamps (not implemented) */
    bool enable_timestamps;

    /* for tracing only certain privilege levels (not implemented) */
    bool trace_privilege;
    uint32_t privilege;

    /* TODO: look at those variables */
    uint64_t resync_cnt;
    bool resync_pend;
    /* uint32_t resync_nh = 0;  */
};

struct trdb_compress {
    struct trdb_state lastc;
    struct trdb_state thisc;
    struct trdb_state nextc;
    struct branch_map_state branch_map;
    struct filter_state filter;
    addr_t last_iaddr; /* TODO: make this work with 64 bit */
};

/* Current state of the cpu during decompression. Allows one to precisely emit a
 * sequence tr_instr. Handles the exception stack (well here we assume that
 * programs actually do nested exception handling properly) and hardware loops
 * (TODO: not implemented).
 */
/* TODO: Note on hw loops interrupted by an interrupt: look at after
 * the interrupt how many instructions of the loop are executed to
 * figure out in which loop number we got interrupted
 */

/* stack vector for return addresses */
kvec_nt(trdb_stack, addr_t);

struct trdb_decompress {
    /* TODO: hw loop addresses handling*/
    /* TODO: nested interrupt stacks for each privilege mode*/
    struct trdb_stack call_stack;
    /* record current privilege level */
    uint32_t privilege : PRIVLEN;
    /* needed for address compression */
    addr_t last_packet_addr;
    struct branch_map_state branch_map;
};

/* struct to record statistics about compression and decompression of traces */
struct trdb_stats {
    size_t payloadbits;
    size_t packetbits;
    size_t pulpbits;
    size_t instrbits;
    size_t instrs;
    size_t packets;
    size_t zo_addresses;  /**< all zero or all ones addresses */
    size_t zo_branchmaps; /**< all zero or all ones branchmaps */
    size_t addr_only_packets;
    size_t exception_packets;
    size_t start_packets;
    size_t diff_packets;
    size_t abs_packets;
    size_t bmap_full_packets;
    size_t bmap_full_addr_packets;
    uint32_t sext_bits[64];
};

/* Library context, needs to be passed to most function calls.
 */
struct trdb_ctx {
    /* specific settings for compression/decompression */
    struct trdb_config config;
    /* state used for compression */
    struct trdb_compress *cmp;
    /* state used for decompression */
    struct trdb_decompress *dec;
    /* state used for disassembling */
    struct tr_instr *dis_instr;
    struct disassembler_unit *dunit;
    /* compression statistics */
    struct trdb_stats stats;
    /* desired logging level and custom logging hook*/
    int log_priority;
    void (*log_fn)(struct trdb_ctx *ctx, int priority, const char *file,
                   int line, const char *fn, const char *format, va_list args);
};

void trdb_log(struct trdb_ctx *ctx, int priority, const char *file, int line,
              const char *fn, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    ctx->log_fn(ctx, priority, file, line, fn, format, args);
    va_end(args);
}

static void log_stderr(struct trdb_ctx *ctx, int priority, const char *file,
                       int line, const char *fn, const char *format,
                       va_list args)
{
    (void)ctx;
    (void)priority;
    fprintf(stderr, "trdb: %s:%d:0: %s(): ", file, line, fn);
    vfprintf(stderr, format, args);
}

static void log_stdout_quiet(struct trdb_ctx *ctx, int priority,
                             const char *file, int line, const char *fn,
                             const char *format, va_list args)
{
    (void)ctx;
    (void)priority;
    (void)file;
    (void)line;
    (void)fn;
    vfprintf(stdout, format, args);
}

static int log_priority(const char *priority)
{
    char *endptr;
    int prio;

    prio = strtol(priority, &endptr, 10);
    if (endptr[0] == '\0' || isspace(endptr[0]))
        return prio;
    if (strncmp(priority, "err", 3) == 0)
        return LOG_ERR;
    if (strncmp(priority, "info", 4) == 0)
        return LOG_INFO;
    if (strncmp(priority, "debug", 5) == 0)
        return LOG_DEBUG;
    return 0;
}

void trdb_reset_compression(struct trdb_ctx *ctx)
{
    ctx->config = (struct trdb_config){.resync_max               = UINT64_MAX,
                                       .full_address             = true,
                                       .pulp_vector_table_packet = true,
                                       .full_statistics          = true};

    ctx->cmp->lastc      = (struct trdb_state){.privilege = 7};
    ctx->cmp->thisc      = (struct trdb_state){.privilege = 7};
    ctx->cmp->nextc      = (struct trdb_state){.privilege = 7};
    ctx->cmp->branch_map = (struct branch_map_state){0};
    ctx->cmp->filter     = (struct filter_state){0};
    ctx->cmp->last_iaddr = 0;
    ctx->stats           = (struct trdb_stats){0};
}

void trdb_reset_decompression(struct trdb_ctx *ctx)
{
    ctx->config = (struct trdb_config){.resync_max               = UINT64_MAX,
                                       .full_address             = true,
                                       .pulp_vector_table_packet = true,
                                       .full_statistics          = true};

    *ctx->dec            = (struct trdb_decompress){0};
    ctx->dec->branch_map = (struct branch_map_state){0};
    kv_destroy(ctx->dec->call_stack);
    kv_init(ctx->dec->call_stack);
    ctx->dec->privilege        = 7;
    ctx->dec->last_packet_addr = 0;

    ctx->cmp->filter = (struct filter_state){0};

    ctx->stats = (struct trdb_stats){0};
}

struct trdb_ctx *trdb_new()
{
    const char *env;
    struct trdb_ctx *ctx = malloc(sizeof(*ctx));
    if (!ctx)
        return NULL;
    *ctx = (struct trdb_ctx){0};

    ctx->cmp       = malloc(sizeof(*ctx->cmp));
    ctx->dec       = malloc(sizeof(*ctx->dec));
    ctx->dis_instr = malloc(sizeof(*ctx->dis_instr));
    if (!ctx->cmp || !ctx->dec || !ctx->dis_instr) {
        free(ctx->cmp);
        free(ctx->dec);
        free(ctx->dis_instr);
        free(ctx);
        return NULL;
    }

    ctx->config = (struct trdb_config){.resync_max               = UINT64_MAX,
                                       .full_address             = true,
                                       .pulp_vector_table_packet = true,
                                       .full_statistics          = true};

    *ctx->cmp            = (struct trdb_compress){0};
    ctx->cmp->lastc      = (struct trdb_state){.privilege = 7};
    ctx->cmp->thisc      = (struct trdb_state){.privilege = 7};
    ctx->cmp->nextc      = (struct trdb_state){.privilege = 7};
    ctx->cmp->branch_map = (struct branch_map_state){0};
    ctx->cmp->filter     = (struct filter_state){0};
    ctx->cmp->last_iaddr = 0;

    *ctx->dec = (struct trdb_decompress){0};
    kv_init(ctx->dec->call_stack);

    *ctx->dis_instr = (struct tr_instr){0};

    ctx->log_fn       = log_stdout_quiet;
    ctx->log_priority = LOG_ERR; // TODO: log nothing by default is better?

    ctx->stats = (struct trdb_stats){0};

    /* environment overwrites config */
    env = secure_getenv("TRDB_LOG");
    if (env != NULL)
        trdb_set_log_priority(ctx, log_priority(env));

    info(ctx, "ctx %p created\n", ctx);
    dbg(ctx, "log_priority=%d\n", ctx->log_priority);

    return ctx;
}

void trdb_free(struct trdb_ctx *ctx)
{
    if (!ctx)
        return;
    info(ctx, "context %p released\n", ctx);

    free(ctx->dis_instr);
    free(ctx->cmp);
    if (ctx->dec)
        kv_destroy(ctx->dec->call_stack);
    free(ctx->dec);
    free(ctx);
}

void trdb_set_log_fn(struct trdb_ctx *ctx,
                     void (*log_fn)(struct trdb_ctx *ctx, int priority,
                                    const char *file, int line, const char *fn,
                                    const char *format, va_list args))
{
    ctx->log_fn = log_fn;
    info(ctx, "custom logging function %p registered\n", log_fn);
}

int trdb_get_log_priority(struct trdb_ctx *ctx)
{
    return ctx->log_priority;
}

void trdb_set_log_priority(struct trdb_ctx *ctx, int priority)
{
    ctx->log_priority = priority;
}

void trdb_set_dunit(struct trdb_ctx *ctx, struct disassembler_unit *dunit)
{
    ctx->dunit = dunit;
}

struct disassembler_unit *trdb_get_dunit(struct trdb_ctx *ctx)
{
    return ctx->dunit;
}

void trdb_set_full_address(struct trdb_ctx *ctx, bool v)
{
    ctx->config.full_address = v;
}

bool trdb_is_full_address(struct trdb_ctx *ctx)
{
    return ctx->config.full_address;
}

void trdb_set_implicit_ret(struct trdb_ctx *ctx, bool implicit_ret)
{
    ctx->config.implicit_ret = implicit_ret;
}

bool trdb_get_implicit_ret(struct trdb_ctx *ctx)
{
    return ctx->config.implicit_ret;
}

void trdb_set_pulp_extra_packet(struct trdb_ctx *ctx, bool extra_packet)
{
    ctx->config.pulp_vector_table_packet = extra_packet;
}

bool trdb_get_pulp_extra_packet(struct trdb_ctx *ctx)
{
    return ctx->config.pulp_vector_table_packet;
}

void trdb_set_compress_branch_map(struct trdb_ctx *ctx, bool compress)
{
    ctx->config.compress_full_branch_map = compress;
}

bool trdb_get_compress_branch_map(struct trdb_ctx *ctx)
{

    return ctx->config.compress_full_branch_map;
}

size_t trdb_get_payloadbits(struct trdb_ctx *ctx)
{
    return ctx->stats.payloadbits;
}

size_t trdb_get_pulpbits(struct trdb_ctx *ctx)
{
    return ctx->stats.pulpbits;
}

size_t trdb_get_packetcnt(struct trdb_ctx *ctx)
{
    return ctx->stats.packets;
}

size_t trdb_get_instrcnt(struct trdb_ctx *ctx)
{
    return ctx->stats.instrs;
}

size_t trdb_get_instrbits(struct trdb_ctx *ctx)
{
    return ctx->stats.instrbits;
}

void trdb_get_packet_stats(struct trdb_ctx *ctx,
                           struct trdb_packet_stats *stats)
{
    struct trdb_stats *rstats     = &ctx->stats;
    stats->packets                = rstats->packets;
    stats->addr_only_packets      = rstats->addr_only_packets;
    stats->exception_packets      = rstats->exception_packets;
    stats->start_packets          = rstats->start_packets;
    stats->diff_packets           = rstats->diff_packets;
    stats->abs_packets            = rstats->abs_packets;
    stats->bmap_full_packets      = rstats->bmap_full_packets;
    stats->bmap_full_addr_packets = rstats->bmap_full_addr_packets;
}

uint32_t trdb_sign_extendable_bits(addr_t addr)
{

    /* TODO: a runtime switch would probably be better */
#ifdef TRDB_ARCH64
    return sign_extendable_bits64(addr);
#else
    return sign_extendable_bits(addr);
#endif
}

static bool is_branch(insn_t instr)
{
    assert((SHR(instr, 32) & 0xffffffff) == 0);
    bool is_riscv_branch = is_beq_instr(instr) || is_bne_instr(instr) ||
                           is_blt_instr(instr) || is_bge_instr(instr) ||
                           is_bltu_instr(instr) || is_bgeu_instr(instr);
    bool is_pulp_branch = is_p_bneimm_instr(instr) || is_p_beqimm_instr(instr);
    bool is_riscv_compressed_branch =
        is_c_beqz_instr(instr) || is_c_bnez_instr(instr);
    return is_riscv_branch || is_pulp_branch || is_riscv_compressed_branch;
}

static bool branch_taken(bool before_compressed, addr_t addr_before,
                         addr_t addr_after)
{
    /* TODO: this definitely doens't work for 64 bit instructions */
    /* since we have already decompressed instructions, but still compressed
     * addresses we need this additional flag to tell us what the instruction
     * originally was. So we can't tell by looking at the lower two bits of
     * instr.
     */
    return before_compressed ? !(addr_before + 2 == addr_after)
                             : !(addr_before + 4 == addr_after);
}

uint32_t branch_map_len(uint32_t branches)
{
    assert(branches <= 31);

    if (branches == 0) {
        return 31;
    } else if (branches <= 1) {
        return 1;
    } else if (branches <= 9) {
        return 9;
    } else if (branches <= 17) {
        return 17;
    } else if (branches <= 25) {
        return 25;
    } else if (branches <= 31) {
        return 31;
    }
    return 0;
}

/* Some jumps can't be predicted i.e. the jump address can only be figured out
 * at runtime. That happens e.g. if the target address depends on some register
 * entries. For plain RISC-V I this is just the jalr instruction. For the PULP
 * extensions, the hardware loop instruction have to be considered too.
 */
static bool is_unpred_discontinuity(insn_t instr, bool implicit_ret)
{
    assert((SHR(instr, 32) & 0xffffffff) == 0);

    bool jump = is_jalr_instr(instr) || is_really_c_jalr_instr(instr) ||
                is_really_c_jr_instr(instr);
    bool exception_ret =
        is_mret_instr(instr) || is_sret_instr(instr) || is_uret_instr(instr);

    /* this allows us to mark ret's as not being discontinuities, if we want */
    bool not_ret = true;
    if (implicit_ret)
        not_ret = !(is_c_ret_instr(instr) || is_ret_instr(instr));

    return (jump || exception_ret) && not_ret;
}

/* Just crash and error if we hit one of those */
static bool is_unsupported(addr_t instr)
{
    return is_lp_setup_instr(instr) || is_lp_counti_instr(instr) ||
           is_lp_count_instr(instr) || is_lp_endi_instr(instr) ||
           is_lp_starti_instr(instr) || is_lp_setupi_instr(instr);
}

/* Sometimes we have to decide whether to put in the absolute or differential
 * address into the packet. We choose the one which has the least amount of
 * meaningfull bits, i.e. the bits that can't be inferred by sign-extension.
 */
static bool differential_addr(int *lead, addr_t absolute, addr_t differential)
{
    int abs  = 0;
    int diff = 0;

    abs  = trdb_sign_extendable_bits(absolute);
    diff = trdb_sign_extendable_bits(differential);
    // /* on tie we probe which one would be better */
    // if ((abs == 32) && (diff == 32)) {
    //     if ((abs & 1) == last) {
    //         *lead = 0;
    //         return prefer_abs;
    //     } else if ((diff & 1) == last) {
    //         *lead = 0;
    //         return prefer_diff;
    //     } else {
    //         *lead = 1;
    //         return prefer_abs;
    //     }
    // }

    // /* check if we can sign extend from the previous byte */
    // if (abs == 32) {
    //     if ((abs & 1) == last_bit)
    //         *lead = 0;
    //     else
    //         *lead = 1;
    //     return prefer_abs;
    // }

    // if (diff == 32) {
    //     if ((diff & 1) == last_bit)
    //         *lead = 0;
    //     else
    //         *lead = 1;
    //     return prefer_diff;
    // }

    /* general case */
    *lead = diff > abs ? diff : abs;
    return diff > abs; /* on tie we prefer absolute */
}

static unsigned quantize_clz(unsigned x)
{

    if (x < 9)
        return 0;
    else if (x < 17)
        return 9;
    else if (x < 25)
        return 17;
    else
        return 25;
}

/* Does the same as differential_addr() but only considers byte boundaries */
static bool pulp_differential_addr(int *lead, addr_t absolute,
                                   addr_t differential)
{
    unsigned abs  = sign_extendable_bits(absolute);
    unsigned diff = sign_extendable_bits(differential);

    /* we are only interested in sign extension for byte boundaries */
    abs  = quantize_clz(abs);
    diff = quantize_clz(diff);
    assert(abs != 32); /* there is always a one or zero leading */

    /* general case */
    *lead = diff > abs ? diff : abs;
    return diff > abs; /* on tie we prefer absolute */
}

/* Set @p tr to contain an exception packet. */
static void emit_exception_packet(struct trdb_ctx *c, struct tr_packet *tr,
                                  struct tr_instr *lc_instr,
                                  struct tr_instr *tc_instr,
                                  struct tr_instr *nc_instr)
{
    tr->format    = F_SYNC;       /* sync */
    tr->subformat = SF_EXCEPTION; /* exception */
    tr->context   = 0;            /* TODO: what comes here? */
    tr->privilege = tc_instr->priv;

    /* if we happen to generate a packet when when the pc points to a branch, we
     * have to record this in the branch field, since this branch won't be in
     * any preceding or following packet that contains a branch map
     */
    if (is_branch(tc_instr->instr) &&
        !branch_taken(tc_instr->compressed, tc_instr->iaddr, nc_instr->iaddr))
        tr->branch = 1;
    else
        tr->branch = 0;

    tr->address = tc_instr->iaddr;
    /* With this packet we record last cycles exception
     * information. It's not possible for (i==0 &&
     * lastc_exception) to be true since it takes one cycle
     * for lastc_exception to change
     */
    tr->ecause    = lc_instr->cause;
    tr->interrupt = lc_instr->interrupt;
    tr->tval      = lc_instr->tval;
    tr->length    = FORMATLEN + FORMATLEN + PRIVLEN + 1 + XLEN + CAUSELEN + 1;
    c->stats.exception_packets++;
}

/* Set @p tr to contain a start packet. */
static void emit_start_packet(struct trdb_ctx *c, struct tr_packet *tr,
                              struct tr_instr *tc_instr,
                              struct tr_instr *nc_instr)
{
    tr->format    = F_SYNC;   /* sync */
    tr->subformat = SF_START; /* start */
    tr->context   = 0;        /* TODO: what comes here? */
    tr->privilege = tc_instr->priv;
    /* again, this information won't be in any preceding or following branch map
     * so we have to put in here. See emit_exception_packet() for details.
     */
    if (is_branch(tc_instr->instr) &&
        !branch_taken(tc_instr->compressed, tc_instr->iaddr, nc_instr->iaddr))
        tr->branch = 1;
    else
        tr->branch = 0;
    tr->address = tc_instr->iaddr;
    tr->length  = FORMATLEN + FORMATLEN + PRIVLEN + 1 + XLEN;
    c->stats.start_packets++;
}

/* Set @p tr to contain a packet that records the instruction flow until @p
 * tc_instr. That means we have to remember all taken/not taken branches (if
 * any) and the current pc. For the pc we try to figure out if we want to keep
 * the difference to the last packet's address or the current absolute address,
 * depending on which one needs less bits to represent.
 */
static int emit_branch_map_flush_packet(struct trdb_ctx *ctx,
                                        struct tr_packet *tr,
                                        struct branch_map_state *branch_map,
                                        struct tr_instr *tc_instr,
                                        addr_t last_iaddr, bool full_address,
                                        bool is_u_discontinuity)
{
    if (!ctx || !tr || !branch_map || !tc_instr)
        return -trdb_internal;

    struct trdb_stats *stats = &ctx->stats;

    /* we don't have any branches to keep track of */
    if (branch_map->cnt == 0) {
        tr->format   = F_ADDR_ONLY;
        tr->branches = branch_map->cnt;

        if (full_address) {
            tr->address = tc_instr->iaddr;
            tr->length  = FORMATLEN + XLEN;
        } else {
            /* always differential in F_ADDR_ONLY*/
            addr_t diff = last_iaddr - tc_instr->iaddr;
            addr_t lead = ctx->config.use_pulp_sext
                              ? quantize_clz(sign_extendable_bits(diff))
                              : sign_extendable_bits(diff);

            int keep = XLEN - lead + 1;
            /* should only be relevant for serialization */
            /* tr->address = MASK_FROM(keep) & diff; */
            tr->address = diff;
            tr->length  = FORMATLEN + keep;
            /* record distribution */
            stats->sext_bits[keep - 1]++;
            if (tr->address == 0 || tr->address == (addr_t)-1)
                stats->zo_addresses++;
        }
        stats->addr_only_packets++;
        assert(branch_map->bits == 0);
    } else {
        if (branch_map->full && is_u_discontinuity)
            dbg(ctx, "full branch map and discontinuity edge case\n");

        tr->branches = branch_map->cnt;

        if (full_address) {
            tr->format  = F_BRANCH_FULL;
            tr->address = tc_instr->iaddr;
            tr->length =
                FORMATLEN + BRANCHLEN + branch_map_len(branch_map->cnt);
            if (branch_map->full) {
                if (is_u_discontinuity) {
                    tr->length += XLEN;
                    stats->bmap_full_addr_packets++;
                } else {
                    /* we don't need to record the address, indicate by settings
                     * branchcnt to 0
                     */
                    tr->length += 0;
                    tr->branches = 0;
                }
                stats->bmap_full_packets++;
            } else {
                tr->length += XLEN;
                stats->abs_packets++;
            }
        } else {
            /* In this mode we try to compress the instruction address by taking
             * the difference address or the absolute address, whichever has
             * more bits which can be inferred by signextension.
             */
            addr_t diff = last_iaddr - tc_instr->iaddr;
            addr_t full = tc_instr->iaddr;
            int keep    = 0;
            int lead    = 0;
            bool use_differential =
                ctx->config.use_pulp_sext
                    ? pulp_differential_addr(&lead, full, diff)
                    : differential_addr(&lead, full, diff);

            if (use_differential) {
                keep       = XLEN - lead + 1;
                tr->format = F_BRANCH_DIFF;
                /* this should only be relevant for serialization */
                /* tr->address = MASK_FROM(keep) & diff; */
                tr->address = diff;
                stats->sext_bits[keep - 1]++;
            } else {
                keep       = XLEN - lead + 1;
                tr->format = F_BRANCH_FULL;
                /* this should only be relevant for serialization */
                /* tr->address = MASK_FROM(keep) & full; */
                tr->address = full;
                stats->sext_bits[keep - 1]++;
            }

            if (tr->address == 0 || tr->address == (addr_t)-1)
                stats->zo_addresses++;
            /* TODO: broke for 64-bit */
            unsigned sext = sign_extendable_bits64(
                ((uint64_t)tr->address << XLEN) |
                ((uint64_t)branch_map->bits
                 << (XLEN - branch_map_len(branch_map->cnt))));
            if (sext > XLEN + branch_map_len(branch_map->cnt))
                sext = XLEN + branch_map_len(branch_map->cnt);
            /* uint32_t ext = XLEN + branch_map_len(branch_map->cnt) - sext + 1;
             */
            tr->length =
                FORMATLEN + BRANCHLEN + branch_map_len(branch_map->cnt);
            /* tr->length = FORMATLEN + BRANCHLEN */
            /* 	+ext; */

            if (branch_map->full) {
                if (is_u_discontinuity) {
                    tr->length += keep;
                    stats->bmap_full_addr_packets++;
                } else {
                    /* we don't need to record the address, indicate by settings
                     * branchcnt to 0
                     */
                    tr->length += 0;
                    tr->branches = 0;
                    stats->bmap_full_packets++;
                }
            } else {
                tr->length += keep;
                if (use_differential)
                    stats->diff_packets++;
                else
                    stats->abs_packets++;
            }
        }
        tr->branch_map = branch_map->bits;
        *branch_map    = (struct branch_map_state){0};
    }

    return 0;
}

/* Set @p tr to contain a packet with a full branch map. This packet is special
 * since we don't need to remember the current pc, because this function call is
 * not initiated due to hitting some discontinuity or anything else that
 * requires to "flush" out the current instruction flow.
 */
static void emit_full_branch_map(struct trdb_ctx *ctx, struct tr_packet *tr,
                                 struct branch_map_state *branch_map)
{
    assert(branch_map->cnt == 31);
    tr->format = F_BRANCH_FULL;
    /* full branch map withouth address is indicated by settings branches to 0*/
    tr->branches   = 0;
    tr->branch_map = branch_map->bits;
    /* No address needed */
    int sext = sign_extendable_bits(branch_map->bits << 1);
    if (sext > 31)
        sext = 31;
    if (ctx->config.compress_full_branch_map)
        tr->length = FORMATLEN + BRANCHLEN + (31 - sext + 1);
    else
        tr->length = FORMATLEN + BRANCHLEN + branch_map_len(31);
    *branch_map = (struct branch_map_state){0};

    ctx->stats.bmap_full_packets++;
}

/* Compress instruction traces the same way as we do it on the PULP trace
 * debugger, so this is meant to emulate that behaviour and not to be a generic
 * way how a trace encoder would do it.
 */
int trdb_compress_trace_step(struct trdb_ctx *ctx, struct tr_packet *packet,
                             struct tr_instr *instr)
{

    int status = 0;
    if (!ctx || !packet || !instr)
        return -trdb_invalid;

    struct trdb_stats *stats   = &ctx->stats;
    struct trdb_config *config = &ctx->config;

    int generated_packet          = 0;
    bool full_address             = config->full_address;
    bool pulp_vector_table_packet = config->pulp_vector_table_packet;
    bool implicit_ret             = config->implicit_ret;

    /* for each cycle */
    // TODO: fix this hack by doing unqualified instead
    struct trdb_state *lastc = &ctx->cmp->lastc;
    struct trdb_state *thisc = &ctx->cmp->thisc;
    struct trdb_state *nextc = &ctx->cmp->nextc;

    nextc->instr = *instr;

    struct tr_instr *nc_instr = &ctx->cmp->nextc.instr;
    struct tr_instr *tc_instr = &ctx->cmp->thisc.instr;
    struct tr_instr *lc_instr = &ctx->cmp->lastc.instr;

    struct branch_map_state *branch_map = &ctx->cmp->branch_map;
    struct filter_state *filter         = &ctx->cmp->filter;

    thisc->halt = false;
    /* test for qualification by filtering */
    /* TODO: implement filtering logic */

    nextc->qualified   = true;
    nextc->unqualified = !nextc->qualified;
    nextc->exception   = instr->exception;
    nextc->unpred_disc = is_unpred_discontinuity(instr->instr, implicit_ret);
    nextc->privilege   = instr->priv;
    nextc->privilege_change = (thisc->privilege != nextc->privilege);

    /* last address we emitted in packet, needed to compute differential
     * addresses
     */
    addr_t *last_iaddr = &ctx->cmp->last_iaddr;

    /* TODO: clean this up, proper initial state per round required */
    thisc->emitted_exception_sync = false;
    nextc->emitted_exception_sync = false;

    bool firstc_qualified = !lastc->qualified && thisc->qualified;

    /* Start of one cycle */
    if (!instr->valid) {
        /* Invalid interface data, just freeze state*/
        return 0;
    }

    if (!thisc->qualified) {
        /* check if we even need to record anything */
        *lastc = *thisc;
        *thisc = *nextc;
        return 0; /* end of cycle */
    }

    if (is_unsupported(tc_instr->instr)) {
        err(ctx,
            "Instruction is not supported for compression: 0x%" PRIxINSN
            " at addr: 0x%" PRIxADDR "\n",
            tc_instr->instr, tc_instr->iaddr);
        status = -trdb_bad_instr;
        goto fail;
    }

    if (filter->resync_cnt++ == config->resync_max) {
        filter->resync_pend = true;
        filter->resync_cnt  = 0;
    }

    if (is_branch(tc_instr->instr)) {
        /* update branch map */
        if (!branch_taken(tc_instr->compressed, tc_instr->iaddr,
                          nc_instr->iaddr))
            branch_map->bits = branch_map->bits | (1u << branch_map->cnt);
        branch_map->cnt++;
        if (branch_map->cnt == 31) {
            branch_map->full = true;
        }
    }

    /* initialize packet */
    *packet = (struct tr_packet){.msg_type = W_TRACE};

    /* We trace the packet before the trapped instruction and the
     * first one of the exception handler
     */
    if (lastc->exception) {
        /* Send te_inst:
         * format 3
         * subformat 1 (exception -> all fields present)
         * resync_pend = 0
         */
        emit_exception_packet(ctx, packet, lc_instr, tc_instr, nc_instr);
        *last_iaddr = tc_instr->iaddr;

        thisc->emitted_exception_sync = true;
        filter->resync_pend           = false; /* TODO: how to handle this */

        generated_packet = 1;
        /* end of cycle */

    } else if (lastc->emitted_exception_sync && pulp_vector_table_packet) {
        /* Hack of PULP: First we assume that the vector table entry is a jump.
         * Since that entry can change during runtime, we need to emit the jump
         * destination address, which is the second instruction of the trap
         * handler. This a bit hacky and made to work for the PULP. If someone
         * puts something else than a jump instruction there then all bets are
         * off. This is a custom change.
         */
        /* Start packet */
        /* Send te_inst:
         * format 3
         * subformat 0 (start, no ecause, interrupt and tval)
         * resync_pend = 0
         */
        emit_start_packet(ctx, packet, tc_instr, nc_instr);
        *last_iaddr = tc_instr->iaddr;

        filter->resync_pend = false;
        generated_packet    = 1;

    } else if (firstc_qualified || thisc->unhalted || thisc->privilege_change ||
               (filter->resync_pend && branch_map->cnt == 0)) {

        /* Start packet */
        /* Send te_inst:
         * format 3
         * subformat 0 (start, no ecause, interrupt and tval)
         * resync_pend = 0
         */
        emit_start_packet(ctx, packet, tc_instr, nc_instr);
        *last_iaddr = tc_instr->iaddr;

        filter->resync_pend = false;
        generated_packet    = 1;

    } else if (lastc->unpred_disc) {
        /* Send te_inst:
         * format 0/1/2
         */
        status = emit_branch_map_flush_packet(ctx, packet, branch_map, tc_instr,
                                              *last_iaddr, full_address, true);
        if (status < 0) {
            generated_packet = 0;
            goto fail;
        }

        *last_iaddr      = tc_instr->iaddr;
        generated_packet = 1;

    } else if (filter->resync_pend && branch_map->cnt > 0) {
        /* we treat resync_pend && branches == 0 before */
        /* Send te_inst:
         * format 0/1/2
         */
        status = emit_branch_map_flush_packet(ctx, packet, branch_map, tc_instr,
                                              *last_iaddr, full_address, false);
        if (status < 0) {
            generated_packet = 0;
            goto fail;
        }

        *last_iaddr = tc_instr->iaddr;

        generated_packet = 1;

    } else if (nextc->halt || nextc->exception || nextc->privilege_change ||
               nextc->unqualified) {
        /* Send te_inst:
         * format 0/1/2
         */
        status = emit_branch_map_flush_packet(ctx, packet, branch_map, tc_instr,
                                              *last_iaddr, full_address, false);
        if (status < 0) {
            generated_packet = 0;
            goto fail;
        }

        *last_iaddr = tc_instr->iaddr;

        generated_packet = 1;

    } else if (branch_map->full) {
        /* Send te_inst:
         * format 0
         * no address
         */
        emit_full_branch_map(ctx, packet, branch_map);

        generated_packet = 1;

    } else if (thisc->context_change) {
        /* Send te_inst:
         * format 3
         * subformat 2
         */
        err(ctx, "context_change not supported\n");
        status = -trdb_unimplemented;
        goto fail;
    }

    /* update last cycle state */
    *lastc = *thisc;
    *thisc = *nextc;

    /* TODO: no 64 bit instr support */
    stats->instrbits += instr->compressed ? 16 : 32;
    stats->instrs++;
    if (generated_packet) {
        *branch_map = (struct branch_map_state){0};
        stats->payloadbits += (packet->length);
        stats->packets++;

        if (config->full_statistics) {
            /* figure out pulp payload by serializing and couting bits */
            uint8_t bin[16] = {0};
            size_t bitcnt   = 0;
            if (trdb_pulp_serialize_packet(ctx, packet, &bitcnt, 0, bin)) {
                dbg(ctx, "failed to count bits of pulp packet\n");
            }
            /* we have to round up to the next full byte */
            stats->pulpbits += ((bitcnt / 8) + (bitcnt % 8 != 0)) * 8;
        }
    }

    if (generated_packet) {
        trdb_log_packet(ctx, packet);
    }

    if (ctx->dunit) {
        /* TODO: unfortunately this ignores log_fn */
        if (trdb_get_log_priority(ctx) == LOG_DEBUG) {
            trdb_disassemble_instr(instr, ctx->dunit);
        }
    } else {
        dbg(ctx, "0x%08jx  0x%08jx\n", (uintmax_t)instr->iaddr,
            (uintmax_t)instr->instr);
    }

fail:
    if (status == trdb_ok) {
        return generated_packet;
    } else
        return status;
}

/* this is just a different interface to trdb_compress_trace_step() where
 * packets generated packetes are appened to a given list header
 */
int trdb_compress_trace_step_add(struct trdb_ctx *ctx,
                                 struct trdb_packet_head *packet_list,
                                 struct tr_instr *instr)
{
    if (!ctx || !packet_list || !instr)
        return -trdb_invalid;

    struct tr_packet *packet = malloc(sizeof(*packet));
    if (!packet)
        return -trdb_nomem;

    int status = trdb_compress_trace_step(ctx, packet, instr);
    if (status < 0) {
        free(packet);
        return status;
    }

    if (status == 1)
        TAILQ_INSERT_TAIL(packet_list, packet, list);
    else
        free(packet);

    return status;
}

int trdb_pulp_model_step(struct trdb_ctx *ctx, struct tr_instr *instr,
                         uint32_t *packet_word)
{
    (void)packet_word;
    struct tr_packet packet;
    int status = trdb_compress_trace_step(ctx, &packet, instr);
    if (status < 0) {
        /* some error */
    }

    if (status == 1) {
        /* enqueue into fifo */
    }

    /* if fifo not empty, grab uint32_t */
    /* if fifo empty, nothing*/
    /* if fifo full, do backstalling etc. depending on recovery mode*/
    return 0; /* TODO: unimplemented */
}

/* try to update the return address stack*/
static int update_ras(struct trdb_ctx *c, addr_t instr, addr_t addr,
                      struct trdb_stack *stack, addr_t *ret_addr)
{
    if (!stack)
        return -trdb_invalid;

    bool compressed   = (instr & 0x3) != 0x3;
    enum trdb_ras ras = get_instr_ras_type(instr);

    switch (ras) {
    case none:
        return none;

    case ret:
        if (kv_size(*stack) == 0)
            return -trdb_bad_ras;
        *ret_addr = kv_pop(*stack);
        dbg(c, "return to: %" PRIxADDR "\n", *ret_addr);
        return ret;

    case coret:
        dbg(c, "coret call/ret: %" PRIxADDR "\n", addr + (compressed ? 2 : 4));
        if (kv_size(*stack) == 0)
            return -trdb_bad_ras;
        *ret_addr = kv_pop(*stack);
        kv_push(addr_t, *stack, addr + (compressed ? 2 : 4));
        return coret;

    case call:
        dbg(c, "pushing to stack: %" PRIxADDR "\n",
            addr + (compressed ? 2 : 4));
        kv_push(addr_t, *stack, addr + (compressed ? 2 : 4));
        return call;
    }
    return none;
}

/* Try to read instruction at @p pc into @p instr. It uses read_memory_func()
 * which is set using libopcodes.
 */
static int read_memory_at_pc(bfd_vma pc, uint64_t *instr,
                             struct disassemble_info *dinfo)
{
    bfd_byte packet[2];
    *instr = 0;
    bfd_vma n;
    int status = 0;

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

/* Use libopcodes to disassemble @p instr at address @p pc. Any potential error
 * is signaled in @p status. The decoding algorithm tries to recover as much
 * information as possible from the packets.
 */
static int disassemble_at_pc(struct trdb_ctx *c, bfd_vma pc,
                             struct tr_instr *instr,
                             struct disassembler_unit *dunit, int *status)
{
    *status = 0;

    if (!c || !instr || !dunit) {
        *status = -trdb_invalid;
        return 0;
    }

    /* make sure start froma a clean slate */
    *instr = (struct tr_instr){0};

    struct disassemble_info *dinfo = dunit->dinfo;
    /* Important to set for internal calls to fprintf */
    /* c->dis_instr  = instr; */
    dinfo->stream = c;

    /* print instr address */
    (*dinfo->fprintf_func)(c, "0x%08jx  ", (uintmax_t)pc);

    /* check if insn_info works */
    dinfo->insn_info_valid = 0;

    int instr_size = (*dunit->disassemble_fn)(pc, dinfo);
    (*dinfo->fprintf_func)(c, "\n");
    if (instr_size <= 0) {
        err(c, "encountered instruction with %d bytes, stopping\n", instr_size);
        *status = -trdb_bad_instr;
        return 0;
    }
    if (!dinfo->insn_info_valid) {
        err(c, "encountered invalid instruction info\n");
        *status = -trdb_bad_instr;
        return 0;
    }
    uint64_t instr_bits = 0;
    if (read_memory_at_pc(pc, &instr_bits, dinfo)) {
        err(c, "reading instr at pc failed\n");
        *status = -trdb_bad_instr;
        return 0;
    }

    instr->valid      = true;
    instr->iaddr      = pc;
    instr->instr      = (insn_t)instr_bits;
    instr->compressed = instr_size == 2;
    /* instr->priv = 0 */
    return instr_size;
}

/* Libopcodes only knows how to call a fprintf based callback function. We abuse
 * it by passing through the void pointer our custom data (instead of a stream).
 * This ugly hack doesn't seem to be used by just me.
 */
static int build_instr_fprintf(void *stream, const char *format, ...)
{
    struct trdb_ctx *c = stream;
    char tmp[INSTR_STR_LEN];
    va_list args;
    va_start(args, format);
    int rv = vsnprintf(tmp, INSTR_STR_LEN - 1, format, args);
    if (rv >= INSTR_STR_LEN) {
        err(c, "build_instr_fprintf output truncated, adjust buffer size\n");
    }
    if (rv < 0) {
        err(c, "Encountered an error in vsnprintf\n");
    }
    va_end(args);

    dbg(c, "%s", tmp);

    return rv;
}

/* Free up any resources we allocated for disassembling */
static void free_section_for_debugging(struct disassemble_info *dinfo)
{
    if (!dinfo)
        return;
    free(dinfo->buffer);
    dinfo->buffer        = NULL;
    dinfo->buffer_vma    = 0;
    dinfo->buffer_length = 0;
    dinfo->section       = NULL;
}

/* Load the section given by @p section from @p abfd into @p dinfo. */
static int alloc_section_for_debugging(struct trdb_ctx *c, bfd *abfd,
                                       asection *section,
                                       struct disassemble_info *dinfo)
{
    if (!dinfo || !section)
        return -trdb_invalid;

    bfd_size_type section_size = bfd_get_section_size(section);

    bfd_byte *section_data = malloc(section_size);
    if (!section_data)
        return -trdb_nomem;

    if (!bfd_get_section_contents(abfd, section, section_data, 0,
                                  section_size)) {
        err(c, "bfd_get_section_contents: %s\n", bfd_errmsg(bfd_get_error()));
        free(section_data);
        return -trdb_section_empty;
    }

    dinfo->buffer        = section_data;
    dinfo->buffer_vma    = section->vma;
    dinfo->buffer_length = section_size;
    dinfo->section       = section;
    return 0;
}

/* Allocate memory and the new instruction @p instr to @p instr_list. */
static int add_trace(struct trdb_ctx *c, struct trdb_instr_head *instr_list,
                     struct tr_instr *instr)
{
    (void)c;
    struct tr_instr *add = malloc(sizeof(*add));
    if (!add)
        return -trdb_nomem;

    memcpy(add, instr, sizeof(*add));
    TAILQ_INSERT_TAIL(instr_list, add, list);
    return 0;
}

/* TODO: packet wise decompression with error events */
int trdb_decompress_trace(struct trdb_ctx *c, bfd *abfd,
                          struct trdb_packet_head *packet_list,
                          struct trdb_instr_head *instr_list)
{
    int status = 0;
    if (!c || !abfd || !packet_list || !instr_list)
        return -trdb_invalid;

    /* We assume our hw block in the pulp generated little endian
     * addresses, thus we should make sure that before we interact
     * with bfd addresses to convert this foreign format to the local
     * host format
     */
    /* TODO: supports only statically linked elf executables */
    struct trdb_config *config = &c->config;
    bool full_address          = config->full_address;
    bool implicit_ret          = config->implicit_ret;

    /* define disassembler stuff */
    struct disassembler_unit dunit = {0};
    struct disassemble_info dinfo  = {0};
    /*TODO: move this into trdb_ctx so that we can continuously decode pieces?*/
    struct trdb_decompress *dec_ctx = c->dec;
    struct trdb_stack *ras          = &c->dec->call_stack;
    struct tr_instr *dis_instr      = c->dis_instr;

    /* find section belonging to start_address */
    bfd_vma start_address = abfd->start_address;
    asection *section     = trdb_get_section_for_vma(abfd, start_address);
    if (!section) {
        err(c, "VMA not pointing to any section\n");
        status = -trdb_bad_vma;
        goto fail;
    }
    info(c, "Section of start_address:%s\n", section->name);

    dunit.dinfo = &dinfo;
    /* TODO: remove that stuff, goes into global context */
    trdb_init_disassembler_unit(&dunit, abfd, "no-aliases");
    /* advanced fprintf output handling */
    dunit.dinfo->fprintf_func = build_instr_fprintf;
    /* dunit.dinfo->stream = &instr; */

    /* Alloc and config section data for disassembler */
    bfd_vma stop_offset = bfd_get_section_size(section) / dinfo.octets_per_byte;

    if ((status = alloc_section_for_debugging(c, abfd, section, &dinfo)) < 0)
        goto fail;

    bfd_vma pc = start_address; /* TODO: well we get a sync packet anyway... */

    struct tr_packet *packet = NULL;

    TAILQ_FOREACH (packet, packet_list, list) {
        /* we ignore unknown or unused packets (TIMER, SW) */
        if (packet->msg_type != W_TRACE) {
            info(c, "skipped a packet\n");
            continue;
        }

        /* Sometimes we leave the current section (e.g. changing from
         * the .start to the .text section), so let's load the
         * appropriate section and remove the old one
         */
        if (pc >= section->vma + stop_offset || pc < section->vma) {
            section = trdb_get_section_for_vma(abfd, pc);
            if (!section) {
                err(c, "VMA (PC) not pointing to any section\n");
                status = -trdb_bad_vma;
                goto fail;
            }
            stop_offset = bfd_get_section_size(section) / dinfo.octets_per_byte;

            free_section_for_debugging(&dinfo);
            if ((status =
                     alloc_section_for_debugging(c, abfd, section, &dinfo)) < 0)
                goto fail;

            info(c, "switched to section:%s\n", section->name);
        }

        switch (packet->format) {
        case F_BRANCH_FULL:
        case F_BRANCH_DIFF:
            dec_ctx->branch_map.cnt  = packet->branches;
            dec_ctx->branch_map.bits = packet->branch_map;
            dec_ctx->branch_map.full =
                (packet->branches == 31) || (packet->branches == 0);
            break;
        case F_SYNC:
            break;
        case F_ADDR_ONLY:
            break;
        default:
            /* impossible */
            status = -trdb_bad_packet;
            goto fail;
        }

        trdb_log_packet(c, packet);

        if (packet->format == F_BRANCH_FULL) {
            bool hit_address = false; /* TODO: for now we don't allow
                                       * resync packets, see todo.org
                                       */
            /* We don't need to care about the address field if the branch map
             * is full,(except if full and instr before last branch is
             * discontinuity)
             */
            bool hit_discontinuity = dec_ctx->branch_map.full;

            /* Remember last packet address to be able to compute differential
             * address. Careful, a full branch map doesn't always have an
             * address field
             * TODO: edgecase where we sign extend from msb of branchmap
             */
            addr_t absolute_addr = packet->address;

            if (dec_ctx->branch_map.cnt > 0)
                dec_ctx->last_packet_addr = absolute_addr;

            /* this indicates we don't have a valid address but still a full
             * branch map
             */
            if (dec_ctx->branch_map.cnt == 0)
                dec_ctx->branch_map.cnt = 31;

            while (!(dec_ctx->branch_map.cnt == 0 &&
                     (hit_discontinuity || hit_address))) {

                if (pc >= section->vma + stop_offset || pc < section->vma) {
                    section = trdb_get_section_for_vma(abfd, pc);
                    if (!section) {
                        err(c, "VMA (PC) not pointing to any section\n");
                        status = -trdb_bad_vma;
                        goto fail;
                    }
                    stop_offset =
                        bfd_get_section_size(section) / dinfo.octets_per_byte;

                    free_section_for_debugging(&dinfo);
                    if ((status = alloc_section_for_debugging(c, abfd, section,
                                                              &dinfo)) < 0)
                        goto fail;

                    info(c, "switched to section:%s\n", section->name);
                }

                int size = disassemble_at_pc(c, pc, dis_instr, &dunit, &status);
                if (status < 0)
                    goto fail;

                /* TODO: test if packet addr valid first */
                if (dec_ctx->branch_map.cnt == 0 && pc == absolute_addr)
                    hit_address = true;

                /* handle decoding RAS */
                addr_t ret_addr = 0;

                int ras_ret = update_ras(c, dis_instr->instr, dis_instr->iaddr,
                                         ras, &ret_addr);
                if (ras_ret < 0) {
                    err(c, "return address stack in bad state: %s\n",
                        trdb_errstr(trdb_errcode(status)));
                    goto fail;
                }
                enum trdb_ras instr_ras_type = ras_ret;

                if (instr_ras_type == coret) {
                    err(c, "coret not implemented yet\n");
                    goto fail;
                }
                /* generate decoded trace */
                dis_instr->priv = dec_ctx->privilege;
                if ((status = add_trace(c, instr_list, dis_instr)) < 0)
                    goto fail;

                /* advance pc */
                pc += size;

                /* we hit a conditional branch, follow or not and update map */
                switch (dinfo.insn_type) {
                case dis_nonbranch:
                    /* TODO: we need this hack since {m,s,u} ret are not
                     * "classified" by libopcodes
                     */
                    if (!is_unpred_discontinuity(dis_instr->instr,
                                                 implicit_ret)) {
                        break;
                    }
                    dbg(c, "detected mret, uret or sret\n");
                    /* fall through */
                case dis_jsr: /* There is not real difference ... */

                    /* fall through */
                case dis_branch: /* ... between those two */
                    /* we know that this instruction must have its jump target
                     * encoded in the binary else we would have gotten a
                     * non-predictable discontinuity packet. If
                     * branch_map.cnt == 0 + jump target unknown and we are here
                     * then we know that its actually a branch_map
                     * flush + discontinuity packet.
                     */
                    if (implicit_ret && instr_ras_type == ret) {
                        dbg(c, "returning with stack value %" PRIxADDR "\n",
                            ret_addr);
                        pc = ret_addr;
                        break;
                    }

                    /* this should never happen */
                    if (dec_ctx->branch_map.cnt > 1 && dinfo.target == 0)
                        err(c, "can't predict the jump target\n");

                    if (dec_ctx->branch_map.cnt == 1 && dinfo.target == 0) {
                        if (!dec_ctx->branch_map.full) {
                            info(
                                c,
                                "we hit the not-full branch_map + address edge case, "
                                "(branch following discontinuity is included in this "
                                "packet)\n");
                            /* TODO: should I poison addr? */
                            pc = absolute_addr;
                        } else {
                            info(
                                c,
                                "we hit the full branch_map + address edge case\n");
                            pc = absolute_addr;
                        }
                        hit_discontinuity = true;
                    } else if (dec_ctx->branch_map.cnt > 0 ||
                               dinfo.target != 0) {
                        /* we should not hit unpredictable
                         * discontinuities
                         */
                        pc = dinfo.target;
                    } else {
                        /* we finally hit a jump with unknown  destination,
                         * thus the information in this packet  is used up
                         */
                        pc                = absolute_addr;
                        hit_discontinuity = true;
                        info(c, "found discontinuity\n");
                    }
                    break;

                case dis_condbranch:
                    /* this case allows us to exhaust the branch bits */
                    {
                        /* 32 would be undefined */
                        bool branch_taken = !(dec_ctx->branch_map.bits & 1);
                        dec_ctx->branch_map.bits >>= 1;
                        dec_ctx->branch_map.cnt--;
                        /* this should never happen */
                        if (dinfo.target == 0)
                            err(c, "can't predict the jump target\n");
                        if (branch_taken)
                            pc = dinfo.target;
                        /* see in F_BRANCH_DIFF below why we need this */
                        if (dec_ctx->branch_map.cnt == 0 &&
                            (pc - size) == absolute_addr)
                            hit_address = true;
                        break;
                    }
                case dis_dref:
                    /* err(c, "Don't know what to do with this type\n"); */
                    break;

                case dis_dref2:
                case dis_condjsr:
                case dis_noninsn:
                    err(c, "invalid insn_type: %d\n", dinfo.insn_type);
                    status = -trdb_bad_instr;
                    goto fail;
                }
            }
        } else if (packet->format == F_BRANCH_DIFF) {
            if (full_address) {
                err(c,
                    "F_BRANCH_DIFF shouldn't happen, decoder configured with full_address\n");
                status = -trdb_bad_config;
                goto fail;
            }

            /* We have to find the instruction where we can apply the address
             * information to. This might either be a discontinuity information
             * or a sync up address. Furthermore we have to calculate the
             * absolute address we are referring to.
             */
            bool hit_address = false;
            /* We don't need to care about the address field if the branch map
             * is full,(except if full and instr before last branch is
             * discontinuity)
             */
            bool hit_discontinuity = dec_ctx->branch_map.full;

            addr_t absolute_addr = dec_ctx->last_packet_addr - packet->address;

            dbg(c,
                "F_BRANCH_DIFF resolved address:%" PRIxADDR " from %" PRIxADDR
                " - %" PRIxADDR "\n",
                absolute_addr, dec_ctx->last_packet_addr, packet->address);

            /* Remember last packet address to be able to compute differential
             * address. Careful, a full branch map doesn't always have an
             * address field
             * TODO: edgecase where we sign extend from msb of branchmap
             */
            if (dec_ctx->branch_map.cnt > 0)
                dec_ctx->last_packet_addr = absolute_addr;
            /* this indicates we don't have a valid address but still a full
             * branch map
             */
            if (dec_ctx->branch_map.cnt == 0)
                dec_ctx->branch_map.cnt = 31;

            while (!(dec_ctx->branch_map.cnt == 0 &&
                     (hit_discontinuity || hit_address))) {

                if (pc >= section->vma + stop_offset || pc < section->vma) {
                    section = trdb_get_section_for_vma(abfd, pc);
                    if (!section) {
                        err(c, "VMA (PC) not pointing to any section\n");
                        status = -trdb_bad_vma;
                        goto fail;
                    }
                    stop_offset =
                        bfd_get_section_size(section) / dinfo.octets_per_byte;

                    free_section_for_debugging(&dinfo);
                    if ((status = alloc_section_for_debugging(c, abfd, section,
                                                              &dinfo)) < 0)
                        goto fail;

                    info(c, "switched to section:%s\n", section->name);
                }

                int size = disassemble_at_pc(c, pc, dis_instr, &dunit, &status);
                if (status < 0)
                    goto fail;

                if (dec_ctx->branch_map.cnt == 0 && pc == absolute_addr)
                    hit_address = true;

                /* handle decoding RAS */
                addr_t ret_addr              = 0;
                enum trdb_ras instr_ras_type = update_ras(
                    c, dis_instr->instr, dis_instr->iaddr, ras, &ret_addr);
                if (instr_ras_type < 0) {
                    err(c, "return address stack in bad state: %s\n",
                        trdb_errstr(trdb_errcode(status)));
                    goto fail;
                }
                if (instr_ras_type == coret) {
                    err(c, "coret not implemented yet\n");
                    goto fail;
                }

                /* generate decoded trace */
                dis_instr->priv = dec_ctx->privilege;
                if ((status = add_trace(c, instr_list, dis_instr)) < 0)
                    goto fail;

                /* advance pc */
                pc += size;

                /* we hit a conditional branch, follow or not and update map */
                switch (dinfo.insn_type) {
                case dis_nonbranch:
                    /* TODO: we need this hack since {m,s,u} ret are not
                     * "classified" by libopcodes
                     */
                    if (!is_unpred_discontinuity(dis_instr->instr,
                                                 implicit_ret)) {
                        break;
                    }
                    dbg(c, "detected mret, uret or sret\n");
                    /* fall through */
                case dis_jsr: /* There is not real difference ... */

                    /* fall through */
                case dis_branch: /* ... between those two */
                    /* we know that this instruction must have its jump target
                     * encoded in the binary else we would have gotten a
                     * non-predictable discontinuity packet. If
                     * branch_map.cnt == 0 + jump target unknown and we are here
                     * then we know that its actually a branch_map
                     * flush + discontinuity packet.
                     */
                    if (implicit_ret && instr_ras_type == ret) {
                        dbg(c, "returning with stack value %" PRIxADDR "\n",
                            ret_addr);
                        pc = ret_addr;
                        break;
                    }

                    /* this should never happen */
                    if (dec_ctx->branch_map.cnt > 1 && dinfo.target == 0)
                        err(c, "can't predict the jump target\n");

                    if (dec_ctx->branch_map.cnt == 1 && dinfo.target == 0) {
                        if (!dec_ctx->branch_map.full) {
                            info(
                                c,
                                "we hit the not-full branch_map + address edge case, "
                                "(branch following discontinuity is included in this "
                                "packet)\n");
                            /* TODO: should I poison addr? */
                            pc = absolute_addr;
                        } else {
                            info(
                                c,
                                "we hit the full branch_map + address edge case\n");
                            pc = absolute_addr;
                        }
                        hit_discontinuity = true;
                    } else if (dec_ctx->branch_map.cnt > 0 ||
                               dinfo.target != 0) {
                        /* we should not hit unpredictable discontinuities */
                        pc = dinfo.target;
                    } else {
                        /* we finally hit a jump with unknown destination, thus
                         * the information in this packet is used up
                         */
                        pc                = absolute_addr;
                        hit_discontinuity = true;
                        info(c, "found discontinuity\n");
                    }
                    break;

                case dis_condbranch:
                    /* this case allows us to exhaust the branch bits */
                    {
                        /* 32 would be undefined */
                        bool branch_taken = !(dec_ctx->branch_map.bits & 1);
                        dec_ctx->branch_map.bits >>= 1;
                        dec_ctx->branch_map.cnt--;
                        /* this should never happen */
                        if (dinfo.target == 0)
                            err(c, "can't predict the jump target\n");
                        /* go to new address */
                        if (branch_taken)
                            pc = dinfo.target;
                        /* When dealing with exceptions, a "flush" packet can
                         * have its address set to a branch. The branchmap
                         * counter will be incremented in that case. The
                         * behaviour we want is that we want to consume a
                         * branchmap entry and the address entry at the same
                         * time. This we have to check here.
                         *
                         * Example:
                         * 0x3c  [...]
                         * [...] assume 3 branches seen
                         * 0x40  bneq a0, a1, somewhere <- F_BRANCH_*
                         *       with branchmap count = 3 + 1 and addr = 0x40
                         * 0x100 first instruction of trap handler <- F_SYNC
                         */
                        if (dec_ctx->branch_map.cnt == 0 &&
                            ((pc - size) == absolute_addr))
                            hit_address = true;
                        break;
                    }
                case dis_dref:
                    /* err(c, "Don't know what to do with this type\n"); */
                    break;

                case dis_dref2:
                case dis_condjsr:
                case dis_noninsn:
                    err(c, "invalid insn_type: %d\n", dinfo.insn_type);
                    status = -trdb_bad_instr;
                    goto fail;
                }
            }

        } else if (packet->format == F_SYNC) {
            /* Sync pc. */
            dec_ctx->privilege = packet->privilege;
            pc                 = packet->address;

            /* Remember last packet address to be able to compute differential
             * addresses
             */
            dec_ctx->last_packet_addr = packet->address;

            /* since we are abruptly changing the pc we have to check if we
             * leave the section before we can disassemble. This is really ugly
             * TODO: fix
             */
            if (pc >= section->vma + stop_offset || pc < section->vma) {
                section = trdb_get_section_for_vma(abfd, pc);
                if (!section) {
                    err(c, "VMA (PC) not pointing to any section\n");
                    status = -trdb_bad_vma;
                    goto fail;
                }
                stop_offset =
                    bfd_get_section_size(section) / dinfo.octets_per_byte;

                free_section_for_debugging(&dinfo);
                if ((status = alloc_section_for_debugging(c, abfd, section,
                                                          &dinfo)) < 0)
                    goto fail;

                info(c, "switched to section:%s\n", section->name);
            }

            int status = 0;
            int size   = disassemble_at_pc(c, pc, dis_instr, &dunit, &status);
            if (status < 0)
                goto fail;

            /* TODO: warning, RAS handling is missing here */
            dis_instr->priv = dec_ctx->privilege;
            if ((status = add_trace(c, instr_list, dis_instr)) < 0)
                goto fail;

            pc += size;

            switch (dinfo.insn_type) {
            case dis_nonbranch:
                /* TODO: we need this hack since {m,s,u} ret are not
                 * "classified"" in libopcodes
                 */
                if (!is_unpred_discontinuity(dis_instr->instr, implicit_ret)) {
                    break;
                }
                dbg(c, "detected mret, uret or sret\n");
                /* fall through */
            case dis_jsr: /* There is not real difference ... */

                /* fall through */
            case dis_branch: /* ... between those two */
                             /* this should never happen */
                if (dinfo.target == 0)
                    err(c, "can't predict the jump target\n");
                pc = dinfo.target;
                break;

            case dis_condbranch:
                /* this should never happen */
                if (dinfo.target == 0)
                    err(c, "can't predict the jump target\n");
                if (packet->branch == 0) {
                    err(c, "doing a branch from a F_SYNC packet\n");
                    pc = dinfo.target;
                }
                break;
            case dis_dref: /* TODO: is this useful? */
                break;
            case dis_dref2:
            case dis_condjsr:
            case dis_noninsn:
                err(c, "invalid insn_type: %d\n", dinfo.insn_type);
                status = -trdb_bad_instr;
                goto fail;
            }
        } else if (packet->format == F_ADDR_ONLY) {
            /* Thoughts on F_ADDR_ONLY packets:
             *
             * We have to find the instruction where we can apply the address
             * information to. This might either be a "discontinuity
             * information", that is the target address of e.g. a jalr, or a the
             * address itself. The latter case e.g. happens when we get a packet
             * due to stopping the tracing, kind of a delimiter packet. So what
             * we do is we stop either at the given address or use it on a
             * discontinuity, whichever comes first. This means there can be an
             * ambiguity between whether a packet is meant for an address or
             * for a jump target. This could happen if we have jr infinite loop
             * with the only way out being exception.
             *
             * Resync packets have that issue too, namely that we can't
             * distinguishing between address sync and unpredictable
             * discontinuities.
             */
            bool hit_address       = false;
            bool hit_discontinuity = false;

            addr_t absolute_addr = 0;
            if (full_address) {
                absolute_addr = packet->address;
            } else {
                /* absolute_addr = dec_ctx->last_packet_addr - sext_addr; */
                absolute_addr = dec_ctx->last_packet_addr - packet->address;
            }

            dbg(c,
                "F_ADDR_ONLY resolved address:%" PRIxADDR " from %" PRIxADDR
                " - %" PRIxADDR "\n",
                absolute_addr, dec_ctx->last_packet_addr, packet->address);

            /* Remember last packet address to be able to compute differential
             * addresses
             */
            dec_ctx->last_packet_addr = absolute_addr;

            while (!(hit_address || hit_discontinuity)) {
                int status = 0;

                if (pc >= section->vma + stop_offset || pc < section->vma) {
                    section = trdb_get_section_for_vma(abfd, pc);
                    if (!section) {
                        err(c, "VMA (PC) not pointing to any section\n");
                        status = -trdb_bad_vma;
                        goto fail;
                    }
                    stop_offset =
                        bfd_get_section_size(section) / dinfo.octets_per_byte;

                    free_section_for_debugging(&dinfo);
                    if ((status = alloc_section_for_debugging(c, abfd, section,
                                                              &dinfo)) < 0)
                        goto fail;

                    info(c, "switched to section:%s\n", section->name);
                }

                int size = disassemble_at_pc(c, pc, dis_instr, &dunit, &status);
                if (status < 0)
                    goto fail;

                if (pc == absolute_addr)
                    hit_address = true;

                /* handle decoding RAS */
                addr_t ret_addr              = 0;
                enum trdb_ras instr_ras_type = update_ras(
                    c, dis_instr->instr, dis_instr->iaddr, ras, &ret_addr);
                if (instr_ras_type < 0) {
                    err(c, "return address stack in bad state: %s\n",
                        trdb_errstr(trdb_errcode(status)));
                    goto fail;
                }
                if (instr_ras_type == coret) {
                    err(c, "coret not implemented yet\n");
                    goto fail;
                }

                /* generate decoded trace */
                dis_instr->priv = dec_ctx->privilege;
                if ((status = add_trace(c, instr_list, dis_instr)) < 0)
                    goto fail;

                /* advance pc */
                pc += size;

                switch (dinfo.insn_type) {
                case dis_nonbranch:
                    /* TODO: we need this hack since {m,s,u} ret are not
                     * "classified" by libopcodes
                     */
                    if (!is_unpred_discontinuity(dis_instr->instr,
                                                 implicit_ret)) {
                        break;
                    }
                    dbg(c, "detected mret, uret or sret\n");

                    /* fall through */
                case dis_jsr: /* There is not real difference ... */

                    /* fall through */
                case dis_branch: /* ... between those two */
                    if (implicit_ret && instr_ras_type == ret) {
                        dbg(c, "returning with stack value %" PRIxADDR "\n",
                            ret_addr);
                        pc = ret_addr;
                        break;
                    }

                    if (dinfo.target) {
                        pc = dinfo.target;
                    } else {
                        info(c, "found the discontinuity\n");
                        pc                = absolute_addr;
                        hit_discontinuity = true;
                    }
                    break;

                case dis_condbranch:
                    err(c,
                        "we shouldn't hit conditional branches with F_ADDRESS_ONLY\n");
                    break;

                case dis_dref: /* TODO: is this useful? */
                    break;
                case dis_dref2:
                case dis_condjsr:
                case dis_noninsn:
                    err(c, "invalid insn_type: %d\n", dinfo.insn_type);
                    status = -trdb_bad_instr;
                    goto fail;
                }
            }
        }
    }
    free_section_for_debugging(&dinfo);
    return status;

fail:
    free_section_for_debugging(&dinfo);
    return status;
}

void trdb_disassemble_trace(size_t len, struct tr_instr trace[len],
                            struct disassembler_unit *dunit)
{
    struct disassemble_info *dinfo = dunit->dinfo;
    for (size_t i = 0; i < len; i++) {
        (*dinfo->fprintf_func)(dinfo->stream, "0x%08jx  ",
                               (uintmax_t)trace[i].iaddr);
        (*dinfo->fprintf_func)(dinfo->stream, "0x%08jx  ",
                               (uintmax_t)trace[i].instr);
        (*dinfo->fprintf_func)(dinfo->stream, "%s",
                               trace[i].exception ? "TRAP!  " : "");
        trdb_disassemble_single_instruction(trace[i].instr, trace[i].iaddr,
                                            dunit);
    }
}

void trdb_disassemble_trace_with_bfd(struct trdb_ctx *c, size_t len,
                                     struct tr_instr trace[len], bfd *abfd,
                                     struct disassembler_unit *dunit)
{
    struct disassemble_info *dinfo = dunit->dinfo;
    for (size_t i = 0; i < len; i++) {
        (*dinfo->fprintf_func)(dinfo->stream, "%s",
                               trace[i].exception ? "TRAP!  " : "");
        trdb_disassemble_instruction_with_bfd(c, abfd, trace[i].iaddr, dunit);
    }
}

void trdb_disassemble_instr(struct tr_instr *instr,
                            struct disassembler_unit *dunit)
{
    struct disassemble_info *dinfo = dunit->dinfo;
    (*dinfo->fprintf_func)(dinfo->stream, "0x%08jx  ", (uintmax_t)instr->iaddr);
    (*dinfo->fprintf_func)(dinfo->stream, "0x%08jx  ", (uintmax_t)instr->instr);
    (*dinfo->fprintf_func)(dinfo->stream, "%s",
                           instr->exception ? "TRAP!  " : "");

    trdb_disassemble_single_instruction(instr->instr, instr->iaddr, dunit);
}

void trdb_disassemble_instr_with_bfd(struct trdb_ctx *c, struct tr_instr *instr,
                                     bfd *abfd, struct disassembler_unit *dunit)
{
    struct disassemble_info *dinfo = dunit->dinfo;
    (*dinfo->fprintf_func)(dinfo->stream, "%s",
                           instr->exception ? "TRAP!  " : "");
    trdb_disassemble_instruction_with_bfd(c, abfd, instr->iaddr, dunit);
}

void trdb_dump_packet_list(FILE *stream,
                           const struct trdb_packet_head *packet_list)
{
    struct tr_packet *packet;
    TAILQ_FOREACH (packet, packet_list, list) {
        trdb_print_packet(stream, packet);
    }
}

void trdb_dump_instr_list(FILE *stream,
                          const struct trdb_instr_head *instr_list)
{
    struct tr_instr *instr;
    TAILQ_FOREACH (instr, instr_list, list) {
        trdb_print_instr(stream, instr);
    }
}

void trdb_log_packet(struct trdb_ctx *c, const struct tr_packet *packet)
{
    if (!c)
        return;

    if (!packet) {
        dbg(c, "error printing packet\n");
        return;
    }

    switch (packet->msg_type) {
    case W_TRACE:
        switch (packet->format) {
        case F_BRANCH_FULL:
        case F_BRANCH_DIFF:
            dbg(c, "PACKET ");
            if (packet->format == F_BRANCH_FULL)
                dbg(c, "0: F_BRANCH_FULL\n");
            else
                dbg(c, "1: F_BRANCH_DIFF\n");

            dbg(c, "    branches  : %" PRIu32 "\n", packet->branches);
            dbg(c, "    branch_map: 0x%" PRIx32 "\n", packet->branch_map);
            dbg(c, "    address   : 0x%" PRIxADDR "\n", packet->address);
            /* TODO: that special full branch map behaviour */
            break;

        case F_ADDR_ONLY:
            dbg(c, "PACKET 2: F_ADDR_ONLY\n");
            dbg(c, "    address   : 0x%" PRIxADDR "\n", packet->address);
            break;
        case F_SYNC:
            dbg(c, "PACKET 3: F_SYNC\n");
            const char *subf[4];
            subf[0] = "SF_START";
            subf[1] = "SF_EXCEPTION";
            subf[2] = "SF_CONTEXT";
            subf[3] = "RESERVED";
            dbg(c, "    subformat : %s\n", subf[packet->subformat]);

            /* TODO fix this */
            dbg(c, "    context   :\n");
            dbg(c, "    privilege : 0x%" PRIx32 "\n", packet->privilege);
            if (packet->subformat == SF_CONTEXT)
                return;

            dbg(c, "    branch    : %s\n", packet->branch ? "true" : "false");
            dbg(c, "    address   : 0x%" PRIxADDR "\n", packet->address);
            if (packet->subformat == SF_START)
                return;

            dbg(c, "    ecause    : 0x%" PRIx32 "\n", packet->ecause);
            dbg(c, "    interrupt : %s\n",
                packet->interrupt ? "true" : "false");
            dbg(c, "    tval      : 0x%" PRIxADDR "\n", packet->tval);
            /* SF_EXCEPTION */
        }
        break;

    case W_SOFTWARE:
        dbg(c, "PACKET W_SOFTWARE\n");
        dbg(c, "    userdata  : 0x%" PRIx32 "\n", packet->userdata);
        break;

    case W_TIMER:
        dbg(c, "PACKET W_TIMER\n");
        dbg(c, "    time : %" PRIu64 "\n", packet->time);
        break;
    }
}

void trdb_print_packet(FILE *stream, const struct tr_packet *packet)
{
    if (!stream)
        return;

    if (!packet) {
        fprintf(stream, "error printing packet\n");
        return;
    }

    switch (packet->msg_type) {
    case W_TRACE:
        switch (packet->format) {
        case F_BRANCH_FULL:
        case F_BRANCH_DIFF:
            fprintf(stream, "PACKET ");
            packet->format == F_BRANCH_FULL
                ? fprintf(stream, "0: F_BRANCH_FULL\n")
                : fprintf(stream, "1: F_BRANCH_DIFF\n");
            fprintf(stream, "    branches  : %" PRIu32 "\n", packet->branches);
            fprintf(stream, "    branch_map: 0x%" PRIx32 "\n",
                    packet->branch_map);
            fprintf(stream, "    address   : 0x%" PRIxADDR "\n",
                    packet->address);
            /* TODO: that special full branch map behaviour */
            break;
        case F_ADDR_ONLY:
            fprintf(stream, "PACKET 2: F_ADDR_ONLY\n");
            fprintf(stream, "    address   : 0x%" PRIxADDR "\n",
                    packet->address);
            break;
        case F_SYNC:
            fprintf(stream, "PACKET 3: F_SYNC\n");
            const char *subf[4];
            subf[0] = "SF_START";
            subf[1] = "SF_EXCEPTION";
            subf[2] = "SF_CONTEXT";
            subf[3] = "RESERVED";
            fprintf(stream, "    subformat : %s\n", subf[packet->subformat]);

            /* TODO fix this */
            fprintf(stream, "    context   :\n");
            fprintf(stream, "    privilege : 0x%" PRIx32 "\n",
                    packet->privilege);
            if (packet->subformat == SF_CONTEXT)
                return;

            fprintf(stream, "    branch    : %s\n",
                    packet->branch ? "true" : "false");
            fprintf(stream, "    address   : 0x%" PRIxADDR "\n",
                    packet->address);
            if (packet->subformat == SF_START)
                return;

            fprintf(stream, "    ecause    : 0x%" PRIx32 "\n", packet->ecause);
            fprintf(stream, "    interrupt : %s\n",
                    packet->interrupt ? "true" : "false");
            fprintf(stream, "    tval      : 0x%" PRIxADDR "\n", packet->tval);
            /* SF_EXCEPTION */
        }
        break;

    case W_SOFTWARE:
        fprintf(stream, "PACKET W_SOFTWARE\n");
        fprintf(stream, "    userdata  : 0x%" PRIx32 "\n", packet->userdata);
        break;

    case W_TIMER:
        fprintf(stream, "PACKET W_TIMER\n");
        fprintf(stream, "    time : %" PRIu64 "\n", packet->time);
        break;
    }
}

void trdb_log_instr(struct trdb_ctx *c, const struct tr_instr *instr)
{
    if (!c)
        return;

    if (!instr) {
        dbg(c, "error logging instruction\n");
        return;
    }

    dbg(c, "INSTR\n");
    dbg(c, "    iaddr     : 0x%08" PRIxADDR "\n", instr->iaddr);
    dbg(c, "    instr     : 0x%08" PRIxINSN "\n", instr->instr);
    dbg(c, "    priv      : 0x%" PRIx32 "\n", instr->priv);
    dbg(c, "    exception : %s\n", instr->exception ? "true" : "false");
    dbg(c, "    cause     : 0x%" PRIx32 "\n", instr->cause);
    dbg(c, "    tval      : 0x%" PRIxADDR "\n", instr->tval);
    dbg(c, "    interrupt : %s\n", instr->interrupt ? "true" : "false");
    dbg(c, "    compressed: %s\n", instr->compressed ? "true" : "false");
}

void trdb_print_instr(FILE *stream, const struct tr_instr *instr)
{
    if (!stream)
        return;

    if (!instr) {
        fprintf(stream, "error printing instruction\n");
        return;
    }

    fprintf(stream, "INSTR\n");
    fprintf(stream, "    iaddr     : 0x%08" PRIxADDR "\n", instr->iaddr);
    fprintf(stream, "    instr     : 0x%08" PRIxINSN "\n", instr->instr);
    fprintf(stream, "    priv      : 0x%" PRIx32 "\n", instr->priv);
    fprintf(stream, "    exception : %s\n",
            instr->exception ? "true" : "false");
    fprintf(stream, "    cause     : 0x%" PRIx32 "\n", instr->cause);
    fprintf(stream, "    tval      : 0x%" PRIxADDR "\n", instr->tval);
    fprintf(stream, "    interrupt : %s\n",
            instr->interrupt ? "true" : "false");
    fprintf(stream, "    compressed: %s\n",
            instr->compressed ? "true" : "false");
}

bool trdb_compare_packet()
{
    return false;
}

bool trdb_compare_instr(struct trdb_ctx *c, const struct tr_instr *instr0,
                        const struct tr_instr *instr1)
{
    (void)c;
    if (!instr0 || !instr1)
        return false;

    bool sum = true;
    sum |= instr0->valid == instr1->valid;
    sum |= instr0->exception == instr1->exception;
    sum |= instr0->interrupt == instr1->interrupt;
    sum |= instr0->cause == instr1->cause;
    sum |= instr0->tval == instr1->tval;
    sum |= instr0->priv == instr1->priv;
    sum |= instr0->iaddr == instr1->iaddr;
    sum |= instr0->instr == instr1->instr;
    sum |= instr0->compressed == instr1->compressed;

    return sum;
}

void trdb_free_packet_list(struct trdb_packet_head *packet_list)
{
    if (TAILQ_EMPTY(packet_list))
        return;

    struct tr_packet *packet;
    while (!TAILQ_EMPTY(packet_list)) {
        packet = TAILQ_FIRST(packet_list);
        TAILQ_REMOVE(packet_list, packet, list);
        free(packet);
    }
}

void trdb_free_instr_list(struct trdb_instr_head *instr_list)
{
    if (TAILQ_EMPTY(instr_list))
        return;

    struct tr_instr *instr;
    while (!TAILQ_EMPTY(instr_list)) {
        instr = TAILQ_FIRST(instr_list);
        TAILQ_REMOVE(instr_list, instr, list);
        free(instr);
    }
}

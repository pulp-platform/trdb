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

/*
 * Author: Robert Balas (balasr@student.ethz.ch)
 * Description: Small tests for trdb
 */

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <argp.h>
#include <libgen.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/limits.h>
#include "trace_debugger.h"
#include "disassembly.h"
#include "utils.h"
#include "serialize.h"
#include "workaround.h"

#define TRDB_SUCCESS 0
#define TRDB_FAIL -1

FILE *trs_fp = NULL;
FILE *tee_fp = NULL;

/* function args formatted as string */
#define FUNC_ARGS_SIZE 256
char func_args_buf[FUNC_ARGS_SIZE];

#define INIT_TESTS() bool _trdb_test_result = true;

void tprintf(const char *restrict format, ...)
{
    va_list args = {0};
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
}

#define RUN_TEST(fun, ...)                                                     \
    do {                                                                       \
        memset(func_args_buf, 0, sizeof(func_args_buf));                       \
        if (!fun(__VA_ARGS__)) {                                               \
            printf("PASS: " #fun "(%s)\n", func_args_buf);                     \
            if (trs_fp)                                                        \
                fprintf(trs_fp, ":test-result: PASS " #fun "(%s)\n",           \
                        func_args_buf);                                        \
        } else {                                                               \
            printf("FAIL: " #fun "(%s)\n", func_args_buf);                     \
            if (trs_fp)                                                        \
                fprintf(trs_fp, ":test-result: FAIL " #fun "(%s)\n",           \
                        func_args_buf);                                        \
            _trdb_test_result = false;                                         \
        }                                                                      \
    } while (false)

#define TESTS_SUCCESSFULL() (_trdb_test_result == true)

static void shiftl_array(uint8_t arr[], size_t len, uint32_t shift)
{
    if (shift >= 8) {
        LOG_ERRT("Shift value too large\n");
        return;
    }
    uint32_t carry_in  = 0;
    uint32_t carry_out = 0;
    for (size_t i = 0; i < len; i++) {
        /* carry_out = arr[i] & ~((1ull << (8 - shift)) - 1); */
        carry_out = (arr[i] >> (8 - shift)) & MASK_FROM(shift);
        arr[i] <<= shift;
        arr[i] |= carry_in;
        carry_in = carry_out;
    }
    if (carry_out)
        LOG_ERRT("Non-zero carry after array shifting\n");
    return;
}

static int test_disasm_bfd()
{
    bfd *abfd              = NULL;
    disassemble_info dinfo = {0};

    bfd_init();

    abfd = bfd_openr("data/interrupt", NULL);

    if (!(abfd && bfd_check_format(abfd, bfd_object)))
        return TRDB_FAIL;

    /* Override the stream the disassembler outputs to */
    init_disassemble_info(&dinfo, stdout, (fprintf_ftype)fprintf);
    dinfo.fprintf_func       = (fprintf_ftype)fprintf;
    dinfo.print_address_func = trdb_riscv32_print_address;

    dinfo.flavour = bfd_get_flavour(abfd);
    dinfo.arch    = bfd_get_arch(abfd);
    dinfo.mach    = bfd_get_mach(abfd);
    dinfo.endian  = abfd->xvec->byteorder;
    disassemble_init_for_target(&dinfo);

    /* Tests for disassembly functions */
    if (TRDB_VERBOSE_TESTS) {
        trdb_dump_target_list();
        trdb_dump_bin_info(abfd);
    }

    /* set up disassembly context */
    struct disassembler_unit dunit = {0};
    dunit.dinfo                    = &dinfo;
    dunit.disassemble_fn           = disassembler(
        bfd_get_arch(abfd), bfd_big_endian(abfd), bfd_get_mach(abfd), abfd);
    if (!dunit.disassemble_fn) {
        LOG_ERRT("No suitable disassembler found\n");
        return TRDB_FAIL;
    }
    /* TODO: use this path also in relase mode, but less noisy */
    if (TRDB_VERBOSE_TESTS) {
        trdb_dump_section_names(abfd);

        LOG_INFOT("num_sections: %d\n", bfd_count_sections(abfd));
        trdb_disassemble_single_instruction(0x10, 0, &dunit);
        bfd_map_over_sections(abfd, trdb_disassemble_section, &dunit);
    }
    bfd_close(abfd);
    return TRDB_SUCCESS;
}

static int test_trdb_dinfo_init(char *path)
{
    int status = TRDB_SUCCESS;
    bfd *abfd  = bfd_openr(path, NULL);
    if (!(abfd && bfd_check_format(abfd, bfd_object)))
        return TRDB_FAIL;

    struct trdb_ctx *c = trdb_new();

    struct disassemble_info dinfo  = {0};
    struct disassembler_unit dunit = {0};

    dunit.dinfo = &dinfo;

    if (trdb_alloc_dinfo_with_bfd(c, abfd, &dunit)) {
        status = TRDB_FAIL;
        goto fail;
    }
    if (TRDB_VERBOSE_TESTS)
        bfd_map_over_sections(abfd, trdb_disassemble_section, &dunit);

fail:
    trdb_free_dinfo_with_bfd(c, abfd, &dunit);
    trdb_free(c);
    bfd_close(abfd);
    return status;
}

static int test_parse_packets(const char *path)
{
    int status         = TRDB_SUCCESS;
    struct trdb_ctx *c = trdb_new();
    struct trdb_packet_head packet_list;
    TAILQ_INIT(&packet_list);

    if (trdb_pulp_read_all_packets(c, path, &packet_list)) {
        status = TRDB_FAIL;
        goto fail;
    }

    if (TAILQ_EMPTY(&packet_list)) {
        LOG_ERRT("packet list empty\n");
        status = TRDB_FAIL;
        goto fail;
    }
    struct tr_packet *packet;
    if (TRDB_VERBOSE_TESTS) {
        TAILQ_FOREACH (packet, &packet_list, list) {
            trdb_print_packet(stdout, packet);
        }
    }
fail:
    trdb_free(c);
    trdb_free_packet_list(&packet_list);
    return status;
}

/* static int test_trdb_serialize_packet(uint32_t shift) */
/* { */
/*     int status = TRDB_SUCCESS; */
/*     struct trdb_ctx *c = trdb_new(); */
/*     struct tr_packet packet = {0}; */

/*     /\* Testing F_BRANCH_FULL packet with full branch map *\/ */
/*     packet = (struct tr_packet){.msg_type = 2, */
/*                                 .format = F_BRANCH_FULL, */
/*                                 .branches = 31, */
/*                                 .branch_map = 0x7fffffff, */
/*                                 .address = 0xaadeadbe}; */
/*     /\*                    0xf2,7 + 1, 31-7-8, 31-7-8-8, 31-7-8-8-8*\/ */
/*     uint8_t expected0[] = {0xf2, 0xff, 0xff, 0xff, 0xff, */
/*                            0xbe, 0xad, 0xde, 0xaa, 0x00}; */
/*     shiftl_array(expected0, TRDB_ARRAY_SIZE(expected0), shift); */

/*     /\* this is surely enough space *\/ */
/*     uint8_t *bin = malloc(sizeof(struct tr_packet)); */
/*     memset(bin, 0, sizeof(struct tr_packet)); */

/*     if (!bin) { */
/*         perror("malloc"); */
/*         status = TRDB_FAIL; */
/*         goto fail; */
/*     } */

/*     size_t bitcnt = 0; */
/*     if (trdb_pulp_serialize_packet(c, &packet, &bitcnt, shift, bin)) { */
/*         LOG_ERRT("Packet conversion failed\n"); */
/*         status = TRDB_FAIL; */
/*     } */
/*     if (bitcnt != (2 + 2 + 5 + branch_map_len(packet.branches) + XLEN)) { */
/*         LOG_ERRT("Wrong bit count value: %zu\n", bitcnt); */
/*         status = TRDB_FAIL; */
/*     } */
/*     if (memcmp(bin, expected0, TRDB_ARRAY_SIZE(expected0))) { */
/*         LOG_ERRT("Packet bits don't match\n"); */
/*         status = TRDB_FAIL; */
/*     } */

/*     /\* Testing F_BRANCH_FULL packet with non-full branch map *\/ */
/*     packet = (struct tr_packet){.msg_type = 2, */
/*                                 .format = F_BRANCH_FULL, */
/*                                 .branches = 25, */
/*                                 .branch_map = 0x1ffffff, */
/*                                 .address = 0xaadeadbe}; */

/*     /\*                           7     8     8      2 *\/ */
/*     uint8_t expected1[] = {0x92, 0xff, 0xff, 0xff, 0xfb, */
/*                            0xb6, 0x7a, 0xab, 0x2,  0x00}; */
/*     memset(bin, 0, sizeof(struct tr_packet)); */
/*     bitcnt = 0; */

/*     if (trdb_pulp_serialize_packet(c, &packet, &bitcnt, 0, bin)) { */
/*         LOG_ERRT("Packet conversion failed\n"); */
/*         status = TRDB_FAIL; */
/*     } */
/*     if (bitcnt != (2 + 2 + 5 + branch_map_len(packet.branches) + XLEN)) { */
/*         LOG_ERRT("Wrong bit count value: %zu\n", bitcnt); */
/*         status = TRDB_FAIL; */
/*     } */
/*     if (memcmp(bin, expected1, TRDB_ARRAY_SIZE(expected1))) { */
/*         LOG_ERRT("Packet bits don't match\n"); */
/*         status = TRDB_FAIL; */
/*     } */

/*     /\* Testing F_ADDR_ONLY packet *\/ */
/*     packet = (struct tr_packet){ */
/*         .msg_type = 2, .format = F_ADDR_ONLY, .address = 0xdeadbeef}; */

/*     /\*                           7     8     8      2 *\/ */
/*     uint8_t expected2[] = {0xfa, 0xee, 0xdb, 0xea, 0x0d, 0x00}; */
/*     memset(bin, 0, sizeof(struct tr_packet)); */
/*     bitcnt = 0; */

/*     if (trdb_pulp_serialize_packet(c, &packet, &bitcnt, 0, bin)) { */
/*         LOG_ERRT("Packet conversion failed\n"); */
/*         status = TRDB_FAIL; */
/*     } */
/*     if (bitcnt != (2 + 2 + XLEN)) { */
/*         LOG_ERRT("Wrong bit count value: %zu\n", bitcnt); */
/*         status = TRDB_FAIL; */
/*     } */
/*     if (memcmp(bin, expected2, TRDB_ARRAY_SIZE(expected2))) { */
/*         LOG_ERRT("Packet bits don't match\n"); */
/*         status = TRDB_FAIL; */
/*     } */

/*     /\* Testing F_SYNC start packet *\/ */
/*     packet = (struct tr_packet){.msg_type = 2, */
/*                                 .format = F_SYNC, */
/*                                 .subformat = SF_START, */
/*                                 .privilege = 3, */
/*                                 .branch = 1, */
/*                                 .address = 0xdeadbeef}; */

/*     /\*                           7     8     8      2 *\/ */
/*     uint8_t expected3[] = {0xce, 0xf8, 0xee, 0xdb, 0xea, 0x0d, 0x00}; */
/*     memset(bin, 0, sizeof(struct tr_packet)); */
/*     bitcnt = 0; */

/*     if (trdb_pulp_serialize_packet(c, &packet, &bitcnt, 0, bin)) { */
/*         LOG_ERRT("Packet conversion failed\n"); */
/*         status = TRDB_FAIL; */
/*     } */
/*     if (bitcnt != (6 + PRIVLEN + 1 + XLEN)) { */
/*         LOG_ERRT("Wrong bit count value: %zu\n", bitcnt); */
/*         status = TRDB_FAIL; */
/*     } */
/*     if (memcmp(bin, expected3, TRDB_ARRAY_SIZE(expected3))) { */
/*         LOG_ERRT("Packet bits don't match\n"); */
/*         status = TRDB_FAIL; */
/*     } */

/*     /\* Testing F_SYNC exception packet *\/ */
/*     packet = (struct tr_packet){.msg_type = 2, */
/*                                 .format = F_SYNC, */
/*                                 .subformat = SF_EXCEPTION, */
/*                                 .privilege = 3, */
/*                                 .branch = 1, */
/*                                 .address = 0xdeadbeef, */
/*                                 .ecause = 0x1a, */
/*                                 .interrupt = 1, */
/*                                 .tval = 0xfeebdeed}; */

/*     /\* 0x3fbaf7bb7adeadbeef8de *\/ */
/*     uint8_t expected4[] = {0xde, 0xf8, 0xee, 0xdb, 0xea, 0xad, */
/*                            0xb7, 0x7b, 0xaf, 0xfb, 0x3,  0x00}; */
/*     memset(bin, 0, sizeof(struct tr_packet)); */
/*     bitcnt = 0; */

/*     if (trdb_pulp_serialize_packet(c, &packet, &bitcnt, 0, bin)) { */
/*         LOG_ERRT("Packet conversion failed\n"); */
/*         status = TRDB_FAIL; */
/*     } */
/*     if (bitcnt != (6 + PRIVLEN + 1 + XLEN + CAUSELEN + 1 + XLEN)) { */
/*         LOG_ERRT("Wrong bit count value: %zu\n", bitcnt); */
/*         status = TRDB_FAIL; */
/*     } */
/*     if (memcmp(bin, expected4, TRDB_ARRAY_SIZE(expected4))) { */
/*         LOG_ERRT("Packet bits don't match\n"); */
/*         status = TRDB_FAIL; */
/*     } */

/* fail: */
/*     trdb_free(c); */
/*     free(bin); */
/*     return status; */
/* } */

static int test_parse_stimuli_line()
{
    int valid      = 0;
    int exception  = 0;
    int interrupt  = 0;
    uint32_t cause = 0;
    addr_t tval    = 0;
    uint32_t priv  = 0;
    addr_t iaddr   = 0;
    insn_t instr   = 0;

    int ret = sscanf(
        "valid=1 exception=0 interrupt=0 cause=00 tval=ff priv=7 addr=1c00809c instr=ffff9317",
        "valid= %d exception= %d interrupt= %d cause= %" SCNx32
        " tval= %" SCNxADDR " priv= %" SCNx32 " addr= %" SCNxADDR
        " instr= %" SCNxINSN,
        &valid, &exception, &interrupt, &cause, &tval, &priv, &iaddr, &instr);

    if (ret != EOF) {
    } else if (errno != 0) {
        perror("scanf");
        return TRDB_FAIL;
    } else {
        LOG_ERRT("No matching characters\n");
        return TRDB_FAIL;
    }

    return (valid == 1 && exception == 0 && cause == 0 && tval == 0xff &&
            iaddr == 0x1c00809c && instr == 0xffff9317)
               ? TRDB_SUCCESS
               : TRDB_FAIL;
}

static int test_stimuli_to_tr_instr(const char *path)
{
    struct trdb_ctx *c = trdb_new();
    struct tr_instr *tmp;
    struct tr_instr **samples = &tmp;
    int status                = 0;
    size_t samplecnt;
    status = trdb_stimuli_to_trace(c, path, samples, &samplecnt);
    if (status < 0) {
        LOG_ERRT("Stimuli to tr_instr failed\n");
        return TRDB_FAIL;
    }

    trdb_free(c);
    free(*samples);
    return TRDB_SUCCESS;
}

static int test_stimuli_to_trace_list(const char *path)
{
    struct trdb_ctx *c = trdb_new();
    struct tr_instr *tmp;
    struct tr_instr **samples = &tmp;
    int status                = TRDB_SUCCESS;
    size_t sizea              = 0;
    status                    = trdb_stimuli_to_trace(c, path, samples, &sizea);
    if (status < 0) {
        LOG_ERRT("Stimuli to tr_instr failed\n");
        return TRDB_FAIL;
    }

    struct trdb_instr_head instr_list;
    TAILQ_INIT(&instr_list);

    size_t sizel = 0;
    status       = trdb_stimuli_to_trace_list(c, path, &instr_list, &sizel);
    if (status < 0) {
        LOG_ERRT("failed to parse stimuli\n");
        status = TRDB_FAIL;
        goto fail;
    }
    if (sizel != sizea) {
        LOG_ERRT("list sizes don't match: %zu vs %zu\n", sizea, sizel);
        status = TRDB_FAIL;
        goto fail;
    }

    if (TAILQ_EMPTY(&instr_list)) {
        LOG_ERRT("list is empty even though we read data\n");
        status = TRDB_FAIL;
        goto fail;
    }

    size_t i = 0;
    struct tr_instr *instr;
    TAILQ_FOREACH (instr, &instr_list, list) {
        if (i >= sizea) {
            LOG_ERRT("trying to access out of bounds index\n");
            status = TRDB_FAIL;
            goto fail;
        }

        if (!trdb_compare_instr(c, instr, &(*samples)[i])) {
            LOG_ERRT("tr_instr are not equal\n");
            status = TRDB_FAIL;
            trdb_print_instr(stdout, instr);
            trdb_print_instr(stdout, &(*samples)[i]);
            goto fail;
        }
        i++;
    }

fail:
    trdb_free(c);
    trdb_free_instr_list(&instr_list);
    free(*samples);
    return status;
}

static int test_stimuli_to_packet_dump(const char *path)
{
    struct tr_instr *tmp      = NULL;
    struct tr_instr **samples = &tmp;
    int status                = TRDB_SUCCESS;
    struct trdb_ctx *c        = trdb_new();
    if (!c) {
        LOG_ERRT("Library context allocation failed.\n");
        status = TRDB_FAIL;
        goto fail;
    }

    size_t samplecnt = 0;
    status           = trdb_stimuli_to_trace(c, path, samples, &samplecnt);
    if (status != 0) {
        LOG_ERRT("Stimuli to tr_instr failed\n");
        status = TRDB_FAIL;
        goto fail;
    }
    status = TRDB_SUCCESS;

    struct trdb_packet_head head;
    TAILQ_INIT(&head);

    /* step by step compression */
    for (size_t i = 0; i < samplecnt; i++) {
        int step = trdb_compress_trace_step_add(c, &head, &(*samples)[i]);
        if (step < 0) {
            LOG_ERRT("Compress trace failed.\n");
            status = TRDB_FAIL;
            goto fail;
        }
    }

    if (TRDB_VERBOSE_TESTS)
        trdb_dump_packet_list(stdout, &head);

fail:
    trdb_free(c);
    free(*samples);
    trdb_free_packet_list(&head);
    return status;
}

static int test_disassemble_trace(const char *bin_path, const char *trace_path)
{
    struct trdb_ctx *c = trdb_new();
    struct tr_instr *tmp;
    struct tr_instr **samples = &tmp;
    size_t samplecnt          = 0;
    int status                = 0;
    status = trdb_stimuli_to_trace(c, trace_path, samples, &samplecnt);
    if (status < 0) {
        LOG_ERRT("Stimuli to tr_instr failed\n");
        return TRDB_FAIL;
    }

    status = TRDB_SUCCESS;

    bfd_init();
    bfd *abfd = bfd_openr(bin_path, NULL);

    if (!(abfd && bfd_check_format(abfd, bfd_object))) {
        bfd_perror("test_decompress_trace");
        status = TRDB_FAIL;
        goto fail;
    }

    struct disassembler_unit dunit = {0};
    struct disassemble_info dinfo  = {0};
    dunit.dinfo                    = &dinfo;
    trdb_init_disassembler_unit(&dunit, abfd, NULL);

    if (TRDB_VERBOSE_TESTS)
        trdb_disassemble_trace(samplecnt, *samples, &dunit);

fail:
    trdb_free(c);
    free(*samples);
    bfd_close(abfd);
    return status;
}

static int test_disassemble_trace_with_bfd(const char *bin_path,
                                           const char *trace_path)
{
    bfd *abfd = bfd_openr(bin_path, NULL);
    if (!(abfd && bfd_check_format(abfd, bfd_object)))
        return TRDB_FAIL;

    struct trdb_ctx *c = trdb_new();
    struct tr_instr *tmp;
    struct tr_instr **samples = &tmp;
    size_t samplecnt          = 0;
    int status                = 0;
    status = trdb_stimuli_to_trace(c, trace_path, samples, &samplecnt);
    if (status < 0) {
        LOG_ERRT("Stimuli to tr_instr failed\n");
        return TRDB_FAIL;
    }

    struct disassemble_info dinfo  = {0};
    struct disassembler_unit dunit = {0};

    dunit.dinfo = &dinfo;

    if (trdb_alloc_dinfo_with_bfd(c, abfd, &dunit)) {
        status = TRDB_FAIL;
        goto fail;
    }
    if (TRDB_VERBOSE_TESTS) {
        trdb_disassemble_trace(samplecnt, *samples, &dunit);
        trdb_set_disassembly_conf(&dunit, TRDB_LINE_NUMBERS | TRDB_SOURCE_CODE |
                                              TRDB_FUNCTION_CONTEXT);
        trdb_disassemble_trace_with_bfd(c, samplecnt, *samples, abfd, &dunit);
    }

fail:
    trdb_free_dinfo_with_bfd(c, abfd, &dunit);
    trdb_free(c);
    free(*samples);
    bfd_close(abfd);
    return status;
}

static int test_compress_trace(const char *trace_path, const char *packets_path)
{
    struct trdb_ctx *ctx = NULL;

    FILE *expected_packets = NULL;
    FILE *tmp_fp0          = NULL;
    FILE *tmp_fp1          = NULL;

    char *compare  = NULL;
    char *expected = NULL;

    struct tr_instr *tmp      = NULL;
    struct tr_instr **samples = &tmp;
    size_t samplecnt          = 0;
    int status                = TRDB_SUCCESS;

    snprintf(func_args_buf, sizeof(func_args_buf), "%s, %s", trace_path,
             packets_path);

    ctx = trdb_new();
    if (!ctx) {
        LOG_ERRT("Library context allocation failed.\n");
        status = TRDB_FAIL;
        goto fail;
    }

    status = trdb_stimuli_to_trace(ctx, trace_path, samples, &samplecnt);
    if (status < 0) {
        LOG_ERRT("Stimuli to tr_instr failed\n");
        return TRDB_FAIL;
    }
    status = TRDB_SUCCESS;

    struct trdb_packet_head packet1_head;
    TAILQ_INIT(&packet1_head);
    struct trdb_instr_head instr_head;
    TAILQ_INIT(&instr_head);

    /* step by step compression */
    for (size_t i = 0; i < samplecnt; i++) {
        int step =
            trdb_compress_trace_step_add(ctx, &packet1_head, &(*samples)[i]);
        if (step < 0) {
            LOG_ERRT("Compress trace failed.\n");
            status = TRDB_FAIL;
            goto fail;
        }
    }

    expected_packets = fopen(packets_path, "r");
    if (!expected_packets) {
        perror("fopen");
        status = TRDB_FAIL;
        goto fail;
    }

    tmp_fp0 = fopen("tmp2", "w+");
    if (!tmp_fp0) {
        perror("fopen");
        status = TRDB_FAIL;
        goto fail;
    }
    rewind(tmp_fp0);

    tmp_fp1 = fopen("tmp3", "w+");
    if (!tmp_fp1) {
        perror("fopen");
        status = TRDB_FAIL;
        goto fail;
    }
    trdb_dump_packet_list(tmp_fp1, &packet1_head);
    rewind(tmp_fp1);

    size_t linecnt = 0;
    size_t len     = 0;
    ssize_t nread_compare;
    ssize_t nread_expected;

    /* test stepwise compression against expected response */
    while ((nread_expected = getline(&expected, &len, expected_packets)) !=
           -1) {
        linecnt++;
        nread_compare = getline(&compare, &len, tmp_fp1);
        if (nread_compare == -1) {
            LOG_ERRT(
                "Hit EOF too early in expected packets file, new compression\n");
            status = TRDB_FAIL;
            goto fail;
        }
        if (nread_expected != nread_compare ||
            strncmp(compare, expected, nread_expected) != 0) {
            LOG_ERRT("Expected packets mismatch on line %zu\n", linecnt);
            LOG_ERRT("Expected: %s", expected);
            LOG_ERRT("Received: %s", compare);
            status = TRDB_FAIL;
            goto fail;
        }
    }

fail:
    free(compare);
    free(expected);
    trdb_free(ctx);
    if (tmp_fp0)
        fclose(tmp_fp0);
    if (tmp_fp1)
        fclose(tmp_fp1);
    if (expected_packets)
        fclose(expected_packets);
    remove("tmp2");
    remove("tmp3");
    free(*samples);
    trdb_free_packet_list(&packet1_head);
    trdb_free_instr_list(&instr_head);
    return status;
}

static int test_compress_cvs_trace(const char *trace_path)
{
    int status           = TRDB_SUCCESS;
    struct trdb_ctx *ctx = trdb_new();

    struct disassembler_unit dunit = {0};
    struct disassemble_info dinfo  = {0};
    dunit.dinfo                    = &dinfo;

    snprintf(func_args_buf, sizeof(func_args_buf), "%s", trace_path);

    trdb_init_disassembler_unit_for_pulp(&dunit, NULL);

    struct trdb_packet_head packet_list;
    TAILQ_INIT(&packet_list);
    struct trdb_instr_head instr_list;
    TAILQ_INIT(&instr_list);

    if (!ctx) {
        LOG_ERRT("Library context allocation failed.\n");
        status = TRDB_FAIL;
        goto fail;
    }

    ctx->dunit                           = &dunit;
    ctx->config.full_address             = false;
    ctx->config.pulp_vector_table_packet = false;
    ctx->config.implicit_ret             = true;
    /* ctx->config.compress_full_branch_map = true; */
    size_t instrcnt = 0;

    status = trdb_cvs_to_trace_list(ctx, trace_path, &instr_list, &instrcnt);
    if (status < 0) {
        LOG_ERRT("CVS to tr_instr failed\n");
        status = TRDB_FAIL;
        goto fail;
    }

    struct tr_instr *instr;
    TAILQ_FOREACH (instr, &instr_list, list) {
        /* trdb_disassemble_instr(instr, &dunit); */
    }

    TAILQ_FOREACH (instr, &instr_list, list) {
        int step = trdb_compress_trace_step_add(ctx, &packet_list, instr);
        if (step < 0) {
            LOG_ERRT("Compress trace failed\n");
            status = TRDB_FAIL;
            goto fail;
        }
    }

    if (TRDB_VERBOSE_TESTS) {
        printf("instructions: %zu, packets: %zu, payload bytes: %zu "
               "exceptions: %zu z/o: %zu\n",
               instrcnt, ctx->stats.packets, ctx->stats.payloadbits / 8,
               ctx->stats.exception_packets, ctx->stats.zo_addresses);
        double bpi_payload = ctx->stats.payloadbits / (double)ctx->stats.instrs;
        double bpi_full    = (ctx->stats.payloadbits + ctx->stats.packets * 6) /
                          (double)ctx->stats.instrs;
        double bpi_pulp = (ctx->stats.pulpbits / (double)ctx->stats.instrs);
        printf("(Compression) Bits per instruction (payload         ): %lf\n",
               bpi_payload);
        printf("(Compression) Bits per instruction (payload + header): %lf "
               "(%+2.lf%%)\n",
               bpi_full, bpi_full / bpi_payload * 100 - 100);
        printf("(Compression) Bits per instruction (pulp            ): %lf "
               "(%+2.lf%%)\n ",
               bpi_pulp, bpi_pulp / bpi_full * 100 - 100);
    }
fail:

    trdb_free_packet_list(&packet_list);
    trdb_free_instr_list(&instr_list);
    trdb_free(ctx);
    return status;
}

static int test_decompress_trace(const char *bin_path, const char *trace_path)
{
    bfd *abfd                 = NULL;
    struct tr_instr *tmp      = NULL;
    struct tr_instr **samples = &tmp;
    size_t samplecnt          = 0;
    int status                = TRDB_SUCCESS;

    snprintf(func_args_buf, sizeof(func_args_buf), "%s", trace_path);

    struct trdb_ctx *ctx = trdb_new();
    if (!ctx) {
        LOG_ERRT("Library context allocation failed.\n");
        status = TRDB_FAIL;
        goto fail;
    }

    bfd_init();
    abfd = bfd_openr(bin_path, NULL);

    if (!(abfd && bfd_check_format(abfd, bfd_object))) {
        bfd_perror("test_decompress_trace");
        return TRDB_FAIL;
    }

    status = trdb_stimuli_to_trace(ctx, trace_path, samples, &samplecnt);
    if (status < 0) {
        LOG_ERRT("Stimuli to tr_instr failed\n");
        status = TRDB_FAIL;
        goto fail;
    }

    ctx->config.full_address  = false;
    ctx->config.use_pulp_sext = true;
    ctx->config.implicit_ret  = false;

    struct trdb_packet_head packet1_head;
    TAILQ_INIT(&packet1_head);
    struct trdb_instr_head instr1_head;
    TAILQ_INIT(&instr1_head);

    /* step by step compression */
    for (size_t i = 0; i < samplecnt; i++) {
        int step =
            trdb_compress_trace_step_add(ctx, &packet1_head, &(*samples)[i]);
        if (step < 0) {
            LOG_ERRT("Compress trace failed.\n");
            status = TRDB_FAIL;
            goto fail;
        }
    }

    if (TRDB_VERBOSE_TESTS)
        printf("(Compression) Bits per instruction: %lf\n",
               ctx->stats.payloadbits / (double)ctx->stats.instrs);

    if (TRDB_VERBOSE_TESTS)
        trdb_dump_packet_list(stdout, &packet1_head);

    status = trdb_decompress_trace(ctx, abfd, &packet1_head, &instr1_head);
    if (status < 0) {
        LOG_ERRT("Decompression failed: %s\n",
                 trdb_errstr(trdb_errcode(status)));
        goto fail;
    }

    if (TRDB_VERBOSE_TESTS) {
        LOG_INFOT("Reconstructed trace disassembly:\n");
        struct tr_instr *instr;
        TAILQ_FOREACH (instr, &instr1_head, list) {
        }
    }

    /* We compare whether the reconstruction matches the original sequence, only
     * the pc for now.
     */
    struct tr_instr *instr;
    int processedcnt = 0;
    int i            = 0;
    TAILQ_FOREACH (instr, &instr1_head, list) {
        /* skip all invalid instructions for the comparison */
        while (!(*samples)[i].valid || (*samples)[i].exception) {
            i++;
        }

        if (instr->iaddr != (*samples)[i].iaddr) {
            LOG_ERRT("original instr: %" PRIxADDR "\n", (*samples)[i].iaddr);
            LOG_ERRT("reconst. instr: %" PRIxADDR "\n", instr->iaddr);
            status = TRDB_FAIL;
            goto fail;
        }
        i++;
        processedcnt++;
    }
    LOG_INFOT("Compared %d instructions\n", processedcnt);

    if (TAILQ_EMPTY(&instr1_head)) {
        LOG_ERRT("Empty instruction list.\n");
        return 0;
    }

fail:
    trdb_free(ctx);
    free(*samples);
    trdb_free_packet_list(&packet1_head);
    trdb_free_instr_list(&instr1_head);
    bfd_close(abfd);

    return status;
}

static int test_decompress_cvs_trace_differential(const char *bin_path,
                                                  const char *trace_path,
                                                  bool differential,
                                                  bool implicit_ret)
{
    bfd *abfd              = NULL;
    size_t instrcnt        = 0;
    int status             = TRDB_SUCCESS;
    struct tr_instr *instr = NULL;

    struct trdb_ctx *ctx = trdb_new();
    if (!ctx) {
        LOG_ERRT("Library context allocation failed.\n");
        status = TRDB_FAIL;
        goto fail;
    }
    struct trdb_packet_head packet_list;
    TAILQ_INIT(&packet_list);
    struct trdb_instr_head instr_list;
    TAILQ_INIT(&instr_list);
    struct trdb_instr_head instr1_head;
    TAILQ_INIT(&instr1_head);

    snprintf(func_args_buf, sizeof(func_args_buf),
             "%s, differential: %s, implicit returns: %s", bin_path,
             differential ? "true" : "false", implicit_ret ? "true" : "false");

    bfd_init();
    abfd = bfd_openr(bin_path, NULL);

    if (!(abfd && bfd_check_format(abfd, bfd_object))) {
        bfd_perror("test_decompress_trace");
        return TRDB_FAIL;
    }

    status = trdb_cvs_to_trace_list(ctx, trace_path, &instr_list, &instrcnt);
    if (status < 0) {
        LOG_ERRT("CVS to tr_instr failed\n");
        status = TRDB_FAIL;
        goto fail;
    }

    ctx->config.full_address  = !differential;
    ctx->config.use_pulp_sext = true;
    ctx->config.implicit_ret  = implicit_ret;

    TAILQ_FOREACH (instr, &instr_list, list) {
        int step = trdb_compress_trace_step_add(ctx, &packet_list, instr);
        if (step < 0) {
            LOG_ERRT("Compress trace failed\n");
            status = TRDB_FAIL;
            goto fail;
        }
    }

    if (TRDB_VERBOSE_TESTS) {
        printf("(Compression) Bits per instruction: %lf\n",
               ctx->stats.payloadbits / (double)ctx->stats.instrs);
        printf("(Compression) Sign extension distribution:\n");
        unsigned sum = 0;
        for (unsigned i = 0; i < 32; i++) {
            sum += ctx->stats.sext_bits[i];
        }
        for (unsigned i = 0; i < 32; i++) {
            printf("(Compression) Bit %2u: %10.5lf%%\n", (i + 1),
                   (ctx->stats.sext_bits[i] * 100 / (double)sum));
        }
    }
    if (TRDB_VERBOSE_TESTS)
        trdb_dump_packet_list(stdout, &packet_list);

    status = trdb_decompress_trace(ctx, abfd, &packet_list, &instr1_head);
    if (status < 0) {
        LOG_ERRT("Decompression failed: %s\n",
                 trdb_errstr(trdb_errcode(status)));
        status = TRDB_FAIL;
        goto fail;
    }

    if (TRDB_VERBOSE_TESTS) {
        LOG_INFOT("Reconstructed trace disassembly:\n");
        TAILQ_FOREACH (instr, &instr1_head, list) {
        }
    }

    /* We compare whether the reconstruction matches the original sequence, only
     * the pc for now.
     */
    int processedcnt        = 0;
    struct tr_instr *sample = TAILQ_FIRST(&instr_list);
    TAILQ_FOREACH (instr, &instr1_head, list) {
        /* skip all invalid instructions for the comparison */
        while (!sample->valid || sample->exception) {
            sample = TAILQ_NEXT(sample, list);
        }

        if (instr->iaddr != sample->iaddr) {
            LOG_ERRT("FAIL at instruction number: % d\n", processedcnt);
            LOG_ERRT("original instr: %" PRIxADDR "\n", sample->iaddr);
            LOG_ERRT("reconst. instr: %" PRIxADDR "\n", instr->iaddr);
            status = TRDB_FAIL;
            goto fail;
        }
        sample = TAILQ_NEXT(sample, list);
        processedcnt++;
    }
    LOG_INFOT("Compared %d instructions\n", processedcnt);

    if (TAILQ_EMPTY(&instr1_head)) {
        LOG_ERRT("Empty instruction list.\n");
        return 0;
    }

fail:
    trdb_free(ctx);
    trdb_free_packet_list(&packet_list);
    trdb_free_instr_list(&instr_list);
    trdb_free_instr_list(&instr1_head);
    bfd_close(abfd);

    return status;
}

static int test_decompress_trace_differential(const char *bin_path,
                                              const char *trace_path,
                                              bool differential,
                                              bool implicit_ret)
{
    bfd *abfd                 = NULL;
    struct tr_instr *tmp      = NULL;
    struct tr_instr **samples = &tmp;
    size_t samplecnt          = 0;
    int status                = TRDB_SUCCESS;

    struct trdb_ctx *ctx = trdb_new();
    if (!ctx) {
        LOG_ERRT("Library context allocation failed.\n");
        status = TRDB_FAIL;
        goto fail;
    }

    snprintf(func_args_buf, sizeof(func_args_buf),
             "%s, differential: %s, implicit returns: %s", trace_path,
             differential ? "true" : "false", implicit_ret ? "true" : "false");

    bfd_init();
    abfd = bfd_openr(bin_path, NULL);

    if (!(abfd && bfd_check_format(abfd, bfd_object))) {
        bfd_perror("test_decompress_trace");
        return TRDB_FAIL;
    }

    status = trdb_stimuli_to_trace(ctx, trace_path, samples, &samplecnt);
    if (status < 0) {
        LOG_ERRT("Stimuli to tr_instr failed\n");
        status = TRDB_FAIL;
        goto fail;
    }

    struct trdb_packet_head packet1_head;
    TAILQ_INIT(&packet1_head);
    struct trdb_instr_head instr1_head;
    TAILQ_INIT(&instr1_head);

    ctx->config.full_address  = !differential;
    ctx->config.use_pulp_sext = true;
    ctx->config.implicit_ret  = implicit_ret;

    /* step by step compression */
    for (size_t i = 0; i < samplecnt; i++) {
        int step =
            trdb_compress_trace_step_add(ctx, &packet1_head, &(*samples)[i]);
        if (step < 0) {
            LOG_ERRT("Compress trace failed.\n");
            status = TRDB_FAIL;
            goto fail;
        }
    }

    if (TRDB_VERBOSE_TESTS) {
        printf("(Compression) Bits per instruction: %lf\n",
               ctx->stats.payloadbits / (double)ctx->stats.instrs);
        printf("(Compression) Sign extension distribution:\n");
        unsigned sum = 0;
        for (unsigned i = 0; i < 32; i++) {
            sum += ctx->stats.sext_bits[i];
        }
        for (unsigned i = 0; i < 32; i++) {
            printf("(Compression) Bit %2u: %10.5lf%%\n", (i + 1),
                   (ctx->stats.sext_bits[i] * 100 / (double)sum));
        }
    }
    if (TRDB_VERBOSE_TESTS)
        trdb_dump_packet_list(stdout, &packet1_head);

    status = trdb_decompress_trace(ctx, abfd, &packet1_head, &instr1_head);
    if (status < 0) {
        LOG_ERRT("Decompression failed: %s\n",
                 trdb_errstr(trdb_errcode(status)));
        status = TRDB_FAIL;
        goto fail;
    }

    if (TRDB_VERBOSE_TESTS) {
        LOG_INFOT("Reconstructed trace disassembly:\n");
        struct tr_instr *instr;
        TAILQ_FOREACH (instr, &instr1_head, list) {
        }
    }

    /* We compare whether the reconstruction matches the original sequence, only
     * the pc for now.
     */
    struct tr_instr *instr;
    int processedcnt = 0;
    int i            = 0;
    TAILQ_FOREACH (instr, &instr1_head, list) {
        /* skip all invalid instructions for the comparison */
        while (!(*samples)[i].valid || (*samples)[i].exception) {
            i++;
        }

        if (instr->iaddr != (*samples)[i].iaddr) {
            LOG_ERRT("original instr: %" PRIxADDR "\n", (*samples)[i].iaddr);
            LOG_ERRT("reconst. instr: %" PRIxADDR "\n", instr->iaddr);
            status = TRDB_FAIL;
            goto fail;
        }
        i++;
        processedcnt++;
    }
    LOG_INFOT("Compared %d instructions\n", processedcnt);

    if (TAILQ_EMPTY(&instr1_head)) {
        LOG_ERRT("Empty instruction list.\n");
        return 0;
    }

fail:
    trdb_free(ctx);
    free(*samples);
    trdb_free_packet_list(&packet1_head);
    trdb_free_instr_list(&instr1_head);
    bfd_close(abfd);

    return status;
}

/* make any directory in path if it doesn't exist*/
static void mkdir_p(char *path)
{
    struct stat stat_buf = {0};
    char *str;
    char *s;

    s = strdup(path);
    if (!s) {
        perror("strdup");
        exit(EXIT_FAILURE);
    }

    while ((str = strtok(s, "/")) != NULL) {
        if (str != s) {
            str[-1] = '/';
        }
        if (stat(path, &stat_buf) == -1) {
            if (mkdir(path, 0777) == -1) {
                perror("mkdir");
                exit(EXIT_FAILURE);
            }
        } else {
            if (!S_ISDIR(stat_buf.st_mode)) {
                LOG_ERRT("could not create directory %s\n", path);
                exit(EXIT_FAILURE);
            }
        }
        s = NULL;
    }

    if (s)
        free(s);
}

static bool is_valid_name(char *name)
{
    return !(*name == '.' || *name == '/');
}

/* record skipped tests to log and trs */
static void record_skipped(const char *restrict format, ...)
{
    va_list ap = {0};

    va_start(ap, format);
    printf("SKIP: ");
    vprintf(format, ap);
    va_end(ap);

    if (trs_fp) {
        va_start(ap, format);
        fprintf(trs_fp, ":test-result: SKIP ");
        vfprintf(trs_fp, format, ap);
        va_end(ap);
    }
}

#define TESTS_NUM_ARGS 1

const char *argp_program_version     = "tests " PACKAGE_VERSION;
const char *argp_program_bug_address = PACKAGE_BUGREPORT;
static char doc[]                    = "tests -- Test driver for trdb tests";
static char args_doc[]               = "TEST-NAME";

static struct argp_option options[] = {
    {"test-name", 'n', "NAME", 0, "The name of the test"},
    {"log-file", 'l', "PATH", 0, "Log of stdout of the test"},
    {"trs-file", 't', "PATH", 0, "Formatted result of test"},
    {"color-tests", 'c', "{yes|no}", 0, "Colorize test console output"},
    {"expect-failure", 'f', "{yes|no}", 0, "Whether test is expected to fail"},
    {"enable-hard-errors", 'e', "{yes|no}", 0,
     "Whether hard errors should be handled differently than normal errors"},
    {"verbose", 'v', 0, 0, "Produce verbose output"},
    {"quiet", 'q', 0, 0, "Don't produce any output"},
    {0}};

struct arguments {
    char *args[TESTS_NUM_ARGS];
    char *test_name;
    char *logfile;
    char *trsfile;
    bool silent, verbose;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct arguments *arguments = state->input;
    switch (key) {
    case 'q':
        arguments->silent = true;
        break;
    case 'v':
        arguments->verbose = true;
        break;
    case 'n':
        arguments->test_name = arg;
        break;
    case 'l':
        arguments->logfile = arg;
        break;
    case 'c':
        break;
    case 'f':
        break;
    case 'e':
        break;
    case 't':
        arguments->trsfile = arg;
        break;
    case ARGP_KEY_ARG:
        if (state->arg_num >= TESTS_NUM_ARGS)
            argp_usage(state);
        arguments->args[state->arg_num] = arg;
        break;
    case ARGP_KEY_END:
        /* We allow passing no positional argument. This will just run the basic
         * tests */
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc};

int main(int argc, char *argv[argc + 1])
{
    const char *tv[] = {
        "data/interrupt",
        "data/trdb_stimuli",
        "data/trdb_stimuli_valid_only_bin",
        "data/trdb_stimuli_valid_only",
        "data/trdb_stimuli_all_bin",
        "data/trdb_stimuli_all",
        "data/hello/build/pulpissimo-riscy/test/test",
        "data/hello/build/pulpissimo-riscy/trdb_stimuli",
        "data/enqueue_delayed/build/pulpissimo-riscy/test/test",
        "data/enqueue_delayed/build/pulpissimo-riscy/trdb_stimuli",
        /* "data/timer_oneshot/build/pulpissimo-riscy/test/test", */
        /* "data/timer_oneshot/build/pulpissimo-riscy/trdb_stimuli", */
        "data/wait_time/build/pulpissimo-riscy/test/test",
        "data/wait_time/build/pulpissimo-riscy/trdb_stimuli",
        "data/uart_send/build/pulpissimo-riscy/test/test",
        "data/uart_send/build/pulpissimo-riscy/trdb_stimuli",
        "data/uart_loopback/build/pulpissimo-riscy/test/test",
        "data/uart_loopback/build/pulpissimo-riscy/trdb_stimuli",
        "data/coremark/build/pulpissimo-riscy/test/test",
        "data/coremark/build/pulpissimo-riscy/trdb_stimuli",
        "data/median/build/pulpissimo-riscy/median/median",
        "data/median/build/pulpissimo-riscy/trdb_stimuli",

    };

    const char *tv_cvs[] = {
        "data/cvs/dhrystone.spike_trace", "data/cvs/median.spike_trace",
        "data/cvs/mm.spike_trace",        "data/cvs/mt-matmul.spike_trace",
        "data/cvs/mt-vvadd.spike_trace",  "data/cvs/multiply.spike_trace",
        "data/cvs/pmp.spike_trace",       "data/cvs/qsort.spike_trace",
        "data/cvs/rsort.spike_trace",     "data/cvs/spmv.spike_trace",
        "data/cvs/towers.spike_trace",    "data/cvs/vvadd.spike_trace"};

    const char *tv_gen_cvs[] = {"riscv-traces-32/dhrystone.riscv",
                                "riscv-traces-32/dhrystone.riscv.cvs",
                                "riscv-traces-32/median.riscv",
                                "riscv-traces-32/median.riscv.cvs",
                                "riscv-traces-32/mm.riscv",
                                "riscv-traces-32/mm.riscv.cvs",
                                "riscv-traces-32/mt-matmul.riscv",
                                "riscv-traces-32/mt-matmul.riscv.cvs",
                                "riscv-traces-32/mt-vvadd.riscv",
                                "riscv-traces-32/mt-vvadd.riscv.cvs",
                                "riscv-traces-32/multiply.riscv",
                                "riscv-traces-32/multiply.riscv.cvs",
                                "riscv-traces-32/pmp.riscv",
                                "riscv-traces-32/pmp.riscv.cvs",
                                "riscv-traces-32/qsort.riscv",
                                "riscv-traces-32/qsort.riscv.cvs",
                                "riscv-traces-32/rsort.riscv",
                                "riscv-traces-32/rsort.riscv.cvs",
                                "riscv-traces-32/spmv.riscv",
                                "riscv-traces-32/spmv.riscv.cvs",
                                "riscv-traces-32/towers.riscv",
                                "riscv-traces-32/towers.riscv.cvs",
                                "riscv-traces-32/vvadd.riscv",
                                "riscv-traces-32/vvadd.riscv.cvs"};
#ifdef TRDB_ARCH64
    const char *tv_gen_cvs_64[] = {"riscv-traces-64/dhrystone.riscv",
                                   "riscv-traces-64/dhrystone.riscv.cvs",
                                   "riscv-traces-64/median.riscv",
                                   "riscv-traces-64/median.riscv.cvs",
                                   "riscv-traces-64/mm.riscv",
                                   "riscv-traces-64/mm.riscv.cvs",
                                   "riscv-traces-64/mt-matmul.riscv",
                                   "riscv-traces-64/mt-matmul.riscv.cvs",
                                   "riscv-traces-64/mt-vvadd.riscv",
                                   "riscv-traces-64/mt-vvadd.riscv.cvs",
                                   "riscv-traces-64/multiply.riscv",
                                   "riscv-traces-64/multiply.riscv.cvs",
                                   "riscv-traces-64/pmp.riscv",
                                   "riscv-traces-64/pmp.riscv.cvs",
                                   "riscv-traces-64/qsort.riscv",
                                   "riscv-traces-64/qsort.riscv.cvs",
                                   "riscv-traces-64/rsort.riscv",
                                   "riscv-traces-64/rsort.riscv.cvs",
                                   "riscv-traces-64/spmv.riscv",
                                   "riscv-traces-64/spmv.riscv.cvs",
                                   "riscv-traces-64/towers.riscv",
                                   "riscv-traces-64/towers.riscv.cvs",
                                   "riscv-traces-64/vvadd.riscv",
                                   "riscv-traces-64/vvadd.riscv.cvs"};
#endif

    struct arguments arguments = {0};
    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    /* create directory structure to logfile */
    if (arguments.logfile) {
        char *dname = dirname(arguments.logfile);
        if (is_valid_name(dname))
            mkdir_p(dname);

        /* redirect stdout to logfile */
        char *bname = basename(arguments.logfile);
        if (is_valid_name(bname)) {
            /* spawn tee */
            char teecmd[PATH_MAX];

            snprintf(teecmd, sizeof(teecmd), "tee %s", arguments.logfile);

            if ((tee_fp = popen(teecmd, "w")) == NULL) {
                perror("popen");
                exit(EXIT_FAILURE);
            }

            /* redirect stdout to pipe */
            if (dup2(fileno(tee_fp), fileno(stdout)) == -1) {
                perror("dup2");
                exit(EXIT_FAILURE);
            }

            /* enable line buffering so that we can tail logs efficienctly as
             * for redirect output buffers are not flushed on a newline */
            setlinebuf(tee_fp);
            setlinebuf(stdout);
        }
    }

    /* create directory structure to trsfile */
    if (arguments.trsfile) {
        char *dname = dirname(arguments.trsfile);
        if (is_valid_name(dname))
            mkdir_p(dname);

        /* prepare trsfile */
        char *bname = basename(arguments.trsfile);
        if (is_valid_name(bname)) {
            if ((trs_fp = fopen(bname, "w+")) == NULL) {
                perror("fopen");
                exit(EXIT_FAILURE);
            }
            /* enable line buffering so that we can tail logs efficienctly as
             * for redirect output buffers are not flushed on a newline */
            setlinebuf(trs_fp);
        }
    }

    /* const char *tv_cvs[] = {"data/cvs/pmp.spike_trace"}; */
    INIT_TESTS();
    /* for (size_t i = 0; i < 8; i++) */
    /*     RUN_TEST(test_trdb_serialize_packet, i); */

    RUN_TEST(test_disasm_bfd);
    RUN_TEST(test_parse_stimuli_line);

    RUN_TEST(test_parse_packets, "data/tx_spi");
    RUN_TEST(test_trdb_dinfo_init, "data/interrupt");

    RUN_TEST(test_stimuli_to_tr_instr, "data/trdb_stimuli");
    RUN_TEST(test_stimuli_to_trace_list, "data/trdb_stimuli");
    RUN_TEST(test_stimuli_to_packet_dump, "data/trdb_stimuli");
    /* NOTE: there is a memory leak ~230 bytes in riscv-dis.c with struct
     * riscv_subset for each instantiation of a disassembler.
     */
    /* RUN_TEST(test_disassemble_trace, "data/interrupt",
    "data/trdb_stimuli");
     */
    RUN_TEST(test_disassemble_trace_with_bfd, "data/interrupt",
             "data/trdb_stimuli");

    RUN_TEST(test_compress_trace, "data/trdb_stimuli", "data/trdb_packets");

    for (unsigned j = 0; j < TRDB_ARRAY_SIZE(tv_cvs); j++) {
        const char *stim = tv_cvs[j];
        if (access(stim, R_OK)) {
            record_skipped("test_compress_cvs_trace(%s)\n", stim);
            continue;
        }
        RUN_TEST(test_compress_cvs_trace, stim);
    }

    if (TRDB_ARRAY_SIZE(tv) % 2 != 0)
        LOG_ERRT("Test vector strings are incomplete.");

        /* not supported for 64-bit simply because of c.jal incompatibility */
#ifndef TRDB_ARCH64
    for (unsigned j = 0; j < TRDB_ARRAY_SIZE(tv); j += 2) {
        const char *bin  = tv[j];
        const char *stim = tv[j + 1];
        if (access(bin, R_OK) || access(stim, R_OK)) {
            record_skipped("test_compress_trace(%s)\n", bin);
            record_skipped(
                "test_compress_trace_differential(%s, true, false)\n", bin);
            record_skipped("test_compress_trace_differential(%s, true, true)\n",
                           bin);
            continue;
        }
        RUN_TEST(test_decompress_trace, bin, stim);
        RUN_TEST(test_decompress_trace_differential, bin, stim, true, false);
        RUN_TEST(test_decompress_trace_differential, bin, stim, true, true);
    }

#endif

#ifdef TRDB_ARCH64
    for (unsigned j = 0; j < TRDB_ARRAY_SIZE(tv_gen_cvs_64); j += 2) {
        const char *bin  = tv_gen_cvs_64[j];
        const char *stim = tv_gen_cvs_64[j + 1];
#else
    for (unsigned j = 0; j < TRDB_ARRAY_SIZE(tv_gen_cvs); j += 2) {
        const char *bin  = tv_gen_cvs[j];
        const char *stim = tv_gen_cvs[j + 1];
#endif
        if (access(bin, R_OK) || access(stim, R_OK)) {
            record_skipped(
                "test_decompress_cvs_trace_differential(%s, %s, true, false)\n",
                bin, stim);
            record_skipped(
                "test_decompress_cvs_trace_differential(%s, %s, true, true)\n",
                bin, stim);
            continue;
        }
        RUN_TEST(test_decompress_cvs_trace_differential, bin, stim, true,
                 false);
        RUN_TEST(test_decompress_cvs_trace_differential, bin, stim, true, true);
    }

    if (TESTS_SUCCESSFULL()) {
        printf("ALL TESTS PASSED\n");
        if (trs_fp)
            fprintf(trs_fp, ":test-global-result: PASS\n");
    } else {
        printf("AT LEAST ONE TEST FAILED\n");
        if (trs_fp)
            fprintf(trs_fp, ":test-global-result: FAIL\n");
    }

    if (trs_fp)
        fclose(trs_fp);

    fflush(tee_fp);
    /* we don't try to pclose(tee_fp) as tee just waits for more data from
     * stdin resulting in getting blocked forever */

    return TESTS_SUCCESSFULL() ? EXIT_SUCCESS : EXIT_FAILURE;
}

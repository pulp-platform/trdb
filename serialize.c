/*
 * trdb - Debugger Software for the PULP platform
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

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/queue.h>
#include "serialize.h"
#include "trace_debugger.h"
#include "trdb_private.h"
#include "utils.h"

/* pulp specific packet serialization */
int trdb_pulp_serialize_packet(struct trdb_ctx *c, struct tr_packet *packet,
                               size_t *bitcnt, uint8_t align, uint8_t bin[])
{
    if (align >= 8) {
        err(c, "bad alignment value: %" PRId8 "\n", align);
        return -trdb_invalid;
    }

    union trdb_pack data = {0};
    /* We put the number of bytes (without header) as the packet length. The
     * PULPPKTLEN, MSGTYPELEN and FORMATLEN are considered the header
     */
    uint32_t bits_without_header = packet->length - FORMATLEN;
    uint32_t byte_len =
        bits_without_header / 8 + (bits_without_header % 8 != 0);
    if (byte_len >= 16) { // TODO: replace with pow or mask
        err(c, "bad packet length\n");
        return -trdb_bad_packet;
    }

    switch (packet->format) {
    case F_BRANCH_FULL: {
        uint32_t len = branch_map_len(packet->branches);

        /* we need enough space to do the packing it in uint128 */
        assert(128 > PULPPKTLEN + FORMATLEN + MSGTYPELEN + 5 + 31 + XLEN);
        /* TODO: assert branch map to overfull */
        data.bits = byte_len | (packet->msg_type << PULPPKTLEN) |
                    (packet->format << (PULPPKTLEN + MSGTYPELEN)) |
                    (packet->branches << (PULPPKTLEN + MSGTYPELEN + FORMATLEN));
        data.bits |= ((__uint128_t)packet->branch_map & MASK_FROM(len))
                     << (PULPPKTLEN + MSGTYPELEN + FORMATLEN + BRANCHLEN);

        *bitcnt = PULPPKTLEN + MSGTYPELEN + FORMATLEN + BRANCHLEN + len;

        /* if we have a full branch we don't necessarily need to emit address */
        if (packet->branches > 0) {
            data.bits |=
                ((__uint128_t)packet->address
                 << (PULPPKTLEN + MSGTYPELEN + FORMATLEN + BRANCHLEN + len));
            if (trdb_is_full_address(c))
                *bitcnt += XLEN;
            else
                *bitcnt +=
                    (XLEN - trdb_sign_extendable_bits(packet->address) + 1);
        } else {
            /* no address, but compress full branch map*/
            if (trdb_get_compress_branch_map(c)) {
                *bitcnt -= len;
                uint32_t sext =
                    trdb_sign_extendable_bits(packet->branch_map << 1);
                if (sext > 31)
                    sext = 31;
                *bitcnt += (31 - sext + 1);
            }
        }
        data.bits <<= align;
        memcpy(bin, data.bin,
               (*bitcnt + align) / 8 + ((*bitcnt + align) % 8 != 0));

        return 0;
    }

    case F_BRANCH_DIFF: {
        if (trdb_is_full_address(c)) {
            err(c, "F_BRANCH_DIFF packet encountered but full_address set\n");
            return -trdb_bad_config;
        }
        uint32_t len = branch_map_len(packet->branches);

        /* we need enough space to do the packing it in uint128 */
        assert(128 > PULPPKTLEN + FORMATLEN + MSGTYPELEN + 5 + 31 + XLEN);
        /* TODO: assert branch map to overfull */
        data.bits = byte_len | (packet->msg_type << PULPPKTLEN) |
                    (packet->format << (PULPPKTLEN + MSGTYPELEN)) |
                    (packet->branches << (PULPPKTLEN + MSGTYPELEN + FORMATLEN));
        data.bits |= ((__uint128_t)packet->branch_map & MASK_FROM(len))
                     << (PULPPKTLEN + MSGTYPELEN + FORMATLEN + BRANCHLEN);

        *bitcnt = PULPPKTLEN + MSGTYPELEN + FORMATLEN + BRANCHLEN + len;

        /* if we have a full branch we don't necessarily need to emit address */
        if (packet->branches > 0) {
            data.bits |=
                ((__uint128_t)packet->address
                 << (PULPPKTLEN + MSGTYPELEN + FORMATLEN + BRANCHLEN + len));
            *bitcnt += (XLEN - trdb_sign_extendable_bits(packet->address) + 1);
        } else {
            /* no address, but compress full branch map*/
            if (trdb_get_compress_branch_map(c)) {
                *bitcnt -= len;
                uint32_t sext =
                    trdb_sign_extendable_bits(packet->branch_map << 1);
                if (sext > 31)
                    sext = 31;
                *bitcnt += (31 - sext + 1);
            }
        }

        data.bits <<= align;
        memcpy(bin, data.bin,
               (*bitcnt + align) / 8 + ((*bitcnt + align) % 8 != 0));

        return 0;
    }
    case F_ADDR_ONLY:
        assert(128 > PULPPKTLEN + MSGTYPELEN + FORMATLEN + XLEN);
        data.bits = byte_len | (packet->msg_type << PULPPKTLEN) |
                    (packet->format << (PULPPKTLEN + MSGTYPELEN)) |
                    ((__uint128_t)packet->address
                     << (PULPPKTLEN + MSGTYPELEN + FORMATLEN));

        *bitcnt = PULPPKTLEN + MSGTYPELEN + FORMATLEN;
        if (trdb_is_full_address(c))
            *bitcnt += XLEN;
        else
            *bitcnt += (XLEN - sign_extendable_bits(packet->address) + 1);

        data.bits <<= align;
        /* this cuts off superfluous bits */
        memcpy(bin, data.bin,
               (*bitcnt + align) / 8 + ((*bitcnt + align) % 8 != 0));
        return 0;

    case F_SYNC:
        assert(PRIVLEN == 3);
        /* check for enough space to the packing */
        assert(128 > PULPPKTLEN + 4 + PRIVLEN + 1 + XLEN + CAUSELEN + 1);
        /* TODO: for now we ignore the context field since we have
         * only one hart
         */

        /* common part to all sub formats */
        data.bits =
            byte_len | (packet->msg_type << PULPPKTLEN) |
            (packet->format << (PULPPKTLEN + MSGTYPELEN)) |
            (packet->subformat << (PULPPKTLEN + MSGTYPELEN + FORMATLEN)) |
            (packet->privilege << (PULPPKTLEN + MSGTYPELEN + 2 * FORMATLEN));
        *bitcnt = PULPPKTLEN + MSGTYPELEN + 2 * FORMATLEN + PRIVLEN;

        /* to reduce repetition */
        uint32_t suboffset = PULPPKTLEN + MSGTYPELEN + 2 * FORMATLEN + PRIVLEN;
        switch (packet->subformat) {
        case SF_START:
            data.bits |= ((__uint128_t)packet->branch << suboffset) |
                         ((__uint128_t)packet->address << (suboffset + 1));
            *bitcnt += 1 + XLEN;
            break;

        case SF_EXCEPTION:
            data.bits |=
                (packet->branch << suboffset) |
                ((__uint128_t)packet->address << (suboffset + 1)) |
                ((__uint128_t)packet->ecause << (suboffset + 1 + XLEN)) |
                ((__uint128_t)packet->interrupt
                 << (suboffset + 1 + XLEN + CAUSELEN));
            // going to be zero anyway in our case
            //  | ((__uint128_t)packet->tval
            //   << (PULPPKTLEN + 4 + PRIVLEN + 1 + XLEN + CAUSELEN + 1));
            *bitcnt += (1 + XLEN + CAUSELEN + 1);
            break;

        case SF_CONTEXT:
            /* TODO: we still ignore the context field */
            break;
        }

        data.bits <<= align;
        memcpy(bin, data.bin,
               (*bitcnt + align) / 8 + ((*bitcnt + align) % 8 != 0));
        return 0;
    }
    return -trdb_bad_packet;
}

int trdb_pulp_read_single_packet(struct trdb_ctx *c, FILE *fp,
                                 struct tr_packet *packet, uint32_t *bytes)
{
    uint8_t header          = 0;
    union trdb_pack payload = {0};
    if (!c || !fp || !packet)
        return -trdb_invalid;

    if (fread(&header, 1, 1, fp) != 1) {
        if (feof(fp))
            return -trdb_bad_packet;
        else if (ferror(fp))
            return -trdb_file_read;

        return -trdb_internal; /* does not happen */
    }

    /* read packet length it bits (including header) */
    uint8_t len    = (header & MASK_FROM(PULPPKTLEN)) * 8 + 8;
    payload.bin[0] = header;
    /* compute how many bytes that is */
    uint32_t byte_len = len / 8 + (len % 8 != 0 ? 1 : 0);
    /* we have to exclude the header byte */
    if (fread((payload.bin + 1), 1, byte_len - 1, fp) != byte_len - 1) {
        if (feof(fp)) {
            err(c, "incomplete packet read\n");
            return -trdb_bad_packet;
        } else if (ferror(fp))
            return -trdb_file_read;

        return -trdb_internal; /* does not happen */
    }
    /* since we succefully read a packet we can now set bytes */
    *bytes = byte_len;
    /* make sure we start from a good state */
    *packet = (struct tr_packet){0};

    /* approxmation in multiple of 8*/
    packet->length =
        (header & MASK_FROM(PULPPKTLEN)) * 8 + MSGTYPELEN + FORMATLEN;
    packet->msg_type = (payload.bits >>= PULPPKTLEN) & MASK_FROM(MSGTYPELEN);

    /* implicit sign extension at packet length' bit */
    payload.bits = sext128(payload.bits, packet->length);

    switch (packet->msg_type) {
    /* we are dealing with a regular trace packet */
    case W_TRACE:
        packet->format = (payload.bits >>= MSGTYPELEN) & MASK_FROM(FORMATLEN);
        payload.bits >>= FORMATLEN;

        uint32_t blen           = 0;
        uint32_t lower_boundary = 0;

        switch (packet->format) {
        case F_BRANCH_FULL:
            packet->branches   = payload.bits & MASK_FROM(BRANCHLEN);
            blen               = branch_map_len(packet->branches);
            packet->branch_map = (payload.bits >>= BRANCHLEN) & MASK_FROM(blen);

            lower_boundary = MSGTYPELEN + FORMATLEN + BRANCHLEN + blen;

            if (trdb_is_full_address(c)) {
                packet->address = (payload.bits >>= blen) & MASK_FROM(XLEN);
            } else {
                packet->address = (payload.bits >>= blen) & MASK_FROM(XLEN);
                packet->address =
                    sext32(packet->address, packet->length - lower_boundary);
            }
            return 0;
        case F_BRANCH_DIFF:
            if (trdb_is_full_address(c)) {
                err(c,
                    "F_BRANCH_DIFF packet encountered but full_address set\n");
                return -trdb_bad_config;
            }
            packet->branches   = payload.bits & MASK_FROM(BRANCHLEN);
            blen               = branch_map_len(packet->branches);
            packet->branch_map = (payload.bits >>= BRANCHLEN) & MASK_FROM(blen);
            lower_boundary     = MSGTYPELEN + FORMATLEN + BRANCHLEN + blen;

            if (trdb_is_full_address(c)) {
                packet->address = (payload.bits >>= blen) & MASK_FROM(XLEN);
            } else {
                packet->address = (payload.bits >>= blen) & MASK_FROM(XLEN);
                packet->address =
                    sext32(packet->address, packet->length - lower_boundary);
            }

            return 0;
        case F_ADDR_ONLY:
            if (trdb_is_full_address(c)) {
                packet->address = payload.bits & MASK_FROM(XLEN);
            } else {
                packet->address = sext32(
                    payload.bits, packet->length - MSGTYPELEN - FORMATLEN);
            }
            return 0;
        case F_SYNC:
            packet->subformat = payload.bits & MASK_FROM(FORMATLEN);
            packet->privilege =
                (payload.bits >>= FORMATLEN) & MASK_FROM(PRIVLEN);
            if (packet->subformat == SF_CONTEXT)
                return -trdb_unimplemented;

            packet->branch  = (payload.bits >>= PRIVLEN) & 1;
            packet->address = (payload.bits >>= 1) & MASK_FROM(XLEN);
            if (packet->subformat == SF_START)
                return 0;
            packet->ecause    = (payload.bits >>= XLEN) & MASK_FROM(CAUSELEN);
            packet->interrupt = (payload.bits >>= CAUSELEN) & 1;
            if (packet->subformat == SF_EXCEPTION)
                return 0;
        }
        break;
    /* this is user defined payload, written through the APB */
    case W_SOFTWARE:
        packet->userdata = (payload.bits >> MSGTYPELEN) & MASK_FROM(XLEN);
        return 0;
    /* timer data */
    case W_TIMER:
        /*careful if TIMELEN=64*/
        packet->time = (payload.bits >> MSGTYPELEN) & MASK_FROM(TIMELEN);
        return 0;
    default:
        err(c, "unknown message type in packet\n");
        return -trdb_bad_packet;
        break;
    }
    return -trdb_bad_packet;
}

int trdb_pulp_read_all_packets(struct trdb_ctx *c, const char *path,
                               struct trdb_packet_head *packet_list)
{
    if (!c || !path || !packet_list)
        return -trdb_invalid;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -trdb_file_open;

    uint32_t total_bytes_read = 0;
    struct tr_packet *packet  = NULL;
    struct tr_packet tmp      = {0};
    uint32_t bytes            = 0;

    /* read the file and malloc entries into the given linked list head */
    while (trdb_pulp_read_single_packet(c, fp, &tmp, &bytes) == 0) {
        packet = malloc(sizeof(*packet));
        if (!packet)
            return -trdb_nomem;
        *packet = tmp;
        total_bytes_read += bytes;

        TAILQ_INSERT_TAIL(packet_list, packet, list);
    }
    dbg(c, "total bytes read: %" PRIu32 "\n", total_bytes_read);
    return 0;
}

int trdb_pulp_write_single_packet(struct trdb_ctx *c, struct tr_packet *packet,
                                  FILE *fp)
{
    int status      = 0;
    size_t bitcnt   = 0;
    size_t bytecnt  = 0;
    uint8_t bin[16] = {0};
    if (!c || !fp || !packet)
        return -trdb_invalid;

    status = trdb_pulp_serialize_packet(c, packet, &bitcnt, 0, bin);
    if (status < 0)
        return status;

    bytecnt = bitcnt / 8 + (bitcnt % 8 != 0);
    if (fwrite(bin, 1, bytecnt, fp) != bytecnt) {
        if (feof(fp)) {
            /* TODO: uhhh */
        } else if (ferror(fp))
            return -trdb_file_write;
    }
    return 0;
}

int trdb_write_packets(struct trdb_ctx *c, const char *path,
                       struct trdb_packet_head *packet_list)
{
    int status = 0;
    if (!c || !path || !packet_list)
        return -trdb_invalid;

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        status = -trdb_file_open;
        goto fail;
    }

    uint8_t bin[sizeof(struct tr_packet)] = {0};
    size_t bitcnt                         = 0;
    uint32_t alignment                    = 0;
    uint8_t carry                         = 0;
    size_t good                           = 0;
    size_t rest                           = 0;

    struct tr_packet *packet;
    /* TODO: do we need the rever version? I think we do*/
    TAILQ_FOREACH_REVERSE (packet, packet_list, trdb_packet_head, list) {
        status = trdb_pulp_serialize_packet(c, packet, &bitcnt, alignment, bin);
        if (status < 0) {
            goto fail;
        }
        /* stitch two consecutive packets together */
        bin[0] |= carry;
        rest = (bitcnt + alignment) % 8;
        good = (bitcnt + alignment) / 8;

        /* write as many bytes as we can i.e. withouth the potentially
         * intersecting ones
         */
        if (fwrite(bin, 1, good, fp) != good) {
            status = -trdb_file_write;
            goto fail;
        }
        /* we keep that for the next packet */
        carry     = bin[good] & MASK_FROM(rest);
        alignment = rest;
    }
    /* done, write remaining carry */
    if (!fwrite(&bin[good], 1, 1, fp)) {
        status = -trdb_file_write;
    }
fail:
    if (fp)
        fclose(fp);
    return status;
}

int trdb_stimuli_to_trace_list(struct trdb_ctx *c, const char *path,
                               struct trdb_instr_head *instrs, size_t *count)
{
    int status = 0;
    FILE *fp   = NULL;

    *count = 0;

    if (!c || !path || !instrs) {
        status = -trdb_invalid;
        goto fail;
    }
    struct tr_instr *sample = NULL;

    fp = fopen(path, "r");
    if (!fp) {
        status = -trdb_file_open;
        goto fail;
    }
    size_t scnt = 0;

    int ret        = 0;
    int valid      = 0;
    int exception  = 0;
    int interrupt  = 0;
    uint32_t cause = 0;
    addr_t tval    = 0;
    uint32_t priv  = 0;
    addr_t iaddr   = 0;
    insn_t instr   = 0;
    int compressed = 0;

    while ((ret = fscanf(fp,
                         "valid= %d exception= %d interrupt= %d cause= %" SCNx32
                         " tval= %" SCNxADDR " priv= %" SCNx32
                         " compressed= %d addr= %" SCNxADDR " instr= %" SCNxINSN
                         " \n",
                         &valid, &exception, &interrupt, &cause, &tval, &priv,
                         &compressed, &iaddr, &instr)) != EOF) {
        // TODO: make this configurable so that we don't have to store so
        // much data
        /* if (!valid) { */
        /*     continue; */
        /* } */
        sample = malloc(sizeof(*sample));
        if (!sample) {
            status = -trdb_nomem;
            goto fail;
        }

        *sample            = (struct tr_instr){0};
        sample->valid      = valid;
        sample->exception  = exception;
        sample->interrupt  = interrupt;
        sample->cause      = cause;
        sample->tval       = tval;
        sample->priv       = priv;
        sample->iaddr      = iaddr;
        sample->instr      = instr;
        sample->compressed = compressed;

        TAILQ_INSERT_TAIL(instrs, sample, list);

        scnt++;
    }

    if (ferror(fp)) {
        status = -trdb_scan_file;
        goto fail;
    }

    *count = scnt;
out:
    if (fp)
        fclose(fp);

    return status;
fail:
    // TODO: it's maybe better to not free the whole list, but just the part
    // where failed
    trdb_free_instr_list(instrs);
    goto out;
}

int trdb_stimuli_to_trace(struct trdb_ctx *c, const char *path,
                          struct tr_instr **samples, size_t *count)
{
    int status = 0;
    FILE *fp   = NULL;

    *count = 0;

    if (!c || !path || !samples)
        return -trdb_invalid;

    fp = fopen(path, "r");
    if (!fp) {
        status = -trdb_file_open;
        goto fail;
    }
    size_t scnt = 0;

    int ret        = 0;
    int valid      = 0;
    int exception  = 0;
    int interrupt  = 0;
    uint32_t cause = 0;
    addr_t tval    = 0;
    uint32_t priv  = 0;
    addr_t iaddr   = 0;
    insn_t instr   = 0;
    int compressed = 0;

    size_t size = 128;
    *samples    = malloc(size * sizeof(**samples));
    if (!*samples) {
        status = -trdb_nomem;
        goto fail;
    }

    while ((ret = fscanf(fp,
                         "valid= %d exception= %d interrupt= %d cause= %" SCNx32
                         " tval= %" SCNxADDR " priv= %" SCNx32
                         " compressed= %d addr= %" SCNxADDR " instr= %" SCNxINSN
                         " \n",
                         &valid, &exception, &interrupt, &cause, &tval, &priv,
                         &compressed, &iaddr, &instr)) != EOF) {
        // TODO: make this configurable so that we don't have to store so much
        // data
        /* if (!valid) { */
        /*     continue; */
        /* } */
        if (scnt >= size) {
            size                 = 2 * size;
            struct tr_instr *tmp = realloc(*samples, size * sizeof(**samples));
            if (!tmp) {
                status = -trdb_nomem;
                goto fail;
            }
            *samples = tmp;
        }
        (*samples)[scnt]            = (struct tr_instr){0};
        (*samples)[scnt].valid      = valid;
        (*samples)[scnt].exception  = exception;
        (*samples)[scnt].interrupt  = interrupt;
        (*samples)[scnt].cause      = cause;
        (*samples)[scnt].tval       = tval;
        (*samples)[scnt].priv       = priv;
        (*samples)[scnt].iaddr      = iaddr;
        (*samples)[scnt].instr      = instr;
        (*samples)[scnt].compressed = compressed;
        scnt++;
    }
    /* initialize the remaining unitialized memory */
    memset((*samples) + scnt, 0, size - scnt);

    if (ferror(fp)) {
        status = -trdb_scan_file;
        goto fail;
    }

    *count = scnt;
out:
    if (fp)
        fclose(fp);
    return status;
fail:
    free(*samples);
    samples = NULL;
    goto out;
}

int trdb_cvs_to_trace_list(struct trdb_ctx *c, const char *path,
                           struct trdb_instr_head *instrs, size_t *count)
{

    FILE *fp                = NULL;
    int status              = 0;
    struct tr_instr *sample = NULL;

    *count = 0;

    if (!c || !path || !instrs)
        status = -trdb_invalid;

    fp = fopen(path, "r");
    if (!fp) {
        status = -trdb_file_open;
        goto fail;
    }
    size_t scnt = 0;

    int valid      = 0;
    int exception  = 0;
    int interrupt  = 0;
    uint32_t cause = 0;
    addr_t tval    = 0;
    uint32_t priv  = 0;
    addr_t iaddr   = 0;
    insn_t instr   = 0;

    char *line = NULL;
    size_t len = 0;

    /* reading header line */
    if (getline(&line, &len, fp) == -1) {
        status = -trdb_bad_cvs_header;
        goto fail;
    }

    if (!strcmp(line, "VALID,ADDRESS,INSN,PRIVILEGE,"
                      "EXCEPTION,ECAUSE,TVAL,INTERRUPT")) {
        status = -trdb_bad_cvs_header;
        goto fail;
    }

    /* parse data into tr_instr list */
    while (getline(&line, &len, fp) != -1) {

        sample = malloc(sizeof(*sample));
        if (!sample) {
            status = -trdb_nomem;
            goto fail;
        }
        *sample = (struct tr_instr){0};

        int ele = 7;
        const char *tok;
        for (tok = strtok(line, ","); tok && *tok; tok = strtok(NULL, ",\n")) {

            if (ele < 0) {
                err(c, "reading too many tokens per line\n");
                status = -trdb_scan_state_invalid;
                goto fail;
            }

            switch (ele) {
            case 7:
                sscanf(tok, "%d", &valid);
                sample->valid = valid;
                break;
            case 6:
                sscanf(tok, "%" SCNxADDR "", &iaddr);
                sample->iaddr = iaddr;
                break;
            case 5:
                sscanf(tok, "%" SCNxINSN "", &instr);
                sample->instr      = instr;
                sample->compressed = ((instr & 3) != 3);
                break;
            case 4:
                sscanf(tok, "%" SCNx32 "", &priv);
                sample->priv = priv;
                break;
            case 3:
                sscanf(tok, "%d", &exception);
                sample->exception = exception;
                break;
            case 2:
                sscanf(tok, "%" SCNx32 "", &cause);
                sample->cause = cause;
                break;
            case 1:
                sscanf(tok, "%" SCNxADDR "", &tval);
                sample->tval = tval;
                break;
            case 0:
                sscanf(tok, "%d", &interrupt);
                sample->interrupt = interrupt;
                break;
            }
            ele--;
        }

        if (ele > 0) {
            err(c, "wrong number of tokens on line, still %d remaining\n", ele);
            status = -trdb_scan_state_invalid;
            goto fail;
        }

        TAILQ_INSERT_TAIL(instrs, sample, list);

        scnt++;
    }

    free(line);

    if (ferror(fp)) {
        status = -trdb_scan_file;
        goto fail;
    }

    *count = scnt;

out:
    if (fp)
        fclose(fp);
    return status;
fail:
    // TODO: it's maybe better to not free the whole list, but just the part
    // where failed
    free(sample);
    trdb_free_instr_list(instrs);
    goto out;
}

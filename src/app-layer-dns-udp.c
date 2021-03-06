/* Copyright (C) 2013 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 * \author Victor Julien <victor@inliniac.net>
 */

#include "suricata-common.h"
#include "suricata.h"

#include "debug.h"
#include "decode.h"

#include "flow-util.h"

#include "threads.h"

#include "util-print.h"
#include "util-pool.h"
#include "util-debug.h"

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp.h"
#include "stream.h"

#include "app-layer-protos.h"
#include "app-layer-parser.h"

#include "util-spm.h"
#include "util-unittest.h"

#include "app-layer-dns-udp.h"

/** \internal
 *  \brief Parse DNS request packet
 */
static int DNSUDPRequestParse(Flow *f, void *dstate,
                          AppLayerParserState *pstate,
                          uint8_t *input, uint32_t input_len,
                          void *local_data, AppLayerParserResult *output)
{
    DNSState *dns_state = (DNSState *)dstate;

    SCLogDebug("starting %u", input_len);

    /** \todo remove this when PP is fixed to enforce ipproto */
    if (f != NULL && f->proto != IPPROTO_UDP)
        SCReturnInt(-1);

    if (input_len == 0 || input_len < sizeof(DNSHeader)) {
        SCLogDebug("ilen too small, hoped for at least %"PRIuMAX, (uintmax_t)sizeof(DNSHeader));
        goto insufficient_data;
    }

    DNSHeader *dns_header = (DNSHeader *)input;
    SCLogDebug("DNS %p", dns_header);

    if (DNSValidateRequestHeader(dns_state, dns_header) < 0)
        goto bad_data;

    uint16_t q;
    const uint8_t *data = input + sizeof(DNSHeader);
    for (q = 0; q < ntohs(dns_header->questions); q++) {
        uint8_t fqdn[DNS_MAX_SIZE];
        uint16_t fqdn_offset = 0;

        if (input + input_len < data + 1) {
            SCLogDebug("input buffer too small for len");
            goto insufficient_data;
        }
        SCLogDebug("query length %u", *data);

        while (*data != 0) {
            if (*data > 63) {
                /** \todo set event?*/
                goto insufficient_data;
            }
            uint8_t length = *data;

            data++;

            if (length == 0) {
                break;
            }

            if (input + input_len < data + length) {
                SCLogDebug("input buffer too small for domain of len %u", length);
                goto insufficient_data;
            }
            //PrintRawDataFp(stdout, data, qry->length);

            if ((size_t)(fqdn_offset + length + 1) < sizeof(fqdn)) {
                memcpy(fqdn + fqdn_offset, data, length);
                fqdn_offset += length;
                fqdn[fqdn_offset++] = '.';
            } else {
                /** \todo set event? */
                goto insufficient_data;
            }

            data += length;

            if (input + input_len < data + 1) {
                SCLogDebug("input buffer too small for len(2)");
                goto insufficient_data;
            }

            SCLogDebug("qry length %u", *data);
        }
        if (fqdn_offset) {
            fqdn_offset--;
        }

        data++;
        if (input + input_len < data + sizeof(DNSQueryTrailer)) {
            SCLogDebug("input buffer too small for DNSQueryTrailer");
            goto insufficient_data;
        }
        DNSQueryTrailer *trailer = (DNSQueryTrailer *)data;
        SCLogDebug("trailer type %04x class %04x", ntohs(trailer->type), ntohs(trailer->class));
        data += sizeof(DNSQueryTrailer);

        /* store our data */
        if (dns_state != NULL) {
            DNSStoreQueryInState(dns_state, fqdn, fqdn_offset,
                    ntohs(trailer->type), ntohs(trailer->class),
                    ntohs(dns_header->tx_id));
        }
    }

	SCReturnInt(1);
bad_data:
insufficient_data:
    SCReturnInt(-1);
}

/** \internal
 *  \brief DNS UDP record parser, entry function
 *
 *  Parses a DNS UDP record and fills the DNS state
 *
 */
static int DNSUDPResponseParse(Flow *f, void *dstate,
                          AppLayerParserState *pstate,
                          uint8_t *input, uint32_t input_len,
                          void *local_data, AppLayerParserResult *output)
{
	DNSState *dns_state = (DNSState *)dstate;

    SCLogDebug("starting %u", input_len);

    /** \todo remove this when PP is fixed to enforce ipproto */
    if (f != NULL && f->proto != IPPROTO_UDP)
        SCReturnInt(-1);

    if (input_len == 0 || input_len < sizeof(DNSHeader)) {
        SCLogDebug("ilen too small, hoped for at least %"PRIuMAX, (uintmax_t)sizeof(DNSHeader));
        goto insufficient_data;
    }

    DNSHeader *dns_header = (DNSHeader *)input;
    SCLogDebug("DNS %p %04x %04x", dns_header, ntohs(dns_header->tx_id), dns_header->flags);

    DNSTransaction *tx = NULL;
    int found = 0;
    TAILQ_FOREACH(tx, &dns_state->tx_list, next) {
        if (tx->tx_id == ntohs(dns_header->tx_id)) {
            found = 1;
            break;
        }
    }
    if (DNSValidateResponseHeader(dns_state, dns_header) < 0)
        goto bad_data;

    SCLogDebug("queries %04x", ntohs(dns_header->questions));

    uint16_t q;
    const uint8_t *data = input + sizeof(DNSHeader);
    for (q = 0; q < ntohs(dns_header->questions); q++) {
        uint8_t fqdn[DNS_MAX_SIZE];
        uint16_t fqdn_offset = 0;

        if (input + input_len < data + 1) {
            SCLogDebug("input buffer too small for len");
            goto insufficient_data;
        }
        SCLogDebug("qry length %u", *data);

        while (*data != 0) {
            uint8_t length = *data;
            data++;

            if (length == 0)
                break;

            if (input + input_len < data + length) {
                SCLogDebug("input buffer too small for domain of len %u", length);
                goto insufficient_data;
            }
            //PrintRawDataFp(stdout, data, length);

            if ((size_t)(fqdn_offset + length + 1) < sizeof(fqdn)) {
                memcpy(fqdn + fqdn_offset, data, length);
                fqdn_offset += length;
                fqdn[fqdn_offset++] = '.';
            }

            data += length;

            if (input + input_len < data + 1) {
                SCLogDebug("input buffer too small for len");
                goto insufficient_data;
            }

            length = *data;
            SCLogDebug("length %u", length);
        }
        if (fqdn_offset) {
            fqdn_offset--;
        }

        data++;
        if (input + input_len < data + sizeof(DNSQueryTrailer)) {
            SCLogDebug("input buffer too small for DNSQueryTrailer");
            goto insufficient_data;
        }
#if DEBUG
        DNSQueryTrailer *trailer = (DNSQueryTrailer *)data;
        SCLogDebug("trailer type %04x class %04x", ntohs(trailer->type), ntohs(trailer->class));
#endif
        data += sizeof(DNSQueryTrailer);
    }

    SCLogDebug("answer_rr %04x", ntohs(dns_header->answer_rr));
    for (q = 0; q < ntohs(dns_header->answer_rr); q++) {
        data = DNSReponseParse(dns_state, dns_header, q, DNS_LIST_ANSWER,
                input, input_len, data);
        if (data == NULL) {
            goto insufficient_data;
        }
    }

    SCLogDebug("authority_rr %04x", ntohs(dns_header->authority_rr));
    for (q = 0; q < ntohs(dns_header->authority_rr); q++) {
        data = DNSReponseParse(dns_state, dns_header, q, DNS_LIST_AUTHORITY,
                input, input_len, data);
        if (data == NULL) {
            goto insufficient_data;
        }
    }

    /* see if this is a "no such name" error */
    if (ntohs(dns_header->flags) & 0x0003) {
        SCLogDebug("no such name");

        if (dns_state->curr != NULL) {
            dns_state->curr->no_such_name = 1;
        }
    }

    if (!found) {
        SCLogDebug("DNS_DECODER_EVENT_UNSOLLICITED_RESPONSE");
        DNSSetEvent(dns_state, DNS_DECODER_EVENT_UNSOLLICITED_RESPONSE);
    }

	SCReturnInt(1);

bad_data:
insufficient_data:
    DNSSetEvent(dns_state, DNS_DECODER_EVENT_MALFORMED_DATA);
    SCReturnInt(-1);
}

static uint16_t DNSUdpProbingParser(uint8_t *input, uint32_t ilen, uint32_t *offset)
{
    if (ilen == 0 || ilen < sizeof(DNSHeader)) {
        SCLogDebug("ilen too small, hoped for at least %"PRIuMAX, (uintmax_t)sizeof(DNSHeader));
        return ALPROTO_UNKNOWN;
    }

    if (DNSUDPRequestParse(NULL, NULL, NULL, input, ilen, NULL, NULL) == -1)
        return ALPROTO_FAILED;

    return ALPROTO_DNS_UDP;
}


void RegisterDNSUDPParsers(void) {
    char *proto_name = "dnsudp";

    /** DNS */
    if (AppLayerProtoDetectionEnabled(proto_name)) {
        if (RunmodeIsUnittests()) {
            AppLayerRegisterProbingParser(&alp_proto_ctx,
                                          IPPROTO_UDP,
                                          "53",
                                          proto_name,
                                          ALPROTO_DNS,
                                          0, sizeof(DNSHeader),
                                          STREAM_TOSERVER,
                                          DNSUdpProbingParser);
        } else {
            AppLayerParseProbingParserPorts(proto_name, ALPROTO_DNS,
                                            0, sizeof(DNSHeader),
                                            DNSUdpProbingParser);
        }
    } else {
        SCLogInfo("Protocol detection and parser disabled for %s protocol.",
                  proto_name);
        return;
    }

    if (AppLayerParserEnabled(proto_name)) {
        AppLayerRegisterProto(proto_name, ALPROTO_DNS_UDP, STREAM_TOSERVER,
                              DNSUDPRequestParse);
        AppLayerRegisterProto(proto_name, ALPROTO_DNS_UDP, STREAM_TOCLIENT,
                              DNSUDPResponseParse);
        AppLayerRegisterStateFuncs(ALPROTO_DNS_UDP, DNSStateAlloc,
                                   DNSStateFree);
        AppLayerRegisterTxFreeFunc(ALPROTO_DNS_UDP,
                                   DNSStateTransactionFree);

        AppLayerRegisterGetEventsFunc(ALPROTO_DNS_UDP, DNSGetEvents);
        AppLayerRegisterHasEventsFunc(ALPROTO_DNS_UDP, DNSHasEvents);

        AppLayerRegisterGetTx(ALPROTO_DNS_UDP,
                              DNSGetTx);
        AppLayerRegisterGetTxCnt(ALPROTO_DNS_UDP,
                                 DNSGetTxCnt);
        AppLayerRegisterGetAlstateProgressFunc(ALPROTO_DNS_UDP,
                                               DNSGetAlstateProgress);
        AppLayerRegisterGetAlstateProgressCompletionStatus(ALPROTO_DNS_UDP,
                                                           DNSGetAlstateProgressCompletionStatus);

        DNSAppLayerRegisterGetEventInfo(ALPROTO_DNS_UDP);
    } else {
        SCLogInfo("Parsed disabled for %s protocol. Protocol detection"
                  "still on.", proto_name);
    }
#ifdef UNITTESTS
    AppLayerParserRegisterUnittests(ALPROTO_DNS_UDP, DNSUDPParserRegisterTests);
#endif
}

/* UNITTESTS */
#ifdef UNITTESTS
#include "util-unittest-helper.h"

static int DNSUDPParserTest01 (void) {
    int result = 0;
    /* query: abcdefghijk.com
     * TTL: 86400
     * serial 20130422 refresh 28800 retry 7200 exp 604800 min ttl 86400
     * ns, hostmaster */
    uint8_t buf[] = { 0x00, 0x3c, 0x85, 0x00, 0x00, 0x01, 0x00, 0x00,
                      0x00, 0x01, 0x00, 0x00, 0x0b, 0x61, 0x62, 0x63,
                      0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b,
                      0x03, 0x63, 0x6f, 0x6d, 0x00, 0x00, 0x0f, 0x00,
                      0x01, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01,
                      0x51, 0x80, 0x00, 0x25, 0x02, 0x6e, 0x73, 0x00,
                      0x0a, 0x68, 0x6f, 0x73, 0x74, 0x6d, 0x61, 0x73,
                      0x74, 0x65, 0x72, 0xc0, 0x2f, 0x01, 0x33, 0x2a,
                      0x76, 0x00, 0x00, 0x70, 0x80, 0x00, 0x00, 0x1c,
                      0x20, 0x00, 0x09, 0x3a, 0x80, 0x00, 0x01, 0x51,
                      0x80};
    size_t buflen = sizeof(buf);
    Flow *f = NULL;

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 53);
    if (f == NULL)
        goto end;
    f->proto = IPPROTO_UDP;
    f->alproto = ALPROTO_DNS_UDP;
    f->alstate = DNSStateAlloc();

    int r = DNSUDPResponseParse(f, f->alstate, NULL, buf, buflen, NULL, NULL);
    if (r != 1)
        goto end;

    result = 1;
end:
    UTHFreeFlow(f);
    return (result);
}

void DNSUDPParserRegisterTests(void) {
	UtRegisterTest("DNSUDPParserTest01", DNSUDPParserTest01, 1);
}
#endif

#pragma once

#include <stddef.h>
#include <stdint.h>

// Turns a DNS query in `packet` (length `length`, buffer size `capacity`)
// into a response in place, answering any A/ANY question with `ip`. Other
// question types get an empty (no-answer) response. Records after the
// question, such as an EDNS OPT record, are dropped so the appended answer
// parses correctly. Returns the response length, or 0 to ignore the packet.
inline size_t captive_dns_response(uint8_t *packet, size_t length, size_t capacity,
                                   const uint8_t ip[4])
{
    constexpr size_t ANSWER_LEN = 16;
    // Header is 12 bytes. Only plain queries (QR=0, opcode 0) with one question.
    if (!packet || length < 12 || length > capacity) return 0;
    if ((packet[2] & 0xF8) != 0 || packet[4] != 0 || packet[5] != 1) return 0;

    // Skip the question name (a run of length-prefixed labels).
    size_t pos = 12;
    while (pos < length && packet[pos] != 0) {
        if (packet[pos] & 0xC0) return 0;
        pos += packet[pos] + 1;
    }
    if (pos + 5 > length) return 0;
    const uint16_t qtype = static_cast<uint16_t>((packet[pos + 1] << 8) | packet[pos + 2]);
    pos += 5; // Terminating zero, QTYPE and QCLASS.

    const bool answer = qtype == 1 /* A */ || qtype == 255 /* ANY */;
    if (answer && pos + ANSWER_LEN > capacity) return 0;

    packet[2] = static_cast<uint8_t>(0x84 | (packet[2] & 0x01)); // QR, AA, keep RD
    packet[3] = 0x00;                                             // No error
    packet[6] = 0; packet[7] = answer ? 1 : 0;                    // ANCOUNT
    packet[8] = 0; packet[9] = 0;                                 // NSCOUNT
    packet[10] = 0; packet[11] = 0;                               // ARCOUNT

    if (answer) {
        const uint8_t record[ANSWER_LEN] = {
            0xC0, 0x0C,             // Name: pointer to the question
            0x00, 0x01, 0x00, 0x01, // Type A, class IN
            0x00, 0x00, 0x00, 0x3C, // TTL 60 s
            0x00, 0x04, ip[0], ip[1], ip[2], ip[3],
        };
        for (size_t i = 0; i < ANSWER_LEN; ++i) packet[pos++] = record[i];
    }
    return pos;
}

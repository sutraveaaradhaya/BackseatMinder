#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "captive_dns.h"
#include "phone_number.h"

namespace {

const uint8_t SETUP_IP[4] = {192, 168, 4, 1};

// Builds a one-question query for `name`; optionally appends an EDNS OPT
// record as phones commonly do.
std::vector<uint8_t> query(const char *name, uint16_t qtype, bool edns)
{
    std::vector<uint8_t> packet = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0,
                                   static_cast<uint8_t>(edns ? 1 : 0)};
    for (const char *label = name; *label;) {
        const char *dot = std::strchr(label, '.');
        const size_t length = dot ? static_cast<size_t>(dot - label) : std::strlen(label);
        packet.push_back(static_cast<uint8_t>(length));
        packet.insert(packet.end(), label, label + length);
        label += length + (dot ? 1 : 0);
    }
    packet.push_back(0);
    packet.push_back(static_cast<uint8_t>(qtype >> 8));
    packet.push_back(static_cast<uint8_t>(qtype));
    packet.push_back(0);
    packet.push_back(1);
    if (edns) {
        const uint8_t opt[] = {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0};
        packet.insert(packet.end(), opt, opt + sizeof(opt));
    }
    return packet;
}

size_t respond(std::vector<uint8_t> &packet, size_t capacity = 512)
{
    const size_t length = packet.size();
    packet.resize(capacity);
    return captive_dns_response(packet.data(), length, capacity, SETUP_IP);
}

void test_dns()
{
    for (bool edns : {false, true}) {
        auto packet = query("connectivitycheck.gstatic.com", 1, edns);
        const size_t question_end = packet.size() - (edns ? 11 : 0);
        const size_t length = respond(packet);
        assert(length == question_end + 16);
        assert(packet[0] == 0x12 && packet[1] == 0x34);  // ID kept
        assert(packet[2] == 0x85 && packet[3] == 0x00);  // QR, AA, RD; no error
        assert(packet[7] == 1 && packet[9] == 0 && packet[11] == 0);
        const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
        assert(std::memcmp(packet.data() + question_end, answer, sizeof(answer)) == 0);
    }

    auto aaaa = query("captive.apple.com", 28, true);
    const size_t aaaa_question_end = aaaa.size() - 11;
    assert(respond(aaaa) == aaaa_question_end);
    assert(aaaa[7] == 0 && aaaa[11] == 0);  // No answer, OPT dropped

    auto response = query("example.com", 1, false);
    response[2] |= 0x80;  // Already a response: ignore.
    assert(respond(response) == 0);

    auto truncated = query("example.com", 1, false);
    truncated.resize(20);
    assert(respond(truncated) == 0);

    auto compressed = query("example.com", 1, false);
    compressed[12] = 0xC0;  // Pointer in a question name: ignore.
    assert(respond(compressed) == 0);

    auto full = query("example.com", 1, false);
    assert(respond(full, full.size() + 15) == 0);  // No room for the answer.

    std::vector<uint8_t> tiny = {1, 2, 3};
    assert(respond(tiny) == 0);
    puts("PASS: captive DNS answers A/ANY with the setup IP, drops EDNS, ignores bad packets");
}

void test_phone()
{
    char out[32];
    assert(normalize_phone("+1 (555) 000-0000", out, sizeof(out)));
    assert(std::string(out) == "+15550000000");
    assert(normalize_phone("555.000.1234", out, sizeof(out)));
    assert(std::string(out) == "5550001234");
    char same[32] = "+44 20 7946 0958";
    assert(normalize_phone(same, same, sizeof(same)));
    assert(std::string(same) == "+442079460958");

    std::strcpy(out, "unchanged");
    for (const char *bad : {"", "123456", "+0 555 000 0000", "555-CALL-NOW",
                            "1234567890123456", "+1 555 000 0000 ext 2",
                            "                                         5"}) {
        assert(!normalize_phone(bad, out, sizeof(out)));
        assert(std::string(out) == "unchanged");
    }
    char small[8];
    assert(!normalize_phone("+15550000000", small, sizeof(small)));
    assert(!normalize_phone(nullptr, out, sizeof(out)));
    puts("PASS: phone numbers are normalized and validated for setup");
}

} // namespace

int main()
{
    test_dns();
    test_phone();
}

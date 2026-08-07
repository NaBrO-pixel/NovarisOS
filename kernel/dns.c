/* dns.c - turning a name into an address.
 *
 * Milestone 39. One query, one answer, A records only. No cache beyond
 * four entries, no CNAME chasing beyond following what the server
 * already put in the same reply, no IPv6, no TCP fallback for long
 * answers.
 *
 * The interesting part is the name encoding, because it is the second
 * place in this kernel that parses attacker-chosen bytes and the first
 * that parses a format with *pointers in it*. A DNS name in a reply can
 * be a compression pointer back into the message, and a pointer can point
 * at another pointer. A parser that follows them without a bound is a
 * parser that loops forever on a malicious reply, so skip_name() counts
 * its jumps and gives up.
 */

#include "dns.h"
#include "net.h"
#include "kstring.h"
#include "pit.h"

#define DNS_PORT 53
#define DNS_CLIENT_PORT 5353

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t questions, answers, authority, additional;
} dns_header_t;

#define DNS_TYPE_A   1
#define DNS_CLASS_IN 1

typedef struct {
    char     name[64];
    uint32_t ip;
    uint32_t stamp;
    int      valid;
} dns_entry_t;

#define DNS_CACHE 4
static dns_entry_t cache[DNS_CACHE];

/* "www.example.com" -> 3www7example3com0. Returns the encoded length, or
 * 0 if the name does not fit or a label is over-long. */
static uint32_t encode_name(const char* name, uint8_t* out, uint32_t max) {
    uint32_t written = 0;

    while (*name) {
        const char* label = name;
        uint32_t len = 0;
        while (name[len] && name[len] != '.') len++;
        if (len == 0 || len > 63) return 0;
        if (written + len + 1 >= max) return 0;

        out[written++] = (uint8_t)len;
        for (uint32_t i = 0; i < len; i++) out[written++] = (uint8_t)label[i];

        name += len;
        if (*name == '.') name++;
    }

    if (written + 1 > max) return 0;
    out[written++] = 0;
    return written;
}

/* Steps over a name in a reply, following compression pointers. Returns
 * the offset just past the name *in the message*, which for a pointer is
 * two bytes rather than wherever it pointed. */
static uint32_t skip_name(const uint8_t* msg, uint32_t len, uint32_t off) {
    uint32_t jumps = 0;

    while (off < len) {
        uint8_t b = msg[off];

        if ((b & 0xC0) == 0xC0) {
            /* A pointer. Its target is not walked here - the caller only
             * needs to know where the *record* continues, which is two
             * bytes on. */
            return off + 2 <= len ? off + 2 : len;
        }
        if (b == 0) return off + 1;
        if (++jumps > 128) return len;      /* a reply designed to loop */

        off += 1 + b;
    }
    return len;
}

int dns_resolve(const char* name, uint32_t* out, uint32_t timeout_ticks) {
    static uint8_t query[512];
    static udp_datagram_t reply;

    if (!name || !*out) { /* out is written, not read; keep the compiler quiet */ }
    if (!name) return 0;

    /* An address typed instead of a name is not a failure to look up. */
    if (net_parse_ip(name, out)) return 1;

    uint32_t now = pit_get_ticks();
    for (int i = 0; i < DNS_CACHE; i++) {
        if (cache[i].valid && kstrcmp(cache[i].name, name) == 0) {
            *out = cache[i].ip;
            return 1;
        }
    }

    uint32_t server = net_config()->dns;
    if (!server) return 0;

    uint16_t id = (uint16_t)(0x4E00u | (now & 0xFF));

    dns_header_t* h = (dns_header_t*)query;
    h->id = htons(id);
    h->flags = htons(0x0100);          /* standard query, recursion desired */
    h->questions = htons(1);
    h->answers = h->authority = h->additional = 0;

    uint32_t n = encode_name(name, query + sizeof(dns_header_t),
                             sizeof(query) - sizeof(dns_header_t) - 4);
    if (!n) return 0;

    uint32_t off = sizeof(dns_header_t) + n;
    query[off++] = 0; query[off++] = DNS_TYPE_A;
    query[off++] = 0; query[off++] = DNS_CLASS_IN;

    if (!net_udp_bind(DNS_CLIENT_PORT)) return 0;
    net_send_udp(server, DNS_CLIENT_PORT, DNS_PORT, query, off);

    uint32_t deadline = now + timeout_ticks;
    int found = 0;

    while (pit_get_ticks() < deadline && !found) {
        net_poll();
        if (!net_udp_recv(DNS_CLIENT_PORT, &reply)) {
            __asm__ __volatile__("hlt");
            continue;
        }
        if (reply.len < sizeof(dns_header_t)) continue;

        const dns_header_t* rh = (const dns_header_t*)reply.data;
        if (ntohs(rh->id) != id) continue;
        if ((ntohs(rh->flags) & 0x000F) != 0) break;      /* an error code */

        uint32_t p = sizeof(dns_header_t);
        uint32_t questions = ntohs(rh->questions);
        uint32_t answers = ntohs(rh->answers);

        for (uint32_t q = 0; q < questions && p < reply.len; q++) {
            p = skip_name(reply.data, reply.len, p);
            p += 4;                                        /* type + class */
        }

        for (uint32_t a = 0; a < answers && p + 10 <= reply.len; a++) {
            p = skip_name(reply.data, reply.len, p);
            if (p + 10 > reply.len) break;

            uint16_t type = (uint16_t)((reply.data[p] << 8) | reply.data[p + 1]);
            uint16_t rdlen = (uint16_t)((reply.data[p + 8] << 8) | reply.data[p + 9]);
            p += 10;
            if (p + rdlen > reply.len) break;

            if (type == DNS_TYPE_A && rdlen == 4) {
                *out = ((uint32_t)reply.data[p] << 24) |
                       ((uint32_t)reply.data[p + 1] << 16) |
                       ((uint32_t)reply.data[p + 2] << 8) |
                       (uint32_t)reply.data[p + 3];
                found = 1;
                break;
            }
            p += rdlen;
        }
        break;
    }

    net_udp_unbind(DNS_CLIENT_PORT);

    if (found) {
        /* Oldest slot out. Four entries is what one program fetching from
         * one host needs, and a bigger cache with no TTL handling would
         * only be wrong for longer. */
        dns_entry_t* slot = &cache[0];
        for (int i = 1; i < DNS_CACHE; i++) {
            if (!cache[i].valid) { slot = &cache[i]; break; }
            if (cache[i].stamp < slot->stamp) slot = &cache[i];
        }
        kstrlcpy(slot->name, name, sizeof(slot->name));
        slot->ip = *out;
        slot->stamp = pit_get_ticks();
        slot->valid = 1;
    }

    return found;
}

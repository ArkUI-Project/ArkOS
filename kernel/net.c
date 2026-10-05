/* Original bounded native Ethernet/ARP/IPv4/DHCP/DNS/TCP/HTTP client.
 * Guest TLS records are processed by the native BearSSL adapter.
 * Wire rules: RFC 826, 791, 768, 2131/2132, 1035, 9293 and 9112.
 */
#include "net.h"
#include "ark_driver.h" /* ArkNetOps: NIC drivers bind through net_bind_nic */
#include "tls.h"
static bool https, https_closing;

#ifndef NET_DNS_PORT
#define NET_DNS_PORT 53u
#endif
#define HTTP_HEADER_CAP 4096u
#define TCP_DATA_CAP 1200u
#define TCP_WINDOW 8192u
#define HTTP_TIMEOUT 3000u
static NetStatus status;
static NetHttpState http_state;
static char http_error[96], http_type[96], http_body[NET_HTTP_BODY_CAP + 1];
static char http_host[254], http_path[768], http_request[1152];
static unsigned http_code;
static size_t http_length, request_length, request_offset;
static uint64_t http_started;
static uint16_t http_port, ip_identification, nonce;
static uint8_t rx_frame[1600], tx_frame[1600];

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    uint64_t expires;
} Neighbor;
static Neighbor neighbors[8];
static unsigned neighbor_next;
/* Bound NIC driver ops (module code inside the kernel module window) and the
 * module slot that owns them, so removal can unbind before the image drops
 * out of service. nic_bound gates every dereference. */
static ArkNetOps nic;
static int nic_owner = -1;
static bool nic_bound;
static uint32_t arp_pending;
static uint64_t arp_sent;
static unsigned dhcp_phase, dhcp_attempt;
static uint32_t dhcp_xid, dhcp_offer, dhcp_server, dhcp_mask, dhcp_gateway, dhcp_dns;
static uint64_t dhcp_deadline, lease_renew, lease_rebind, lease_expire, link_check;
static uint16_t dns_id, dns_port;
static unsigned dns_attempt;
static uint64_t dns_deadline;
static uint32_t remote_ip;
static uint16_t local_port, peer_mss, peer_window;
static uint32_t send_una, send_next, receive_next;
static bool tcp_open, tcp_established, tcp_peer_fin, tcp_fin_sent, flight_active;
static uint8_t flight_flags, flight_data[TCP_DATA_CAP];
static size_t flight_length;
static uint32_t flight_sequence;
static uint64_t flight_deadline, tcp_last_activity, tcp_close_deadline;
static unsigned flight_retries;
static bool flight_sent;

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put16(uint8_t *p, uint16_t n) {
    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)n;
}
static void put32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
}
static bool equal(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return false;
    return true;
}
static bool unicast(uint32_t ip) {
    return ip && ip != 0xffffffffu && (ip >> 28) != 0xe && (ip >> 28) != 0xf;
}
static uint32_t sum_bytes(const uint8_t *p, size_t n, uint32_t sum) {
    while (n > 1) {
        sum += be16(p);
        p += 2;
        n -= 2;
    }
    if (n)
        sum += (unsigned)*p << 8;
    return sum;
}
static uint16_t sum_finish(uint32_t sum) {
    while (sum >> 16)
        sum = (sum & 65535) + (sum >> 16);
    return (uint16_t)~sum;
}
static uint16_t checksum(const uint8_t *p, size_t n) {
    return sum_finish(sum_bytes(p, n, 0));
}
static uint16_t transport_sum(uint32_t source, uint32_t target, unsigned protocol, const uint8_t *p,
                              size_t n) {
    uint32_t sum = (source >> 16) + (source & 65535) + (target >> 16) + (target & 65535) +
                   protocol + (uint32_t)n;
    return sum_finish(sum_bytes(p, n, sum));
}
static bool before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}
static void set_message(const char *s) {
    strcopy(status.message, s, sizeof status.message);
}
static void number_log(const char *prefix, uint64_t n) {
    char text[24];
    serial_write(prefix);
    uint_to_str(n, text);
    serial_write(text);
}
void net_format_ipv4(uint32_t ip, char output[16]) {
    size_t at = 0;
    for (unsigned i = 0; i < 4; i++) {
        char value[4];
        uint_to_str((ip >> (24 - i * 8)) & 255, value);
        for (size_t j = 0; value[j]; j++)
            output[at++] = value[j];
        if (i != 3)
            output[at++] = '.';
    }
    output[at] = 0;
}
static bool parse_ipv4(const char *s, uint32_t *out) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned octet = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 3)
                return false;
            octet = octet * 10 + (unsigned)(*s++ - '0');
            if (octet > 255)
                return false;
        }
        if (!digits)
            return false;
        value = (value << 8) | octet;
        if (i == 3) {
            if (*s)
                return false;
        } else if (*s++ != '.')
            return false;
    }
    *out = value;
    return unicast(value);
}
static bool ethernet_send(const uint8_t *destination, unsigned type, const uint8_t *payload,
                          size_t n) {
    if (n > 1500 || !status.link)
        return false;
    memcpy(tx_frame, destination, 6);
    memcpy(tx_frame + 6, status.mac, 6);
    put16(tx_frame + 12, (uint16_t)type);
    memcpy(tx_frame + 14, payload, n);
    if (!nic_bound || !nic.send(tx_frame, (uint32_t)(n + 14)))
        return false;
    status.tx_packets++;
    status.tx_bytes += n + 14;
    return true;
}
static const uint8_t broadcast_mac[6] = {255, 255, 255, 255, 255, 255};
static void arp_send(uint32_t target, bool reply, const uint8_t *destination) {
    uint8_t data[28] = {0};
    put16(data, 1);
    put16(data + 2, 0x0800);
    data[4] = 6;
    data[5] = 4;
    put16(data + 6, reply ? 2 : 1);
    memcpy(data + 8, status.mac, 6);
    put32(data + 14, status.ipv4);
    if (reply)
        memcpy(data + 18, destination, 6);
    put32(data + 24, target);
    ethernet_send(destination, 0x0806, data, sizeof data);
}
static void neighbor_add(uint32_t ip, const uint8_t *mac) {
    if (!unicast(ip) || (mac[0] & 1) || equal(mac, "\0\0\0\0\0\0", 6))
        return;
    Neighbor *entry = 0;
    for (unsigned i = 0; i < 8; i++)
        if (neighbors[i].ip == ip) {
            entry = &neighbors[i];
            break;
        }
    if (!entry)
        entry = &neighbors[neighbor_next++ % 8];
    entry->ip = ip;
    memcpy(entry->mac, mac, 6);
    entry->expires = platform_ticks() + 6000;
}
static const uint8_t *route(uint32_t destination) {
    if (destination == 0xffffffffu)
        return broadcast_mac;
    uint32_t next =
        ((destination & status.mask) == (status.ipv4 & status.mask)) ? destination : status.gateway;
    if (!next)
        return 0;
    uint64_t now = platform_ticks();
    for (unsigned i = 0; i < 8; i++)
        if (neighbors[i].ip == next && neighbors[i].expires > now)
            return neighbors[i].mac;
    if (arp_pending != next || now - arp_sent >= 100) {
        arp_pending = next;
        arp_sent = now;
        arp_send(next, false, broadcast_mac);
    }
    return 0;
}
static bool ipv4_send(uint32_t destination, unsigned protocol, const uint8_t *data, size_t length) {
    if (length > 1480)
        return false;
    const uint8_t *mac = route(destination);
    if (!mac)
        return false;
    uint8_t packet[1500] = {0};
    packet[0] = 0x45;
    put16(packet + 2, (uint16_t)(length + 20));
    put16(packet + 4, ++ip_identification);
    put16(packet + 6, 0x4000);
    packet[8] = 64;
    packet[9] = (uint8_t)protocol;
    put32(packet + 12, status.ipv4);
    put32(packet + 16, destination);
    put16(packet + 10, checksum(packet, 20));
    memcpy(packet + 20, data, length);
    return ethernet_send(mac, 0x0800, packet, length + 20);
}
static bool udp_send(uint32_t target, unsigned source_port, unsigned target_port,
                     const uint8_t *data, size_t n) {
    if (n > 1472)
        return false;
    uint8_t packet[1480];
    put16(packet, (uint16_t)source_port);
    put16(packet + 2, (uint16_t)target_port);
    put16(packet + 4, (uint16_t)(n + 8));
    put16(packet + 6, 0);
    memcpy(packet + 8, data, n);
    uint16_t sum = transport_sum(status.ipv4, target, 17, packet, n + 8);
    put16(packet + 6, sum ? sum : 0xffff);
    return ipv4_send(target, 17, packet, n + 8);
}
static void http_fail(const char *message) {
    if (http_state == NET_HTTP_DONE)
        return; /* Closing TCP cannot invalidate a complete response. */
    strcopy(http_error, message, sizeof http_error);
    http_state = NET_HTTP_ERROR;
    serial_write("[net] HTTP error: ");
    serial_write(message);
    serial_write("\n");
}
static void tcp_packet(uint32_t sequence, unsigned flags, const uint8_t *data, size_t length);
void net_http_cancel(void) {
    if (tcp_open && tcp_established)
        tcp_packet(send_next, 0x14, 0, 0);
    tcp_open = tcp_established = flight_active = false;
    http_state = NET_HTTP_IDLE;
    tls_cancel();
    https_closing = false;
}
static void dhcp_start(void) {
    status.configured = false;
    status.ipv4 = status.mask = status.gateway = status.dns = 0;
    memset(neighbors, 0, sizeof neighbors);
    arp_pending = 0;
    dhcp_phase = 1;
    dhcp_attempt = 0;
    dhcp_deadline = 0;
    dhcp_xid =
        0x41524b00u ^ (uint32_t)platform_ticks() ^ ((uint32_t)++nonce << 16) ^ be32(status.mac + 2);
    dhcp_offer = dhcp_server = dhcp_mask = dhcp_gateway = dhcp_dns = 0;
    set_message("Obtaining IPv4 configuration through DHCP");
}
static bool dhcp_send(void) {
    uint8_t packet[320] = {0};
    packet[0] = 1;
    packet[1] = 1;
    packet[2] = 6;
    put32(packet + 4, dhcp_xid);
    bool renewing = dhcp_phase >= 4;
    if (renewing)
        put32(packet + 12, status.ipv4);
    if (dhcp_phase != 4)
        put16(packet + 10, 0x8000);
    memcpy(packet + 28, status.mac, 6);
    put32(packet + 236, 0x63825363);
    size_t n = 240;
    packet[n++] = 53;
    packet[n++] = 1;
    packet[n++] = dhcp_phase == 1 ? 1 : 3;
    packet[n++] = 61;
    packet[n++] = 7;
    packet[n++] = 1;
    memcpy(packet + n, status.mac, 6);
    n += 6;
    if (dhcp_phase == 2) {
        packet[n++] = 50;
        packet[n++] = 4;
        put32(packet + n, dhcp_offer);
        n += 4;
        packet[n++] = 54;
        packet[n++] = 4;
        put32(packet + n, dhcp_server);
        n += 4;
    }
    packet[n++] = 55;
    packet[n++] = 6;
    packet[n++] = 1;
    packet[n++] = 3;
    packet[n++] = 6;
    packet[n++] = 51;
    packet[n++] = 58;
    packet[n++] = 59;
    packet[n++] = 12;
    packet[n++] = 5;
    memcpy(packet + n, "arkos", 5);
    n += 5;
    packet[n++] = 255;
    return udp_send(dhcp_phase == 4 ? dhcp_server : 0xffffffffu, 68, 67, packet, n);
}
static void dhcp_receive(uint32_t source, const uint8_t *p, size_t n) {
    if (n < 240 || p[0] != 2 || p[1] != 1 || p[2] != 6 || be32(p + 4) != dhcp_xid ||
        !equal(p + 28, status.mac, 6) || be32(p + 236) != 0x63825363)
        return;
    unsigned type = 0;
    uint32_t server = source, mask = dhcp_mask, gateway = dhcp_gateway, dns = dhcp_dns,
             lease = 3600, t1 = 0, t2 = 0;
    for (size_t at = 240; at < n;) {
        unsigned code = p[at++];
        if (code == 255)
            break;
        if (!code)
            continue;
        if (at >= n)
            return;
        unsigned length = p[at++];
        if (length > n - at)
            return;
        if (code == 53 && length == 1)
            type = p[at];
        else if (code == 54 && length == 4)
            server = be32(p + at);
        else if (code == 1 && length == 4)
            mask = be32(p + at);
        else if (code == 3 && length >= 4 && length % 4 == 0)
            gateway = be32(p + at);
        else if (code == 6 && length >= 4 && length % 4 == 0)
            dns = be32(p + at);
        else if (code == 51 && length == 4)
            lease = be32(p + at);
        else if (code == 58 && length == 4)
            t1 = be32(p + at);
        else if (code == 59 && length == 4)
            t2 = be32(p + at);
        at += length;
    }
    if (!unicast(server))
        return;
    if (type == 2 && dhcp_phase == 1 && unicast(be32(p + 16))) {
        dhcp_offer = be32(p + 16);
        dhcp_server = server;
        dhcp_mask = mask;
        dhcp_gateway = gateway;
        dhcp_dns = dns;
        dhcp_phase = 2;
        dhcp_attempt = 0;
        dhcp_deadline = 0;
        return;
    }
    if (server != dhcp_server && dhcp_phase != 5)
        return;
    if (type == 6 && (dhcp_phase == 2 || dhcp_phase >= 4)) {
        if (tcp_open)
            http_fail("DHCP server rejected the lease");
        tcp_open = false;
        dhcp_start();
        return;
    }
    if (type != 5 || (dhcp_phase != 2 && dhcp_phase < 4) || !lease)
        return;
    uint32_t address = be32(p + 16);
    if (!address && status.configured)
        address = status.ipv4;
    if (!unicast(address) || !mask)
        return;
    uint32_t inverse = ~mask;
    if ((inverse & (inverse + 1)) != 0)
        return;
    if (dhcp_phase == 2 && address != dhcp_offer)
        return;
    status.ipv4 = address;
    status.mask = mask;
    status.gateway = unicast(gateway) ? gateway : 0;
    status.dns = unicast(dns) ? dns : 0;
    status.configured = true;
    dhcp_server = server;
    dhcp_phase = 3;
    if (!t1 || t1 >= lease)
        t1 = lease / 2;
    if (!t2 || t2 <= t1 || t2 >= lease)
        t2 = lease * 7ull / 8;
    uint64_t now = platform_ticks();
    lease_renew = now + (uint64_t)t1 * 100;
    lease_rebind = now + (uint64_t)t2 * 100;
    lease_expire = now + (uint64_t)lease * 100;
    set_message("IPv4 configured; network ready");
    char ip[16];
    net_format_ipv4(address, ip);
    serial_write("[net] DHCP ready ip=");
    serial_write(ip);
    net_format_ipv4(status.gateway, ip);
    serial_write(" gateway=");
    serial_write(ip);
    net_format_ipv4(status.dns, ip);
    serial_write(" dns=");
    serial_write(ip);
    serial_write("\n");
    arp_send(status.ipv4, false, broadcast_mac);
}

/* Incremental bounded HTTP framing. The body never aliases RX DMA buffers. */
static char header_line[1024];
static size_t line_length, header_bytes;
static bool headers_done, status_seen, length_seen, chunked, body_complete;
static uint32_t content_length, chunk_left;
static unsigned chunk_state, interim_count;
static int ascii_lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
static bool insensitive(const char *a, const char *b) {
    for (;; a++, b++) {
        if (ascii_lower(*a) != ascii_lower(*b))
            return false;
        if (!*a)
            return true;
    }
}
static void http_reset_parser(void) {
    http_code = 0;
    http_length = 0;
    http_body[0] = 0;
    http_error[0] = http_type[0] = 0;
    line_length = header_bytes = 0;
    headers_done = status_seen = length_seen = chunked = body_complete = false;
    content_length = chunk_left = chunk_state = interim_count = 0;
}
static void http_complete(void) {
    if (body_complete)
        return;
    body_complete = true;
    http_state = NET_HTTP_DONE;
    number_log("[net] HTTP done status=", http_code);
    number_log(" bytes=", http_length);
    serial_write("\n");
}
static bool decimal(const char *p, uint32_t *out) {
    uint32_t n = 0;
    if (!*p)
        return false;
    for (; *p; p++) {
        if (*p < '0' || *p > '9' || n > (UINT32_MAX - (unsigned)(*p - '0')) / 10)
            return false;
        n = n * 10 + (unsigned)(*p - '0');
    }
    *out = n;
    return true;
}
static bool append_body(uint8_t c) {
    if (http_length >= NET_HTTP_BODY_CAP) {
        http_fail("HTTP body exceeds the 16 KiB limit");
        return false;
    }
    http_body[http_length++] = (char)c;
    http_body[http_length] = 0;
    return true;
}
static bool header_complete_line(void) {
    if (!status_seen) {
        if ((strncmp(header_line, "HTTP/1.0 ", 9) && strncmp(header_line, "HTTP/1.1 ", 9)) ||
            strlen(header_line) < 12 || header_line[9] < '1' || header_line[9] > '5' ||
            header_line[10] < '0' || header_line[10] > '9' || header_line[11] < '0' ||
            header_line[11] > '9' || (header_line[12] && header_line[12] != ' ')) {
            http_fail("Invalid HTTP status line");
            return false;
        }
        http_code = (unsigned)(header_line[9] - '0') * 100 +
                    (unsigned)(header_line[10] - '0') * 10 + (unsigned)(header_line[11] - '0');
        status_seen = true;
        return true;
    }
    if (!header_line[0]) {
        if (http_code < 200) {
            if (http_code == 101 || ++interim_count > 3) {
                http_fail("Unsupported HTTP protocol upgrade");
                return false;
            }
            status_seen = false;
            length_seen = chunked = false;
            content_length = 0;
            http_type[0] = 0;
            return true;
        }
        if (chunked && length_seen) {
            http_fail("Ambiguous HTTP body framing");
            return false;
        }
        headers_done = true;
        if (http_code == 204 || http_code == 304) {
            http_complete();
            return true;
        }
        if (length_seen && content_length > NET_HTTP_BODY_CAP) {
            http_fail("HTTP body exceeds the 16 KiB limit");
            return false;
        }
        if (length_seen && !content_length)
            http_complete();
        return true;
    }
    char *colon = header_line;
    while (*colon && *colon != ':')
        colon++;
    if (!*colon || colon == header_line || header_line[0] == ' ' || header_line[0] == '\t') {
        http_fail("Malformed HTTP header");
        return false;
    }
    *colon++ = 0;
    while (*colon == ' ' || *colon == '\t')
        colon++;
    size_t n = strlen(colon);
    while (n && (colon[n - 1] == ' ' || colon[n - 1] == '\t'))
        colon[--n] = 0;
    if (insensitive(header_line, "Content-Length")) {
        uint32_t value;
        if (!decimal(colon, &value) || (length_seen && value != content_length)) {
            http_fail("Invalid HTTP Content-Length");
            return false;
        }
        content_length = value;
        length_seen = true;
    } else if (insensitive(header_line, "Transfer-Encoding")) {
        if (chunked || !insensitive(colon, "chunked")) {
            http_fail("Unsupported HTTP transfer encoding");
            return false;
        }
        chunked = true;
    } else if (insensitive(header_line, "Content-Encoding")) {
        if (!insensitive(colon, "identity")) {
            http_fail("Compressed HTTP content is unsupported");
            return false;
        }
    } else if (insensitive(header_line, "Content-Type"))
        strcopy(http_type, colon, sizeof http_type);
    return true;
}
static bool consume_line(uint8_t c, bool chunk_line) {
    if (++header_bytes > HTTP_HEADER_CAP) {
        http_fail("HTTP headers or trailers are too large");
        return false;
    }
    if (c == '\n') {
        if (!line_length || header_line[line_length - 1] != '\r') {
            http_fail("HTTP requires CRLF line endings");
            return false;
        }
        header_line[--line_length] = 0;
        line_length = 0;
        if (!chunk_line)
            return header_complete_line();
        if (chunk_state == 4) {
            if (!header_line[0])
                http_complete();
            return true;
        }
        uint32_t value = 0;
        size_t i = 0;
        for (; header_line[i] && header_line[i] != ';'; i++) {
            unsigned digit;
            int c2 = ascii_lower(header_line[i]);
            if (c2 >= '0' && c2 <= '9')
                digit = (unsigned)(c2 - '0');
            else if (c2 >= 'a' && c2 <= 'f')
                digit = (unsigned)(c2 - 'a' + 10);
            else {
                http_fail("Invalid HTTP chunk size");
                return false;
            }
            if (value > (NET_HTTP_BODY_CAP - digit) / 16) {
                http_fail("HTTP chunk exceeds body capacity");
                return false;
            }
            value = value * 16 + digit;
        }
        if (!i || value > NET_HTTP_BODY_CAP - http_length) {
            http_fail("HTTP chunk exceeds body capacity");
            return false;
        }
        chunk_left = value;
        chunk_state = value ? 1 : 4;
        return true;
    }
    if (!c || (c < 32 && c != '\r' && c != '\t') || line_length + 1 >= sizeof header_line) {
        http_fail("Invalid or oversized HTTP line");
        return false;
    }
    if (line_length && header_line[line_length - 1] == '\r') {
        http_fail("Unexpected CR in HTTP line");
        return false;
    }
    header_line[line_length++] = (char)c;
    return true;
}
static bool http_consume(const uint8_t *p, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (body_complete)
            return true;
        if (!headers_done) {
            if (!consume_line(p[i], false))
                return false;
            continue;
        }
        if (chunked) {
            if (chunk_state == 0 || chunk_state == 4) {
                if (!consume_line(p[i], true))
                    return false;
            } else if (chunk_state == 1) {
                if (!append_body(p[i]))
                    return false;
                if (!--chunk_left)
                    chunk_state = 2;
            } else if (chunk_state == 2) {
                if (p[i] != '\r') {
                    http_fail("Missing chunk CRLF");
                    return false;
                }
                chunk_state = 3;
            } else {
                if (p[i] != '\n') {
                    http_fail("Missing chunk CRLF");
                    return false;
                }
                chunk_state = 0;
            }
        } else {
            if (!append_body(p[i]))
                return false;
            if (length_seen && http_length == content_length)
                http_complete();
        }
    }
    return true;
}
static void http_eof(void) {
    if (body_complete || http_state == NET_HTTP_ERROR)
        return;
    if (!headers_done || chunked || (length_seen && http_length != content_length))
        http_fail("HTTP response ended prematurely");
    else
        http_complete();
}

static bool tcp_emit(uint32_t sequence, unsigned flags, const uint8_t *data, size_t length) {
    uint8_t packet[1460] = {0};
    unsigned header = (flags & 2) ? 24 : 20;
    if (length > sizeof packet - header)
        return false;
    put16(packet, local_port);
    put16(packet + 2, http_port);
    put32(packet + 4, sequence);
    put32(packet + 8, (flags & 16) ? receive_next : 0);
    packet[12] = (uint8_t)((header / 4) << 4);
    packet[13] = (uint8_t)flags;
    put16(packet + 14, TCP_WINDOW);
    if (flags & 2) {
        packet[20] = 2;
        packet[21] = 4;
        put16(packet + 22, 1460);
    }
    if (length)
        memcpy(packet + header, data, length);
    put16(packet + 16, transport_sum(status.ipv4, remote_ip, 6, packet, header + length));
    return ipv4_send(remote_ip, 6, packet, header + length);
}
static void tcp_packet(uint32_t sequence, unsigned flags, const uint8_t *data, size_t length) {
    (void)tcp_emit(sequence, flags, data, length);
}
static void tcp_flight(unsigned flags, const uint8_t *data, size_t length) {
    if (length > sizeof flight_data)
        return;
    flight_sequence = send_next;
    flight_flags = (uint8_t)flags;
    flight_length = length;
    if (length)
        memcpy(flight_data, data, length);
    send_next += (uint32_t)length + ((flags & 2) ? 1 : 0) + ((flags & 1) ? 1 : 0);
    flight_active = true;
    flight_sent = false;
    flight_retries = 0;
    flight_deadline = 0;
}
static void tcp_connect(void) {
    tcp_open = true;
    tcp_established = tcp_peer_fin = tcp_fin_sent = false;
    flight_active = false;
    local_port = (uint16_t)(49152 + (++nonce & 0x3fff));
    send_una = send_next =
        0x41520000u ^ (uint32_t)(platform_ticks() * 317) ^ ((uint32_t)nonce << 16);
    receive_next = 0;
    peer_mss = 536;
    peer_window = TCP_WINDOW;
    tcp_last_activity = platform_ticks();
    http_state = NET_HTTP_CONNECTING;
    tcp_flight(2, 0, 0);
}
static bool tcp_options(const uint8_t *p, size_t header) {
    for (size_t at = 20; at < header;) {
        unsigned kind = p[at++];
        if (!kind)
            break;
        if (kind == 1)
            continue;
        if (at >= header)
            return false;
        unsigned n = p[at++];
        if (n < 2 || n - 2 > header - at)
            return false;
        if (kind == 2) {
            if (n != 4)
                return false;
            unsigned value = be16(p + at);
            if (!value)
                return false;
            if (p[13] & 2)
                peer_mss = (uint16_t)(value > TCP_DATA_CAP ? TCP_DATA_CAP : value);
        }
        at += n - 2;
    }
    return true;
}
static void tcp_receive(uint32_t source, const uint8_t *p, size_t length) {
    if (!tcp_open || source != remote_ip || length < 20 || be16(p) != http_port ||
        be16(p + 2) != local_port)
        return;
    unsigned header = (p[12] >> 4) * 4, flags = p[13];
    if (header < 20 || header > length || transport_sum(source, status.ipv4, 6, p, length))
        return;
    uint32_t seq = be32(p + 4), ack = be32(p + 8);
    size_t data_length = length - header;
    if (!tcp_options(p, header)) {
        http_fail("Invalid TCP option");
        tcp_open = false;
        return;
    }
    if (!tcp_established) {
        if ((flags & 4) && (flags & 16) && ack == send_next) {
            http_fail("TCP connection was refused");
            tcp_open = false;
            return;
        }
        if ((flags & 0x12) != 0x12 || (flags & 4) || ack != send_next)
            return;
        send_una = ack;
        flight_active = false;
        receive_next = seq + 1;
        peer_window = be16(p + 14);
        tcp_established = true;
        tcp_last_activity = platform_ticks();
        http_state = NET_HTTP_SENDING;
        tcp_packet(send_next, 16, 0, 0);
        if (!data_length && !(flags & 1))
            return;
        seq++;
        flags &= ~2u;
    } else {
        if (flags & 4) {
            if (seq == receive_next) {
                http_fail("TCP connection was reset");
                tcp_open = false;
            } else
                tcp_packet(send_next, 16, 0, 0);
            return;
        }
        if (flags & 2) {
            tcp_packet(send_next, 16, 0, 0);
            return;
        }
        if (!(flags & 16) || before(send_next, ack))
            return;
        if (!before(ack, send_una)) {
            send_una = ack;
            peer_window = be16(p + 14);
            if (ack == send_next)
                flight_active = false;
        }
    }
    if (before(receive_next, seq)) {
        tcp_packet(send_next, 16, 0, 0);
        return;
    }
    size_t skip = before(seq, receive_next) ? (uint32_t)(receive_next - seq) : 0;
    if (skip > data_length) {
        tcp_packet(send_next, 16, 0, 0);
        return;
    }
    if (data_length - skip > TCP_WINDOW) {
        tcp_packet(send_next, 16, 0, 0);
        return;
    }
    if (data_length > skip) {
        if (http_state != NET_HTTP_DONE && http_state != NET_HTTP_ERROR)
            http_state = NET_HTTP_RECEIVING;
        bool okay = https ? tls_receive(p + header + skip, data_length - skip, http_consume)
                          : http_consume(p + header + skip, data_length - skip);
        if (!okay && https && tls_failed())
            http_fail(tls_error());
        receive_next += (uint32_t)(data_length - skip);
        tcp_last_activity = platform_ticks();
        if (!okay) {
            tcp_packet(send_next, 0x14, 0, 0);
            tcp_open = false;
            return;
        }
    }
    if ((flags & 1) && seq + (uint32_t)data_length == receive_next) {
        receive_next++;
        tcp_peer_fin = true;
        tcp_last_activity = platform_ticks();
        http_eof();
    }
    if (data_length || (flags & 1))
        tcp_packet(send_next, 16, 0, 0);
    if (tcp_peer_fin && tcp_fin_sent && !flight_active) {
        tcp_close_deadline = platform_ticks() + 200;
    }
}

static bool dns_name(const uint8_t *p, size_t n, size_t *offset, char output[254]) {
    size_t at = *offset, written = 0;
    unsigned jumps = 0;
    bool redirected = false;
    for (unsigned labels = 0; labels < 128; labels++) {
        if (at >= n)
            return false;
        unsigned length = p[at++];
        if (!length) {
            if (!redirected)
                *offset = at;
            output[written] = 0;
            return true;
        }
        if ((length & 0xc0) == 0xc0) {
            if (at >= n || ++jumps > 16)
                return false;
            size_t next = ((length & 63) << 8) | p[at++];
            if (next >= n)
                return false;
            if (!redirected)
                *offset = at;
            redirected = true;
            at = next;
            continue;
        }
        if (length > 63 || length > n - at || written + length + (written ? 1 : 0) > 253)
            return false;
        if (written)
            output[written++] = '.';
        for (unsigned i = 0; i < length; i++) {
            if (p[at] < 33 || p[at] > 126)
                return false;
            output[written++] = (char)ascii_lower(p[at++]);
        }
    }
    return false;
}
static bool dns_send(void) {
    uint8_t packet[300] = {0};
    put16(packet, dns_id);
    put16(packet + 2, 0x0100);
    put16(packet + 4, 1);
    size_t at = 12;
    const char *name = http_host;
    while (*name) {
        const char *end = name;
        while (*end && *end != '.')
            end++;
        size_t len = (size_t)(end - name);
        if (!len || len > 63)
            return false;
        packet[at++] = (uint8_t)len;
        memcpy(packet + at, name, len);
        at += len;
        name = *end ? end + 1 : end;
    }
    packet[at++] = 0;
    put16(packet + at, 1);
    put16(packet + at + 2, 1);
    at += 4;
    return udp_send(status.dns, dns_port, NET_DNS_PORT, packet, at);
}
static void dns_receive(uint32_t source, const uint8_t *p, size_t n) {
    if (http_state != NET_HTTP_RESOLVING || source != status.dns || n < 12 || be16(p) != dns_id)
        return;
    unsigned flags = be16(p + 2);
    if (!(flags & 0x8000) || (flags & 0x7800) || be16(p + 4) != 1)
        return;
    size_t at = 12;
    char name[254];
    if (!dns_name(p, n, &at, name) || at + 4 > n || !insensitive(name, http_host) ||
        be16(p + at) != 1 || be16(p + at + 2) != 1)
        return;
    if (flags & 0x200) {
        http_fail("DNS reply requires unsupported TCP fallback");
        return;
    }
    if (flags & 15) {
        http_fail((flags & 15) == 3 ? "DNS name does not exist" : "DNS server returned an error");
        return;
    }
    at += 4;
    size_t answers = at;
    unsigned count = be16(p + 6);
    if (count > 64)
        return;
    char wanted[254];
    strcopy(wanted, http_host, sizeof wanted);
    for (unsigned depth = 0; depth < 8; depth++) {
        bool alias = false;
        at = answers;
        for (unsigned i = 0; i < count; i++) {
            if (!dns_name(p, n, &at, name) || at + 10 > n)
                return;
            unsigned type = be16(p + at), class = be16(p + at + 2), length = be16(p + at + 8);
            at += 10;
            if (length > n - at)
                return;
            if (class == 1 && insensitive(name, wanted)) {
                if (type == 1 && length == 4 && unicast(be32(p + at))) {
                    remote_ip = be32(p + at);
                    tcp_connect();
                    return;
                }
                if (type == 5) {
                    size_t pos = at;
                    if (!dns_name(p, n, &pos, wanted) || pos != at + length)
                        return;
                    alias = true;
                    break;
                }
            }
            at += length;
        }
        if (!alias) {
            http_fail("DNS reply has no usable IPv4 address");
            return;
        }
    }
    http_fail("DNS alias chain is too long");
}
static void receive_frame(const uint8_t *frame, size_t size) {
    if (size < 14 || (!equal(frame, status.mac, 6) && !equal(frame, broadcast_mac, 6))) {
        status.dropped_packets++;
        return;
    }
    unsigned type = be16(frame + 12);
    const uint8_t *p = frame + 14;
    size_t n = size - 14;
    if (type == 0x0806) {
        if (n < 28 || be16(p) != 1 || be16(p + 2) != 0x0800 || p[4] != 6 || p[5] != 4 ||
            !equal(p + 8, frame + 6, 6))
            return;
        unsigned operation = be16(p + 6);
        uint32_t source = be32(p + 14), target = be32(p + 24);
        if (operation != 1 && operation != 2)
            return;
        if (status.configured && source == status.ipv4 && !equal(p + 8, status.mac, 6)) {
            set_message("Duplicate IPv4 address detected; DHCP restarting");
            if (tcp_open)
                http_fail("Duplicate IPv4 address");
            tcp_open = false;
            dhcp_start();
            return;
        }
        if (target == status.ipv4 || source == arp_pending)
            neighbor_add(source, p + 8);
        if (operation == 1 && status.configured && target == status.ipv4)
            arp_send(source, true, p + 8);
        return;
    }
    if (type != 0x0800 || n < 20 || (p[0] >> 4) != 4)
        return;
    unsigned header = (p[0] & 15) * 4, total = be16(p + 2);
    if (header < 20 || header > n || total < header || total > n || checksum(p, header) ||
        (be16(p + 6) & 0xbfff)) {
        status.dropped_packets++;
        return;
    }
    uint32_t source = be32(p + 12), destination = be32(p + 16);
    unsigned protocol = p[9];
    const uint8_t *data = p + header;
    size_t length = total - header;
    bool dhcp = protocol == 17 && length >= 8 && be16(data) == 67 && be16(data + 2) == 68;
    if (destination != status.ipv4 && destination != 0xffffffffu && !dhcp)
        return;
    if (protocol == 17) {
        if (length < 8)
            return;
        unsigned udp_length = be16(data + 4);
        if (udp_length < 8 || udp_length > length)
            return;
        if (be16(data + 6) && transport_sum(source, destination, 17, data, udp_length))
            return;
        if (dhcp)
            dhcp_receive(source, data + 8, udp_length - 8);
        else if (status.configured && be16(data) == NET_DNS_PORT && be16(data + 2) == dns_port)
            dns_receive(source, data + 8, udp_length - 8);
    } else if (protocol == 6 && status.configured && destination == status.ipv4)
        tcp_receive(source, data, length);
    else if (protocol == 1 && status.configured && destination == status.ipv4 && length >= 8 &&
             !checksum(data, length) && data[0] == 8 && data[1] == 0) {
        uint8_t reply[1480];
        if (length > sizeof reply)
            return;
        memcpy(reply, data, length);
        reply[0] = 0;
        put16(reply + 2, 0);
        put16(reply + 2, checksum(reply, length));
        ipv4_send(source, 1, reply, length);
    }
}

bool net_http_get(const char *url) {
    net_http_cancel();
    http_reset_parser();
    request_length = request_offset = 0;
    remote_ip = 0;
    https = url && !strncmp(url, "https://", 8);
    if (!url || (!https && strncmp(url, "http://", 7))) {
        http_fail("请输入 http:// 或 https:// 开头的网站地址");
        return false;
    }
    const char *p = url + (https ? 8 : 7);
    size_t host_length = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        unsigned c = (uint8_t)*p++;
        if (host_length >= sizeof http_host - 1 ||
            !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-')) {
            http_fail("Invalid HTTP host name");
            return false;
        }
        http_host[host_length++] = (char)ascii_lower(c);
    }
    if (host_length && http_host[host_length - 1] == '.')
        host_length--;
    http_host[host_length] = 0;
    if (!host_length) {
        http_fail("HTTP host is empty");
        return false;
    }
    size_t label = 0;
    for (size_t i = 0; i <= host_length; i++) {
        if (!http_host[i] || http_host[i] == '.') {
            if (!label || label > 63) {
                http_fail("Invalid DNS host label");
                return false;
            }
            label = 0;
        } else
            label++;
    }
    http_port = https ? 443 : 80;
    if (*p == ':') {
        p++;
        unsigned port = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 5) {
                http_fail("Invalid HTTP port");
                return false;
            }
            port = port * 10 + (unsigned)(*p++ - '0');
        }
        if (!digits || !port || port > 65535) {
            http_fail("Invalid HTTP port");
            return false;
        }
        http_port = (uint16_t)port;
    }
    if (*p && *p != '/' && *p != '?' && *p != '#') {
        http_fail("Unexpected text after HTTP port");
        return false;
    }
    size_t path_length = 0;
    if (*p != '/')
        http_path[path_length++] = '/';
    while (*p && *p != '#') {
        unsigned c = (uint8_t)*p++;
        if (c <= 32 || c >= 127 || path_length >= sizeof http_path - 1) {
            http_fail("HTTP path is invalid or too long");
            return false;
        }
        http_path[path_length++] = (char)c;
    }
    http_path[path_length] = 0;
    const char *pieces[] = {"GET ", http_path, " HTTP/1.1\r\nHost: ", http_host};
    for (unsigned i = 0; i < 4; i++) {
        size_t n = strlen(pieces[i]);
        if (n >= sizeof http_request - request_length) {
            http_fail("HTTP request is too long");
            return false;
        }
        memcpy(http_request + request_length, pieces[i], n);
        request_length += n;
    }
    if (http_port != (https ? 443 : 80)) {
        char port[8];
        uint_to_str(http_port, port);
        http_request[request_length++] = ':';
        size_t n = strlen(port);
        memcpy(http_request + request_length, port, n);
        request_length += n;
    }
    const char *tail =
        "\r\nConnection: close\r\nAccept-Encoding: identity\r\nUser-Agent: ArkOS/0.5\r\n\r\n";
    size_t tail_length = strlen(tail);
    if (tail_length >= sizeof http_request - request_length) {
        http_fail("HTTP request is too long");
        return false;
    }
    memcpy(http_request + request_length, tail, tail_length);
    request_length += tail_length;
    if (https && !tls_begin(http_host)) {
        http_fail(tls_error());
        return false;
    }
    http_started = platform_ticks();
    http_state = NET_HTTP_WAIT_NETWORK;
    return true;
}
bool net_init(void) {
    memset(&status, 0, sizeof status);
    memset(neighbors, 0, sizeof neighbors);
    neighbor_next = 0;
    nonce = 0;
    http_state = NET_HTTP_IDLE;
    tcp_open = false;
    dhcp_phase = 0;
    link_check = 0;
    /* The NIC arrives through a loadable driver (.arco module): module_boot_load
     * runs after net_init, so the interface comes up in net_bind_nic. */
    set_message("waiting for a network driver");
    return true;
}
int net_bind_nic(const void *ops, unsigned owner) {
    if (!ops)
        return -22;
    const ArkNetOps *table = (const ArkNetOps *)ops;
    if (!table->send || !table->link || !table->receive || !table->read_mac)
        return -22;
    if (nic_bound)
        return -16;
    nic = *table;
    nic_owner = (int)owner;
    nic_bound = true;
    nic.read_mac(status.mac);
    status.present = true;
    status.link = nic.link() != 0;
    if (status.link)
        dhcp_start();
    else
        set_message("network adapter present; link is down");
    return 0;
}
void net_unbind_nic(unsigned owner) {
    if (!nic_bound || nic_owner != (int)owner)
        return;
    memset(&nic, 0, sizeof nic);
    nic_owner = -1;
    nic_bound = false;
    status.present = false;
    status.link = false;
    status.configured = false;
    status.ipv4 = status.mask = status.gateway = status.dns = 0;
    dhcp_phase = 0;
    if (tcp_open)
        http_fail("network driver removed");
    tcp_open = false;
    set_message("network driver removed");
}
void net_poll(void) {
    uint64_t now = platform_ticks();
    if (http_state > NET_HTTP_IDLE && http_state < NET_HTTP_DONE &&
        now - http_started >= HTTP_TIMEOUT) {
        http_fail("HTTP request timed out");
        if (tcp_open && tcp_established)
            tcp_packet(send_next, 0x14, 0, 0);
        tcp_open = false;
    }
    if (!status.present)
        return;
    if (now >= link_check) {
        link_check = now + 50;
        bool link = nic_bound && nic.link();
        if (link != status.link) {
            status.link = link;
            if (link)
                dhcp_start();
            else {
                status.configured = false;
                dhcp_phase = 0;
                set_message("Network cable/link disconnected");
                if (tcp_open)
                    http_fail("Network link was disconnected");
                tcp_open = false;
            }
        }
    }
    if (!status.link)
        return;
    for (unsigned budget = 0; budget < 32 && nic_bound; budget++) {
        size_t n = nic.receive(rx_frame, sizeof rx_frame);
        if (!n)
            break;
        status.rx_packets++;
        status.rx_bytes += n;
        receive_frame(rx_frame, n);
    }
    if (status.configured && now >= lease_expire) {
        if (tcp_open)
            http_fail("DHCP lease expired");
        tcp_open = false;
        dhcp_start();
    }
    if (dhcp_phase == 3 && now >= lease_renew) {
        dhcp_phase = 4;
        dhcp_attempt = 0;
        dhcp_deadline = 0;
    }
    if (dhcp_phase == 4 && now >= lease_rebind) {
        dhcp_phase = 5;
        dhcp_attempt = 0;
        dhcp_deadline = 0;
    }
    if ((dhcp_phase == 1 || dhcp_phase == 2 || dhcp_phase >= 4) && now >= dhcp_deadline) {
        if (dhcp_send()) {
            dhcp_deadline = now + (100u << (dhcp_attempt < 4 ? dhcp_attempt : 4));
            dhcp_attempt++;
        }
        if (dhcp_attempt > 8 && !status.configured) {
            set_message("DHCP timed out; retrying configuration");
            dhcp_start();
            dhcp_deadline = now + 500;
        }
    }
    if (http_state == NET_HTTP_WAIT_NETWORK && status.configured) {
        if (parse_ipv4(http_host, &remote_ip))
            tcp_connect();
        else if (!status.dns)
            http_fail("DHCP did not provide a DNS server");
        else {
            dns_id = (uint16_t)(++nonce ^ (uint16_t)now ^ 0x4152);
            dns_port = (uint16_t)(53000 + (++nonce & 8191));
            dns_attempt = 0;
            dns_deadline = 0;
            http_state = NET_HTTP_RESOLVING;
        }
    }
    if (http_state == NET_HTTP_RESOLVING && now >= dns_deadline) {
        if (dns_attempt >= 4)
            http_fail("DNS lookup timed out");
        else if (dns_send()) {
            dns_attempt++;
            dns_deadline = now + 150;
        }
    }
    if (!tcp_open)
        return;
    if (http_state == NET_HTTP_ERROR) {
        tcp_packet(send_next, 0x14, 0, 0);
        tcp_open = false;
        return;
    }
    if (flight_active && (!flight_sent || now >= flight_deadline)) {
        if (flight_sent && flight_retries >= 5) {
            if (!body_complete)
                http_fail("TCP retransmission limit reached");
            tcp_open = false;
            return;
        }
        if (tcp_emit(flight_sequence, flight_flags, flight_data, flight_length)) {
            if (flight_sent)
                flight_retries++;
            flight_sent = true;
            flight_deadline = now + (100u << (flight_retries < 3 ? flight_retries : 3));
        }
    }
    if (tcp_established && !flight_active) {
        if (https) {
            if (request_offset < request_length && !tcp_peer_fin && !body_complete)
                request_offset += tls_write((const uint8_t *)http_request + request_offset,
                                            request_length - request_offset);
            if (body_complete && !https_closing) {
                tls_close();
                https_closing = true;
            }
            size_t n = 0;
            const uint8_t *p = tls_output(&n);
            if (tls_failed()) {
                http_fail(tls_error());
                tcp_packet(send_next, 0x14, 0, 0);
                tcp_open = false;
                return;
            }
            if (p && n) {
                if (n > peer_mss)
                    n = peer_mss;
                if (n > peer_window)
                    n = peer_window;
                if (n) {
                    tcp_flight(0x18, p, n);
                    tls_output_ack(n);
                }
                return;
            }
        } else if (request_offset < request_length && !tcp_peer_fin && !body_complete) {
            size_t length = request_length - request_offset;
            if (length > peer_mss)
                length = peer_mss;
            if (length > peer_window)
                length = peer_window;
            if (length) {
                tcp_flight(0x18, (const uint8_t *)http_request + request_offset, length);
                request_offset += length;
            }
        }
        if ((body_complete || tcp_peer_fin) && !tcp_fin_sent) {
            tcp_fin_sent = true;
            tcp_flight(0x11, 0, 0);
            tcp_close_deadline = now + 500;
        } else if (tcp_fin_sent && now >= tcp_close_deadline)
            tcp_open = false;
    }
    if (!body_complete && now - tcp_last_activity >= 2000) {
        http_fail("TCP peer stopped responding");
        tcp_packet(send_next, 0x14, 0, 0);
        tcp_open = false;
    }
}
const NetStatus *net_status(void) {
    return &status;
}
NetHttpState net_http_state(void) {
    return http_state;
}
const char *net_http_body(void) {
    return http_body;
}
size_t net_http_length(void) {
    return http_length;
}
unsigned net_http_status(void) {
    return http_code;
}
const char *net_http_error(void) {
    return http_error;
}
const char *net_http_content_type(void) {
    return http_type;
}

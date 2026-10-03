/* Sanitized hosted wire/parser boundary tests; never substitutes for VM DMA. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../kernel/net.c"
/* This harness exercises plaintext wire parsing; native TLS is covered by
 * tls_vm_test.py with real encrypted traffic and certificate failures. */
void tls_cancel(void) {
}
bool tls_begin(const char *h) {
    (void)h;
    return false;
}
bool tls_receive(const uint8_t *p, size_t n, bool (*consume)(const uint8_t *, size_t)) {
    (void)p;
    (void)n;
    (void)consume;
    return false;
}
size_t tls_write(const uint8_t *p, size_t n) {
    (void)p;
    (void)n;
    return 0;
}
const uint8_t *tls_output(size_t *n) {
    *n = 0;
    return 0;
}
void tls_output_ack(size_t n) {
    (void)n;
}
void tls_close(void) {
}
bool tls_failed(void) {
    return false;
}
const char *tls_error(void) {
    return "TLS unavailable in plaintext harness";
}
static uint64_t fake_ticks;
uint64_t platform_ticks(void) {
    return fake_ticks;
}
void serial_write(const char *s) {
    (void)s;
}
void strcopy(char *d, const char *s, size_t cap) {
    if (!cap)
        return;
    size_t n = strlen(s);
    if (n >= cap)
        n = cap - 1;
    memcpy(d, s, n);
    d[n] = 0;
}
void uint_to_str(uint64_t n, char *out) {
    sprintf(out, "%llu", (unsigned long long)n);
}
bool e1000_init(uint8_t mac[6]) {
    memcpy(mac, "\x52\x54\x00\x12\x34\x56", 6);
    return true;
}
bool e1000_link(void) {
    return true;
}
bool e1000_send(const uint8_t *p, size_t n) {
    (void)p;
    (void)n;
    return true;
}
size_t e1000_receive(uint8_t *p, size_t n) {
    (void)p;
    (void)n;
    return 0;
}
static void parse(const char *wire, bool okay, const char *body) {
    http_reset_parser();
    http_state = NET_HTTP_RECEIVING;
    bool result = true;
    for (size_t i = 0; wire[i] && result; i++)
        result = http_consume((const uint8_t *)wire + i, 1);
    http_eof();
    assert(okay ? (result && http_state == NET_HTTP_DONE) : http_state == NET_HTTP_ERROR);
    if (body) {
        assert(http_length == strlen(body));
        assert(!memcmp(body, http_body, http_length));
    }
}
static void tcp_test_segment(uint32_t sequence, const char *payload, bool corrupt) {
    uint8_t wire[512] = {0};
    size_t n = strlen(payload);
    put16(wire, http_port);
    put16(wire + 2, local_port);
    put32(wire + 4, sequence);
    put32(wire + 8, send_next);
    wire[12] = 0x50;
    wire[13] = 0x18;
    put16(wire + 14, 8192);
    memcpy(wire + 20, payload, n);
    put16(wire + 16, transport_sum(remote_ip, status.ipv4, 6, wire, n + 20));
    if (corrupt)
        wire[16] ^= 1;
    tcp_receive(remote_ip, wire, n + 20);
}
static void wire_state_tests(void) {
    memset(&status, 0, sizeof status);
    status.configured = true;
    status.ipv4 = 0x0a00020f;
    status.dns = 0x0a000202;
    remote_ip = status.dns;
    http_port = 80;
    local_port = 49152;
    send_next = send_una = 1000;
    receive_next = 100;
    tcp_open = tcp_established = true;
    http_reset_parser();
    http_state = NET_HTTP_RECEIVING;
    const char *first = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na";
    tcp_test_segment(105, first, false);
    assert(receive_next == 100 && http_length == 0);
    tcp_test_segment(100, first, true);
    assert(receive_next == 100 && http_length == 0);
    tcp_test_segment(100, first, false);
    assert(receive_next == 100 + strlen(first) && http_length == 1);
    tcp_test_segment(100, first, false);
    assert(http_length == 1); /* exact duplicate */
    tcp_test_segment(receive_next - 1, "abc", false);
    assert(http_state == NET_HTTP_DONE && http_length == 3 && !strcmp(http_body, "abc"));
    http_fail("late close failure");
    assert(http_state == NET_HTTP_DONE);
    assert(before(0xfffffff0u, 3) && !before(3, 0xfffffff0u));
    /* Matching DNS transaction with forged question must not fail our lookup. */
    uint8_t reply[128] = {0};
    http_state = NET_HTTP_RESOLVING;
    dns_id = 123;
    strcopy(http_host, "arkos.test", sizeof http_host);
    put16(reply, 123);
    put16(reply + 2, 0x8183);
    put16(reply + 4, 1);
    reply[12] = 1;
    reply[13] = 'x';
    reply[14] = 0;
    put16(reply + 15, 1);
    put16(reply + 17, 1);
    dns_receive(status.dns, reply, 19);
    assert(http_state == NET_HTTP_RESOLVING);
    /* Valid compressed answer resolves to the actual selected IPv4 address. */
    put16(reply + 2, 0x8180);
    put16(reply + 6, 1);
    reply[12] = 5;
    memcpy(reply + 13, "arkos", 5);
    reply[18] = 4;
    memcpy(reply + 19, "test", 4);
    reply[23] = 0;
    put16(reply + 24, 1);
    put16(reply + 26, 1);
    reply[28] = 0xc0;
    reply[29] = 12;
    put16(reply + 30, 1);
    put16(reply + 32, 1);
    put32(reply + 34, 60);
    put16(reply + 38, 4);
    put32(reply + 40, 0x0a000202);
    dns_receive(status.dns, reply, 44);
    assert(http_state == NET_HTTP_CONNECTING && remote_ip == 0x0a000202);
}
static uint32_t random_state = 0xdeadbeef;
static unsigned random32(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
int main(void) {
    wire_state_tests();
    tcp_open = false;
    parse("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\n你好", true, "你好");
    parse("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: "
          "chunked\r\n\r\n3;x=y\r\nabc\r\n2\r\nde\r\n0\r\nX-Test: okay\r\n\r\n",
          true, "abcde");
    parse("HTTP/1.0 404 Missing\r\n\r\nMissing", true, "Missing");
    parse("HTTP/1.1 204 Empty\r\n\r\n", true, "");
    parse("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nshort", false, 0);
    parse("HTTP/1.1 200 OK\r\nContent-Length: 16385\r\n\r\n", false, 0);
    parse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n", false, 0);
    parse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n", false, 0);
    parse("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n", false, 0);
    parse("HTTP/1.1 200 OK\n\n", false, 0);
    parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc!\n", false, 0);
    parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFF\r\n", false, 0);
    assert(!net_http_get("http://a:80junk/"));
    assert(!net_http_get("https://arkos.test/"));
    assert(!net_http_get("http://a:0/"));
    assert(!net_http_get("http://a:65536/"));
    assert(!net_http_get("http://a/x\r\nInjected: yes"));
    assert(!net_http_get("http://a..test/"));
    char url[1600];
    memset(url, 'x', sizeof url);
    memcpy(url, "http://host/", 12);
    url[sizeof url - 1] = 0;
    assert(!net_http_get(url));
    assert(net_http_get("http://host:8080/a?x=y#fragment"));
    assert(!strcmp(http_path, "/a?x=y"));
    assert(http_port == 8080);
    status.present = false;
    fake_ticks = HTTP_TIMEOUT + 1;
    net_poll();
    assert(net_http_state() == NET_HTTP_ERROR);
    uint8_t loop[] = {0xc0, 0};
    char name[254];
    size_t at = 0;
    assert(!dns_name(loop, sizeof loop, &at, name));
    uint8_t compressed[] = {1, 'a', 0, 0xc0, 0};
    at = 3;
    assert(dns_name(compressed, sizeof compressed, &at, name));
    assert(at == 5 && !strcmp(name, "a"));
    uint8_t packet[2048];
    for (unsigned i = 0; i < 12000; i++) {
        size_t n = random32() % sizeof packet;
        for (size_t j = 0; j < n; j++)
            packet[j] = (uint8_t)random32();
        at = 0;
        (void)dns_name(packet, n, &at, name);
        http_reset_parser();
        http_state = NET_HTTP_RECEIVING;
        (void)http_consume(packet, n);
        http_eof();
        memset(&status, 0, sizeof status);
        memcpy(status.mac, "\x52\x54\x00\x12\x34\x56", 6);
        status.configured = true;
        status.ipv4 = 0x0a00020f;
        if (n >= 14)
            memcpy(packet, status.mac, 6);
        receive_frame(packet, n);
    }
    /* Exact 16KiB acceptance and one-byte overflow for connection-close body. */
    const char *header = "HTTP/1.1 200 OK\r\n\r\n";
    http_reset_parser();
    http_state = NET_HTTP_RECEIVING;
    assert(http_consume((const uint8_t *)header, strlen(header)));
    memset(packet, 'A', sizeof packet);
    for (int i = 0; i < 8; i++)
        assert(http_consume(packet, sizeof packet));
    assert(http_length == NET_HTTP_BODY_CAP);
    assert(!http_consume(packet, 1));
    puts("PASS HTTP incremental framing, cap, URL validation, offline timeout, DNS compression "
         "bounds, 12000 malformed wire cases (ASan/UBSan)");
    return 0;
}

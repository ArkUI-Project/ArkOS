/* Isolated real-DMA network diagnostic. Include permits test-only resolver
 * override to the controlled local fixture, never modifies release defaults. */
#define NET_DNS_PORT 5353u
#include "../kernel/net.c"
static BootInfo boot;
static void require(bool ok, const char *why) {
    if (!ok) {
        serial_write("[net-test] FAIL ");
        serial_write(why);
        serial_write("\n");
        for (;;)
            platform_idle();
    }
}
static void get(const char *url, unsigned code, const char *body) {
    require(net_http_get(url), "GET accepted");
    uint64_t until = platform_ticks() + 3500;
    while (net_http_state() != NET_HTTP_DONE && net_http_state() != NET_HTTP_ERROR &&
           platform_ticks() < until) {
        net_poll();
        platform_idle();
    }
    if (net_http_state() != NET_HTTP_DONE)
        serial_write(net_http_error());
    require(net_http_state() == NET_HTTP_DONE, "GET completion");
    require(net_http_status() == code, "HTTP status");
    require(net_http_length() == strlen(body) && !strcmp(net_http_body(), body),
            "body bytes exact");
    serial_write("[net-test] GET passed ");
    serial_write(url);
    serial_write("\n");
}
void kernel_main(uint32_t magic, uint32_t info) {
    platform_init(magic, info, &boot);
    require(net_init(), "e1000 PCI init");
    uint64_t until = platform_ticks() + 1500;
    while (!net_status()->configured && platform_ticks() < until) {
        net_poll();
        platform_idle();
    }
    require(net_status()->configured, "DHCP address acquired");
    require(net_status()->ipv4 == 0x0a00020f, "slirp DHCP address");
    get("http://10.0.2.2:8080/plain", 200,
        "ArkOS native HTTP: real DMA packets.\nChinese: 你好，网络。\n");
    get("http://10.0.2.2:8080/chunked", 200, "Chunked response: 你好\n");
    get("http://10.0.2.2:8080/missing", 404, "Controlled missing page\n");
    /* Only the diagnostic directs DNS to our host fixture (UDP5353). Production
     * continues to use DHCP-provided DNS; all DNS traffic here is real Ethernet. */
    status.dns = 0x0a000202;
    get("http://arkos.test:8080/plain", 200,
        "ArkOS native HTTP: real DMA packets.\nChinese: 你好，网络。\n");
    require(net_http_get("http://10.0.2.2:8080/oversize"), "oversize request");
    until = platform_ticks() + 1500;
    while (net_http_state() != NET_HTTP_ERROR && platform_ticks() < until) {
        net_poll();
        platform_idle();
    }
    require(net_http_state() == NET_HTTP_ERROR, "oversize rejected");
    require(!net_http_get("https://example.com/"), "TLS explicitly rejected");
    require(net_status()->rx_packets > 15 && net_status()->tx_packets > 15,
            "real traffic counters");
    number_log("[net-test] PASS rx=", net_status()->rx_packets);
    number_log(" tx=", net_status()->tx_packets);
    serial_write(" DHCP ARP IPv4 UDP DNS TCP HTTP chunked UTF-8 cap TLS-rejection\n");
    for (;;)
        platform_idle();
}

/* Only this isolated diagnostic trusts its generated private CA and resolver.
 * Production TLS uses the pinned public anchors and DHCP DNS. */
#define NET_DNS_PORT 55353u
#include "../kernel/net.c"
static BootInfo boot;
static void require(bool okay, const char *why) {
    if (!okay) {
        serial_write("[tls-test] FAIL ");
        serial_write(why);
        serial_write("\n");
        for (;;)
            platform_idle();
    }
}
static bool contains(const char *s, const char *part) {
    while (*s) {
        if (!strncmp(s, part, strlen(part)))
            return true;
        s++;
    }
    return false;
}
static void request(const char *url, bool okay, const char *reason) {
    require(net_http_get(url), "request start");
    uint64_t until = platform_ticks() + 3000;
    while (net_http_state() != NET_HTTP_DONE && net_http_state() != NET_HTTP_ERROR &&
           platform_ticks() < until) {
        net_poll();
        platform_idle();
    }
    if (okay) {
        serial_write(net_http_error());
        require(net_http_state() == NET_HTTP_DONE && net_http_status() == 200, "HTTPS completion");
        require(!strcmp(net_http_body(), "Native TLS authenticated body\n"),
                "decrypted exact bytes");
    } else {
        require(net_http_state() == NET_HTTP_ERROR, "invalid connection rejected");
        require(net_http_length() == 0, "no unauthenticated body exposed");
        require(contains(net_http_error(), reason), "specific rejection");
    }
    serial_write("[tls-test] checked ");
    serial_write(url);
    serial_write("\n");
}
void kernel_main(uint32_t magic, uint32_t info) {
    platform_init(magic, info, &boot);
    require(net_init(), "e1000 init");
    uint64_t until = platform_ticks() + 2000;
    while (!net_status()->configured && platform_ticks() < until) {
        net_poll();
        platform_idle();
    }
    require(net_status()->configured, "DHCP");
    status.dns = 0x0a000202;
    request("https://arkos.test:8443/plain", true, "");
    request("https://arkos.test:8444/wrong", false, "地址不匹配");
    request("https://arkos.test:8445/expired", false, "已过期");
    request("https://arkos.test:8446/untrusted", false, "无法验证");
    request("https://arkos.test:8447/tampered", false, "安全连接失败");
    request("http://arkos.test:8080/plain", true, "");
    require(net_status()->rx_packets > 30 && net_status()->tx_packets > 30, "real DMA packets");
    serial_write("[tls-test] PASS TLS1.2 ECDHE AES-GCM exact-body hostname expiry trust tamper "
                 "HTTP-fallback\n");
    for (;;)
        platform_idle();
}

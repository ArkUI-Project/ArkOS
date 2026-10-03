#ifndef ARK_NET_H
#define ARK_NET_H
#include "ark.h"

#define NET_HTTP_BODY_CAP 16384u
/* Addresses are host integers in dotted order: 10.0.2.15 == 0x0a00020f. */
typedef struct {
    bool present, link, configured;
    uint8_t mac[6];
    uint32_t ipv4, mask, gateway, dns;
    uint64_t rx_packets, tx_packets, dropped_packets, rx_bytes, tx_bytes;
    char message[96];
} NetStatus;
typedef enum {
    NET_HTTP_IDLE,
    NET_HTTP_WAIT_NETWORK,
    NET_HTTP_RESOLVING,
    NET_HTTP_CONNECTING,
    NET_HTTP_SENDING,
    NET_HTTP_RECEIVING,
    NET_HTTP_DONE,
    NET_HTTP_ERROR
} NetHttpState;

bool net_init(void);
/* Nonblocking, bounded packet processing. Call from the kernel service loop. */
void net_poll(void);
const NetStatus *net_status(void);
void net_format_ipv4(uint32_t address, char output[16]);
/* One HTTP/1.x GET at a time, http:// and validated https://. A new GET cancels the old one.
 * Valid URLs may wait for DHCP. False means a synchronous validation error.
 * DONE also exposes non-2xx HTTP status. Redirects are not followed silently.
 * Returned bytes are length-delimited and additionally NUL-terminated.
 */
bool net_http_get(const char *url);
void net_http_cancel(void);
NetHttpState net_http_state(void);
const char *net_http_body(void);
size_t net_http_length(void);
unsigned net_http_status(void);
const char *net_http_error(void);
const char *net_http_content_type(void);

#endif

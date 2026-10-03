#ifndef ARK_TLS_H
#define ARK_TLS_H
#include "ark.h"
bool tls_begin(const char *hostname);
void tls_cancel(void);
bool tls_receive(const uint8_t *, size_t, bool (*consume)(const uint8_t *, size_t));
size_t tls_write(const uint8_t *, size_t);
const uint8_t *tls_output(size_t *length);
void tls_output_ack(size_t);
void tls_close(void);
bool tls_failed(void);
const char *tls_error(void);
bool platform_secure_random(void *output, size_t length);
#endif

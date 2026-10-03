/* Original nonblocking ArkOS adapter, BearSSL 0.6 executes in the guest. */
#include "tls.h"
#include "../third_party/bearssl/inc/bearssl.h"
#ifdef ARK_TLS_TEST_ANCHORS
#include ARK_TLS_TEST_ANCHORS
#else
#include "tls_anchors.h"
#endif
static br_ssl_client_context client;
static br_x509_minimal_context verifier;
static unsigned char buffers[BR_SSL_BUFSIZE_BIDI];
static bool started, failed, closing;
static char error[96];
static void tls_fail(const char *s) {
    strcopy(error, s, sizeof error);
    failed = true;
}
void tls_cancel(void) {
    started = failed = closing = false;
    error[0] = 0;
    memset(&client, 0, sizeof client);
    memset(buffers, 0, sizeof buffers);
}
static bool check(void) {
    if (!started || failed)
        return false;
    int code = br_ssl_engine_last_error(&client.eng);
    if (code) {
        if (code == BR_ERR_X509_BAD_SERVER_NAME)
            tls_fail("网站证书与地址不匹配");
        else if (code == BR_ERR_X509_EXPIRED)
            tls_fail("网站证书已过期或尚未生效");
        else if (code >= BR_ERR_X509_INVALID_VALUE)
            tls_fail("无法验证网站证书，请检查地址与系统时间");
        else
            tls_fail("安全连接失败，网站返回了无效数据");
        return false;
    }
    return true;
}
bool tls_begin(const char *host) {
    tls_cancel();
    uint8_t seed[32];
    if (!platform_secure_random(seed, sizeof seed)) {
        tls_fail("设备暂时无法提供安全连接所需的随机数据");
        return false;
    }
    int year, month, day, hour, minute, second, weekday;
    if (!platform_datetime(&year, &month, &day, &hour, &minute, &second, &weekday) || year < 2020) {
        memset(seed, 0, sizeof seed);
        tls_fail("请先校准系统日期，再访问安全网站");
        return false;
    }
    br_ssl_client_init_full(&client, &verifier, TAs, TAs_NUM);
    br_ssl_engine_set_versions(&client.eng, BR_TLS12, BR_TLS12);
    static const uint16_t suites[] = {BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
                                      BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
                                      BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
                                      BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
                                      BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
                                      BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384};
    br_ssl_engine_set_suites(&client.eng, suites, sizeof suites / sizeof suites[0]);
    br_ssl_engine_add_flags(&client.eng, BR_OPT_NO_RENEGOTIATION);
    br_ssl_engine_set_buffer(&client.eng, buffers, sizeof buffers, 1);
    /* BearSSL epoch is 0000-01-01. Count complete years, including year zero. */
    uint32_t days =
        (uint32_t)(365 * year + (year + 3) / 4 - (year + 99) / 100 + (year + 399) / 400 + day - 1);
    static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m < month; m++)
        days +=
            (unsigned)md[m - 1] + (m == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    br_x509_minimal_set_time(&verifier, days, (uint32_t)(hour * 3600 + minute * 60 + second));
    br_x509_minimal_set_minrsa(&verifier, 256);
    br_ssl_engine_inject_entropy(&client.eng, seed, sizeof seed);
    memset(seed, 0, sizeof seed);
    if (!br_ssl_client_reset(&client, host, 0)) {
        tls_fail("无法开始安全连接");
        return false;
    }
    started = true;
    return check();
}
static bool drain(bool (*consume)(const uint8_t *, size_t)) {
    for (unsigned budget = 0; budget < 32; budget++) {
        size_t n;
        unsigned char *p = br_ssl_engine_recvapp_buf(&client.eng, &n);
        if (!p || !n)
            break;
        if (!consume(p, n))
            return false;
        br_ssl_engine_recvapp_ack(&client.eng, n);
    }
    return check();
}
bool tls_receive(const uint8_t *input, size_t length, bool (*consume)(const uint8_t *, size_t)) {
    if (!check())
        return false;
    while (length) {
        size_t n;
        unsigned char *p = br_ssl_engine_recvrec_buf(&client.eng, &n);
        if (!p || !n) {
            tls_fail("安全连接接收状态异常");
            return false;
        }
        if (n > length)
            n = length;
        memcpy(p, input, n);
        input += n;
        length -= n;
        br_ssl_engine_recvrec_ack(&client.eng, n);
        if (!drain(consume))
            return false;
    }
    return true;
}
size_t tls_write(const uint8_t *input, size_t length) {
    if (!check() || closing)
        return 0;
    size_t n;
    unsigned char *p = br_ssl_engine_sendapp_buf(&client.eng, &n);
    if (!p || !n)
        return 0;
    if (n > length)
        n = length;
    memcpy(p, input, n);
    br_ssl_engine_sendapp_ack(&client.eng, n);
    br_ssl_engine_flush(&client.eng, 0);
    return n;
}
const uint8_t *tls_output(size_t *length) {
    *length = 0;
    if (!check())
        return 0;
    return br_ssl_engine_sendrec_buf(&client.eng, length);
}
void tls_output_ack(size_t n) {
    br_ssl_engine_sendrec_ack(&client.eng, n);
    (void)check();
}
void tls_close(void) {
    if (started && !closing) {
        closing = true;
        br_ssl_engine_close(&client.eng);
    }
}
bool tls_failed(void) {
    (void)check();
    return failed;
}
const char *tls_error(void) {
    return error;
}

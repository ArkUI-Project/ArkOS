#ifndef ARK_SDK_CONFIG_H
#define ARK_SDK_CONFIG_H
#include "app.h"
static inline bool app_config_get(const char *key, void *value, size_t bytes) {
    ArkRegistryRequest q = {.op = ARK_REG_GET};
    strcopy(q.key, key, 128);
    if (ark_registry(&q) < 0 || q.type != ARK_REG_BINARY || q.length != bytes)
        return false;
    memcpy(value, q.value, bytes);
    return true;
}
static inline bool app_config_set(const char *key, const void *value, size_t bytes) {
    if (bytes > 512)
        return false;
    ArkRegistryRequest q = {.op = ARK_REG_SET, .type = ARK_REG_BINARY, .length = (uint32_t)bytes};
    strcopy(q.key, key, 128);
    memcpy(q.value, value, bytes);
    return ark_registry(&q) >= 0;
}
#endif

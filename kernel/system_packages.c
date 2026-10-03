/* Read-only system packages inherit trust from the boot kernel image.
 * Disk packages, even byte-identical copies, never gain this privilege. */
#define ARK_KERNEL
#include "package.h"
#include "ark_catalog.h"
#include "sha256.h"
#include "elf.h"
#include "permissions.h"
#include "accounts.h"
extern const uint8_t ark_system_package_headers[];
extern const uint8_t _binary_build_user_desktop_elf_start[], _binary_build_user_desktop_elf_end[];
#define DECLARE(p, t, l, d, m, g)                                                                  \
    extern const uint8_t _binary_build_apps_##p##_elf_start[], _binary_build_apps_##p##_elf_end[];
ARK_CATALOG(DECLARE)
#undef DECLARE
#define ROW(p, t, l, d, m, g)                                                                      \
    {_binary_build_apps_##p##_elf_start, _binary_build_apps_##p##_elf_end},
static const struct {
    const uint8_t *first, *last;
} images[ARK_CATALOG_COUNT] = {ARK_CATALOG(ROW)};
#undef ROW
static uint8_t verified[ARK_PACKAGE_DESKTOP_FIRST];
static uint32_t rd(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}
static bool verify(unsigned app) {
    if (app >= ARK_PACKAGE_DESKTOP_FIRST)
        return false;
    if (verified[app])
        return verified[app] == 1;
    const uint8_t *h = ark_system_package_headers + app * 256, *first, *last;
    int native = ark_catalog_desktop((int)app);
    if (native < 0) {
        first = _binary_build_user_desktop_elf_start;
        last = _binary_build_user_desktop_elf_end;
    } else {
        first = images[native].first;
        last = images[native].last;
    }
    size_t bytes = (size_t)(last - first);
    uint8_t sha[32];
    ark_sha256(first, bytes, sha);
    bool ok = !memcmp(h, "ARKPKG1\0", 8) && h[8] == 1 && h[10] == 62 && rd(h + 12) == 256 &&
              rd(h + 16) == bytes + 256 && rd(h + 20) == ARK_ABI_VERSION && rd(h + 40) == bytes &&
              rd(h + 44) == (native < 0 ? (2u | (app << 8)) : 1u) &&
              rd(h + 24) == (native < 0 ? 31u : ark_catalog[native].maximum) &&
              rd(h + 252) == crc(h, 252) && !memcmp(sha, h + 208, 32) &&
              process_validate_elf(first, bytes,
                                   native < 0 ? ARK_CAP_SYSTEM : ark_catalog[native].maximum);
    verified[app] = ok ? 1 : 2;
    return ok;
}
unsigned package_system_count(void) {
    return ARK_PACKAGE_DESKTOP_FIRST;
}
bool package_system_ready(void) {
    for (unsigned i = 0; i < ARK_PACKAGE_DESKTOP_FIRST; i++)
        if (!verify(i))
            return false;
    return true;
}
int64_t package_system_info(unsigned app, ArkPackageInfo *out) {
    if (!verify(app))
        return -5;
    const uint8_t *h = ark_system_package_headers + app * 256;
    memset(out, 0, sizeof *out);
    out->slot = app;
    out->installed = 2;
    out->major = rd(h + 28);
    out->minor = rd(h + 32);
    out->patch = rd(h + 36);
    out->maximum = rd(h + 24);
    out->bytes = rd(h + 16);
    int native = ark_catalog_desktop((int)app);
    out->grants =
        native < 0 ? 31u : (uint32_t)permissions_caps((unsigned)native, accounts_current_uid());
    strcopy(out->id, (const char *)h + 48, sizeof out->id);
    strcopy(out->title, (const char *)h + 80, sizeof out->title);
    strcopy(out->summary, (const char *)h + 144, sizeof out->summary);
    ark_sha256(h, 256, out->manifest_sha256);
    return 0;
}
bool package_system_find(const char *id, ArkPackageInfo *info) {
    for (unsigned i = 0; i < ARK_PACKAGE_DESKTOP_FIRST; i++)
        if (!strcmp(id, (const char *)ark_system_package_headers + i * 256 + 48))
            return package_system_info(i, info) >= 0;
    return false;
}
const uint8_t *package_system_image(unsigned catalog, size_t *bytes) {
    if (catalog >= ARK_CATALOG_COUNT || !verify(ark_catalog[catalog].desktop))
        return 0;
    *bytes = (size_t)(images[catalog].last - images[catalog].first);
    return images[catalog].first;
}

#ifndef ARK_PACKAGE_H
#define ARK_PACKAGE_H
#include "ark_api.h"
#include <stdbool.h>
#define ARK_PACKAGE_HEADER 256u
#define ARK_PACKAGE_MAX (ARK_BLOB_MAX - 32u)
#define ARK_PACKAGE_DESKTOP_FIRST 19u
#define ARK_PACKAGE_PERMISSION_FIRST 256u
#define ARK_PACKAGE_CAPS                                                                            \
    (ARK_CAP_UI | ARK_CAP_FILES | ARK_CAP_NETWORK | ARK_CAP_ACTIVITY | ARK_CAP_DEVICE)
enum {
    ARK_PACKAGE_LIST,
    ARK_PACKAGE_INSPECT,
    ARK_PACKAGE_INSTALL,
    ARK_PACKAGE_UPGRADE,
    ARK_PACKAGE_REMOVE,
    ARK_PACKAGE_GRANTS,
    ARK_PACKAGE_FIND
};
typedef struct {
    uint32_t slot, installed, major, minor, patch, maximum, grants, bytes, pid;
    char id[32], title[64], summary[64];
    uint8_t manifest_sha256[32];
} ArkPackageInfo;
typedef struct {
    uint32_t op, index, grants, reserved;
    char path[128], id[32], error[128];
    uint8_t expected_manifest_sha256[32];
    ArkPackageInfo info;
} ArkPackageRequest;
/* Management requires SYSTEM and an active session. Sources are scoped local
 * paths or blob:NAME. Untrusted packages can never acquire SYSTEM/PROCESS. */
#ifdef ARK_KERNEL
#ifdef ARK_PACKAGE_HOST_TEST
static inline unsigned package_system_count(void) {
    return 0;
}
static inline int64_t package_system_info(unsigned n, ArkPackageInfo *p) {
    (void)n;
    (void)p;
    return -2;
}
static inline bool package_system_find(const char *id, ArkPackageInfo *p) {
    (void)id;
    (void)p;
    return false;
}
#else
unsigned package_system_count(void);
int64_t package_system_info(unsigned, ArkPackageInfo *);
bool package_system_find(const char *, ArkPackageInfo *);
const uint8_t *package_system_image(unsigned, size_t *);
bool package_system_ready(void);
#endif
int64_t package_request(ArkPackageRequest *request);
int64_t package_spawn(ArkSpawn *request);
int64_t package_launch_info(uint32_t pid, ArkLaunchInfo *info);
void package_process_exit(uint32_t pid);
int64_t package_permissions(ArkPermissionRequest *request);
int package_activity_identity(uint32_t pid, ArkActivityRequest *request);
#else
static inline int64_t ark_package(ArkPackageRequest *p) {
    return ark_call(ARK_SYS_PACKAGE, p, sizeof(*p));
}
#endif
#endif

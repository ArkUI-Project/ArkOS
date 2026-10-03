#ifndef ARK_SDK_FILES_H
#define ARK_SDK_FILES_H
#include "ark_api.h"
/* Binary objects are separate from text FILE. Names: 1..63 UTF-8 bytes,
 * no slash/backslash/control characters. Capacity <=8 MiB; 16 global slots.
 * Scope comes from the kernel's authenticated UID, never the request. */
static inline int64_t ark_blob(ArkBlobRequest *q) {
    return ark_call(ARK_SYS_BLOB, q, sizeof *q);
}
static inline int64_t ark_permissions(ArkPermissionRequest *q) {
    return ark_call(ARK_SYS_PERMISSION, q, sizeof *q);
}
static inline int64_t ark_install(ArkInstallRequest *q) {
    return ark_call(ARK_SYS_INSTALL, q, sizeof *q);
}
#endif

#ifndef ARK_SDK_PERMISSIONS_H
#define ARK_SDK_PERMISSIONS_H
#include "ark_api.h"
#include <stdbool.h>
/* 0 granted; -11 awaiting trusted UI; -1 denied for this app lifetime.
 * Does not invent authority or grant capabilities inside the application. */
static inline int64_t app_request_permission(unsigned caps) {
    ArkPermissionRequest r = {0};
    r.op = ARK_PERMISSION_REQUEST;
    r.grants = caps;
    return ark_call(ARK_SYS_PERMISSION, &r, sizeof r);
}
static inline int64_t app_launch_info(ArkLaunchInfo *r) {
    return ark_call(ARK_SYS_LAUNCH, r, sizeof *r);
}
static inline int64_t app_activity(bool active, uint64_t deadline) {
    ArkActivityRequest r = {0};
    r.op = ARK_ACTIVITY_SET;
    r.active = active;
    r.kind = 1;
    r.deadline = deadline;
    return ark_call(ARK_SYS_ACTIVITY, &r, sizeof r);
}
#endif

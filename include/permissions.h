#ifndef ARK_PERMISSIONS_H
#define ARK_PERMISSIONS_H
#define ARK_KERNEL
#include "ark_api.h"
uint64_t permissions_caps(unsigned app, uint32_t uid);
int64_t permissions_request(ArkPermissionRequest *);
void permissions_process_exit(uint32_t pid);
void permissions_session_reset(void);
void services_permissions_changed(const char *name, uint32_t uid, uint64_t caps);
#endif

#ifndef ARK_REMINDER_H
#define ARK_REMINDER_H
#include <stdint.h>
#define ARK_REMINDER_LIMIT 128
typedef struct {
    int64_t due;
    uint32_t completed, notified;
    char title[128], date[24];
} ArkReminder;
#endif

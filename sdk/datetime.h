#ifndef ARK_SDK_DATETIME_H
#define ARK_SDK_DATETIME_H
#include "app.h"
static inline int app_month_days(int y, int m) {
    static const int n[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return n[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}
static inline int app_timezone(void) {
    ArkRegistryRequest q = {.op = ARK_REG_GET};
    strcopy(q.key, "/user/desktop/timezone", 128);
    return ark_registry(&q) >= 0 && q.type == ARK_REG_INTEGER && q.integer >= -12 && q.integer <= 14
               ? (int)q.integer
               : 8;
}
static inline int64_t app_epoch(const ArkDateTime *d) {
    int64_t days = 0;
    for (int y = 1970; y < d->year; y++)
        days += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (int m = 1; m < d->month; m++)
        days += app_month_days(d->year, m);
    days += d->day - 1;
    return days * 86400 + d->hour * 3600 + d->minute * 60 + d->second;
}
static inline void app_shift_time(ArkDateTime *d, int hours) {
    d->hour += hours;
    while (d->hour < 0) {
        d->hour += 24;
        d->day--;
        d->weekday = (d->weekday + 6) % 7;
    }
    while (d->hour >= 24) {
        d->hour -= 24;
        d->day++;
        d->weekday = (d->weekday + 1) % 7;
    }
    if (d->day < 1) {
        if (--d->month < 1) {
            d->month = 12;
            d->year--;
        }
        d->day = app_month_days(d->year, d->month);
    } else if (d->day > app_month_days(d->year, d->month)) {
        d->day = 1;
        if (++d->month > 12) {
            d->month = 1;
            d->year++;
        }
    }
}
static inline bool app_local_datetime(ArkDateTime *d) {
    if (ark_datetime(d) < 0 || !d->valid)
        return false;
    app_shift_time(d, app_timezone());
    return true;
}
#endif

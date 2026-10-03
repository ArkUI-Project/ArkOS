/* Due reminders are checked by the native desktop service even when all
 * Reminders windows are closed. Committed values survive guest restarts. */
#include "reminder.h"
#include "datetime.h"
static ArkReminder reminder_cache[ARK_REMINDER_LIMIT];
static char reminder_keys[ARK_REMINDER_LIMIT][128];
static unsigned reminder_count;
static uint64_t reminder_revision, reminder_poll;
static void reminders_reset(void) {
    reminder_revision = reminder_poll = 0;
    reminder_count = 0;
    reminder_pending = false;
    memset(reminder_cache, 0, sizeof reminder_cache);
    memset(reminder_keys, 0, sizeof reminder_keys);
    reminder_title[0] = 0;
}
static void reminders_tick(uint64_t now) {
    if (now < reminder_poll)
        return;
    reminder_poll = now + 100;
    ArkRegistryRequest state = {.op = ARK_REG_REVISION};
    strcopy(state.key, "/apps/reminders/items/", 128);
    if (ark_registry(&state) < 0)
        return;
    if (state.generation != reminder_revision) {
        reminder_count = 0;
        for (unsigned index = 0;; index++) {
            ArkRegistryRequest q = {.op = ARK_REG_LIST, .index = index};
            strcopy(q.key, "/apps/reminders/items/", 128);
            int64_t result = ark_registry(&q);
            if (result == -2)
                break;
            if (result < 0)
                return;
            if (q.type != ARK_REG_BINARY || q.length != sizeof(ArkReminder))
                continue;
            ArkReminder item;
            memcpy(&item, q.value, sizeof item);
            if (item.completed > 1 || item.notified > 1 || item.due < 0 || item.title[127] ||
                item.date[23])
                continue;
            if (reminder_count == ARK_REMINDER_LIMIT)
                break;
            reminder_cache[reminder_count] = item;
            strcopy(reminder_keys[reminder_count++], q.key, 128);
        }
        reminder_revision = state.generation;
    }
    ArkDateTime date = {0};
    if (ark_datetime(&date) < 0 || !date.valid)
        return;
    int64_t utc = app_epoch(&date);
    for (unsigned i = 0; i < reminder_count; i++) {
        ArkReminder *item = &reminder_cache[i];
        if (item->completed || item->notified || item->due > utc)
            continue;
        item->notified = 1;
        ArkRegistryRequest q = {.op = ARK_REG_SET, .type = ARK_REG_BINARY, .length = sizeof *item};
        strcopy(q.key, reminder_keys[i], 128);
        memcpy(q.value, item, sizeof *item);
        bool saved = ark_registry(&q) >= 0;
        if (saved)
            reminder_revision = 0;
        reminder_pending = true;
        strcopy(reminder_title, item->title, 128);
        island_texture_valid = false;
        notice(saved ? item->title : "提醒已到期；通知状态暂时无法保存");
        serial_write("[reminders] due notification\n");
        dirty = true;
    }
}

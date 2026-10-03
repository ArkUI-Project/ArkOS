#ifndef ARK_SDK_DRAG_H
#define ARK_SDK_DRAG_H
#include "app.h"
static inline bool app_drag_begin(ArkApp *app, unsigned kind, const char *data, const char *mime) {
    ArkDragRequest q = {.op = ARK_DRAG_BEGIN,
                        .source = app->id,
                        .target = UINT32_MAX,
                        .kind = kind,
                        .action = ARK_DRAG_COPY};
    q.length = (uint32_t)strlen(data);
    if (q.length >= sizeof q.data)
        return false;
    strcopy(q.data, data, sizeof q.data);
    strcopy(q.mime, mime, sizeof q.mime);
    bool ok = ark_drag(&q) >= 0;
    if (ok)
        app->pointer_cancelled = true;
    return ok;
}
static inline bool app_drop_read(ArkApp *app, const ArkEvent *e, ArkDragRequest *q) {
    memset(q, 0, sizeof *q);
    q->op = ARK_DRAG_READ;
    q->target = app->id;
    q->token = (unsigned)e->key;
    return e->type == ARK_EV_DROP && ark_drag(q) >= 0;
}
static inline void app_drop_accept(ArkApp *app, const ArkDragRequest *drop, bool accepted) {
    ArkDragRequest q = {.op = ARK_DRAG_ACCEPT,
                        .target = app->id,
                        .token = drop->token,
                        .action = accepted ? ARK_DRAG_COPY : 0};
    (void)ark_drag(&q);
}
#endif

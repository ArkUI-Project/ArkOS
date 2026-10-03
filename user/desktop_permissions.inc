/* Only the trusted desktop resolves kernel-created consent requests. */
static ArkPermissionRequest permission_request;
static uint64_t permission_next;
static void permission_tick(uint64_t now) {
    if (now < permission_next)
        return;
    permission_next = now + 10;
    ArkPermissionRequest q = {.op = ARK_PERMISSION_PENDING};
    bool available = ark_call(ARK_SYS_PERMISSION, &q, sizeof q) >= 0 && q.app < ARK_CATALOG_COUNT;
    if (available && (!permission_showing || q.app != permission_request.app ||
                      q.grants != permission_request.grants)) {
        motion_cancel();
        context_visible = false;
        caption_menu = -1;
        drag = resize_app = -1;
        permission_request = q;
        permission_showing = true;
        dirty = true;
        serial_write("[permission] Consent requested by ");
        serial_write(q.name);
        serial_write("\n");
    } else if (!available && permission_showing) {
        permission_showing = false;
        dirty = true;
    }
}
static void permission_resolve(bool allow) {
    ArkPermissionRequest q = permission_request;
    q.op = ARK_PERMISSION_RESOLVE;
    if (!allow)
        q.grants = 0;
    int64_t result = ark_call(ARK_SYS_PERMISSION, &q, sizeof q);
    if (result < 0)
        notice("授权状态保存失败，请检查磁盘和应用状态");
    else {
        serial_write(allow ? "[permission] Allowed " : "[permission] Denied ");
        serial_write(permission_request.name);
        serial_write("\n");
    }
    permission_showing = false;
    permission_next = 0;
    dirty = true;
}
static void permission_draw(void) {
    if (!permission_showing)
        return;
    int x = (sw - 500) / 2, y = (sh - 272) / 2;
    rr(0, 0, sw, sh, 0, 0x101c30, 105);
    shadow(x, y, 500, 272);
    glass(x, y, 500, 272, 24, 250);
    icon((int)ark_catalog[permission_request.app].desktop, x + 24, y + 22, 42);
    label(x + 82, y + 24, ark_catalog[permission_request.app].label, ink);
    label(x + 82, y + 49, "请求系统授权", muted);
    const char *scope = permission_request.grants & ARK_CAP_NETWORK ? "访问网络与网站"
                        : permission_request.grants & ARK_CAP_FILES ? "读写你的文件及已挂载的外置卷"
                                                                    : "在顶部显示应用活动";
    clip(x + 25, y + 101, scope, 450, ink);
    label(x + 25, y + 141, "授权只授予此应用，可随时在设置中撤销。", muted);
    label(x + 25, y + 169, "拒绝后，本次运行不再重复弹窗。", muted);
    button(x + 255, y + 215, 100, "拒绝", false);
    button(x + 366, y + 215, 110, "允许", true);
}
static bool permission_key(int key) {
    if (!permission_showing)
        return false;
    if (key == KEY_ESCAPE)
        permission_resolve(false);
    else if (key == KEY_ENTER)
        permission_resolve(true);
    else if (key == 11)
        session_lock();
    return true;
}
static bool permission_pointer(bool pressed) {
    if (!permission_showing)
        return false;
    int x = (sw - 500) / 2, y = (sh - 272) / 2;
    if (pressed) {
        if (in(mx, my, x + 255, y + 215, 100, 36))
            permission_resolve(false);
        else if (in(mx, my, x + 366, y + 215, 110, 36))
            permission_resolve(true);
    }
    return true;
}

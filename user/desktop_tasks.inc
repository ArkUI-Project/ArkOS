/* Activity Monitor: one row per live address space, sampled from the kernel.
 * Thread records contribute CPU time; shared memory and I/O are counted once. */
typedef struct {
    ArkTaskMetrics metric;
    unsigned threads, cpu;
    uint64_t read_rate, write_rate;
} TaskRow;
static ArkTaskMetrics task_samples[32], task_previous[32];
static TaskRow task_rows[32];
static unsigned task_count, task_old_count, task_order[32], task_visible, task_scroll,
    task_selected, task_cores = 1;
static unsigned task_tab = 1, task_filter, task_sort = 1;
static bool task_descending = true, task_search_active, task_more, task_information, task_stop;
static char task_search[64];
static ArkAccountRequest task_accounts[8];
static unsigned task_account_count;
static uint64_t task_next, task_at;
static ArkPerformanceInfo task_perf, task_perf_old;
static unsigned task_graph[5][60], task_graph_count, task_total_cpu, task_memory_pressure;
static uint64_t task_read_rate, task_write_rate, task_rx_rate, task_tx_rate;
static void task_units(char out[40], uint64_t value) {
    const char *unit = "B";
    uint64_t divisor = 1;
    if (value >= 1024ull * 1024 * 1024) {
        unit = "GB";
        divisor = 1024ull * 1024 * 1024;
    } else if (value >= 1024 * 1024) {
        unit = "MB";
        divisor = 1024 * 1024;
    } else if (value >= 1024) {
        unit = "KB";
        divisor = 1024;
    }
    uint_to_str(value / divisor, out);
    if (divisor > 1) {
        cat(out, 40, ".");
        char n[8];
        uint_to_str((value % divisor) * 10 / divisor, n);
        cat(out, 40, n);
    }
    cat(out, 40, " ");
    cat(out, 40, unit);
}
static bool task_contains(const char *hay, const char *needle) {
    if (!*needle)
        return true;
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        unsigned i = 0;
        for (; i < n; i++) {
            unsigned a = (unsigned char)hay[i], b = (unsigned char)needle[i];
            if (!a)
                break;
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b)
                break;
        }
        if (i == n)
            return true;
    }
    return false;
}
static void task_user(uint32_t uid, char out[64]) {
    if (uid == 0) {
        strcopy(out, "系统", 64);
        return;
    }
    for (unsigned i = 0; i < task_account_count; i++)
        if (task_accounts[i].uid == uid) {
            strcopy(out, task_accounts[i].user, 64);
            return;
        }
    uint_to_str(uid, out);
}
static TaskRow *task_chosen(void) {
    for (unsigned i = 0; i < task_count; i++)
        if (task_rows[i].metric.task.pid == task_selected)
            return &task_rows[i];
    return 0;
}
static uint64_t task_sort_value(const TaskRow *r) {
    switch (task_sort) {
    case 0:
        return r->cpu;
    case 1:
        return r->metric.task.mapped_bytes;
    case 2:
        return r->threads;
    case 3:
        return r->metric.task.pid;
    case 4:
        return r->read_rate;
    case 5:
        return r->write_rate;
    default:
        return 0;
    }
}
static const char *task_name(const ArkTaskInfo *t) {
    if (!strcmp(t->name, "Ark Desktop"))
        return "ArkOS 桌面";
    for (unsigned i = 0; i < ARK_CATALOG_COUNT; i++)
        if (!strcmp(t->name, ark_catalog[i].program))
            return ark_catalog[i].label;
    return t->name;
}
static unsigned task_graph_rate(uint64_t n) {
    n /= 1024;
    return n > INT32_MAX ? INT32_MAX : (unsigned)n;
}
static void task_filter_rows(void) {
    task_visible = 0;
    for (unsigned i = 0; i < task_count; i++) {
        ArkTaskInfo *t = &task_rows[i].metric.task;
        bool system = (t->capabilities & ARK_CAP_SYSTEM) != 0;
        if ((task_filter == 1 && system) || (task_filter == 2 && !system))
            continue;
        char pid[24], user[64];
        uint_to_str(t->pid, pid);
        task_user(t->uid, user);
        if (!task_contains(t->name, task_search) && !task_contains(task_name(t), task_search) &&
            !task_contains(pid, task_search) && !task_contains(user, task_search))
            continue;
        task_order[task_visible++] = i;
    }
    for (unsigned i = 1; i < task_visible; i++) {
        unsigned v = task_order[i], j = i;
        while (j) {
            unsigned prev = task_order[j - 1];
            uint64_t a = task_sort_value(&task_rows[prev]), b = task_sort_value(&task_rows[v]);
            bool before = task_sort == 6 ? strcmp(task_rows[prev].metric.task.name,
                                                  task_rows[v].metric.task.name) > 0
                                         : (task_descending ? a < b : a > b);
            if (!before)
                break;
            task_order[j] = prev;
            j--;
        }
        task_order[j] = v;
    }
    if (task_scroll >= task_visible)
        task_scroll = task_visible ? task_visible - 1 : 0;
    if (!task_chosen()) {
        task_selected = 0;
        task_information = task_stop = false;
    }
}
static void tasks_open(void) {
    task_next = 0;
    tasks_tick(ark_ticks());
}
static void tasks_tick(uint64_t now) {
    if (now < task_next)
        return;
    bool visible = false;
    for (int w = 0; w < WINS; w++)
        if (window_app(w) == 10 && windows[w].visible)
            visible = true;
    if (!visible)
        return;
    task_next = now + 100;
    ArkTaskRequest q = {.op = ARK_TASK_METRICS, .buffer = (uintptr_t)task_samples, .capacity = 32};
    if (ark_tasks(&q) < 0)
        return;
    task_cores = q.online_cpus;
    task_account_count = 0;
    for (unsigned i = 0; i < 8; i++) {
        ArkAccountRequest a = {.op = ARK_ACCOUNT_LIST, .index = i};
        if (ark_account(&a) < 0)
            break;
        task_accounts[task_account_count++] = a;
    }
    uint64_t elapsed = q.ticks - task_at;
    task_count = 0;
    for (unsigned i = 0; i < q.count; i++) {
        ArkTaskMetrics *m = &task_samples[i];
        unsigned group = task_count;
        for (unsigned j = 0; j < task_count; j++)
            if (task_rows[j].metric.task.pid == m->task.pid) {
                group = j;
                break;
            }
        if (group == task_count) {
            task_rows[group] = (TaskRow){.metric = *m};
            task_count++;
            if (elapsed)
                for (unsigned j = 0; j < task_old_count; j++)
                    if (task_previous[j].task.pid == m->task.pid) {
                        task_rows[group].read_rate =
                            (m->read_bytes - task_previous[j].read_bytes) * 100 / elapsed;
                        task_rows[group].write_rate =
                            (m->write_bytes - task_previous[j].write_bytes) * 100 / elapsed;
                        break;
                    }
        }
        task_rows[group].threads++;
        if (!m->task.thread)
            task_rows[group].metric = *m;
        if (elapsed)
            for (unsigned j = 0; j < task_old_count; j++)
                if (task_previous[j].task.tid == m->task.tid) {
                    task_rows[group].cpu +=
                        (unsigned)((m->task.cpu_ticks - task_previous[j].task.cpu_ticks) * 100 /
                                   elapsed);
                    break;
                }
    }
    if (ark_call(ARK_SYS_PERFORMANCE, &task_perf, sizeof task_perf) >= 0) {
        uint64_t busy = 0, total = 0, ms = task_perf.millis - task_perf_old.millis;
        for (unsigned c = 0; c < 8; c++) {
            busy += task_perf.cpu_busy_ms[c] - task_perf_old.cpu_busy_ms[c];
            total += task_perf.cpu_total_ms[c] - task_perf_old.cpu_total_ms[c];
        }
        task_total_cpu = total ? (unsigned)(busy * 100 / total) : 0;
        task_memory_pressure = task_perf.pool_bytes
                                   ? (unsigned)((task_perf.pool_bytes - task_perf.free_bytes) *
                                                100 / task_perf.pool_bytes)
                                   : 0;
        if (task_perf_old.millis && ms) {
            task_read_rate =
                (task_perf.disk_read_bytes - task_perf_old.disk_read_bytes) * 1000 / ms;
            task_write_rate =
                (task_perf.disk_write_bytes - task_perf_old.disk_write_bytes) * 1000 / ms;
            task_rx_rate =
                (task_perf.network_rx_bytes - task_perf_old.network_rx_bytes) * 1000 / ms;
            task_tx_rate =
                (task_perf.network_tx_bytes - task_perf_old.network_tx_bytes) * 1000 / ms;
        }
        if (task_graph_count == 60) {
            for (unsigned t = 0; t < 5; t++)
                memmove(task_graph[t], task_graph[t] + 1, 59 * sizeof(unsigned));
            task_graph_count--;
        }
        task_graph[0][task_graph_count] = task_total_cpu;
        task_graph[1][task_graph_count] = task_memory_pressure;
        task_graph[2][task_graph_count] = 0;
        task_graph[3][task_graph_count] = task_graph_rate(task_read_rate + task_write_rate);
        task_graph[4][task_graph_count] = task_graph_rate(task_rx_rate + task_tx_rate);
        task_graph_count++;
        task_perf_old = task_perf;
    }
    memcpy(task_previous, task_samples, sizeof task_samples);
    task_old_count = q.count;
    task_at = q.ticks;
    task_filter_rows();
    dirty = true;
}
static void task_metric_label(int x, int y, const char *title, uint64_t value, int gap) {
    char b[40];
    label(x, y, title, muted);
    task_units(b, value);
    label(x + gap, y, b, ink);
}
static void task_round_button(int x, int y, ArkUISymbol s, bool enabled) {
    click_region(x, y, 36, 36);
    circle_tint(x + 18, y + 18, 18, night ? 0x71829b : 0xffffff, enabled ? 145 : 65);
    symbol(x + 9, y + 9, 18, s, enabled ? ink : muted);
}
static void task_layout(Window *w, int *x, int *y, int *width, int *rows, int *footer) {
    *x = w->x + 20;
    *y = w->y + 130;
    *width = w->w - 40;
    *footer = max(*y + 95, w->y + w->h - 142);
    *rows = max(1, (*footer - *y - 36) / 32);
}
static void task_dialog(Window *w) {
    TaskRow *r = task_chosen();
    if (!r || (!task_information && !task_stop))
        return;
    int width = min(480, w->w - 48), height = task_stop ? 224 : 386, x = w->x + (w->w - width) / 2,
        y = w->y + 60 + (w->h - 60 - height) / 2;
    rr(w->x + 1, w->y + 54, w->w - 2, w->h - 55, 18, night ? 0x071322 : 0x24324a, 75);
    shadow(x, y, width, height);
    glass(x, y, width, height, 22, 230);
    text(x + 24, y + 22, task_stop ? "结束这个应用？" : "进程信息", ink, 2);
    clip(x + 24, y + 74, r->metric.task.name, width - 48, ink);
    if (task_stop) {
        clip(x + 24, y + 108, "尚未保存的更改可能丢失。", width - 48, muted);
        button(x + width - 236, y + height - 61, 98, "取消", false);
        button(x + width - 126, y + height - 61, 102, "结束", true);
        return;
    }
    int yy = y + 112;
    label(x + 24, yy, "PID", muted);
    num(x + 206, yy, r->metric.task.pid, ink);
    char u[64];
    task_user(r->metric.task.uid, u);
    label(x + 24, yy + 29, "用户", muted);
    label(x + 206, yy + 29, u, ink);
    task_metric_label(x + 24, yy + 58, "驻留内存", r->metric.task.mapped_bytes, 182);
    task_metric_label(x + 24, yy + 87, "已提交内存", r->metric.committed_bytes, 182);
    task_metric_label(x + 24, yy + 116, "已交换内存", r->metric.swapped_bytes, 182);
    label(x + 24, yy + 145, "线程 / 换页次数", muted);
    num(x + 206, yy + 145, r->threads, ink);
    num(x + 254, yy + 145, r->metric.faults, ink);
    button(x + width - 126, y + height - 57, 102, "完成", true);
}
static void draw_tasks(Window *w) {
    int x, y, width, rows, footer;
    task_layout(w, &x, &y, &width, &rows, &footer);
    TaskRow *chosen = task_chosen();
    task_round_button(x, w->y + 73, ARKUI_SYMBOL_CLOSE,
                      chosen && !(chosen->metric.task.capabilities & ARK_CAP_SYSTEM));
    task_round_button(x + 45, w->y + 73, ARKUI_SYMBOL_MORE,
                      chosen != 0); /* i is drawn with the system font. */
    circle(x + 63, w->y + 91, 10, night ? 0x33465b : 0xe2e6eb);
    label(x + 61, w->y + 80, "i", ink);
    task_round_button(x + 90, w->y + 73, ARKUI_SYMBOL_MORE, true);
    static const char *tabs[] = {"CPU", "内存", "能耗", "磁盘", "网络"};
    int tx = x + 144, tw = 57;
    rr(tx, w->y + 72, tw * 5 + 6, 38, 19, night ? 0x66738a : 0xffffff, 85);
    for (unsigned i = 0; i < 5; i++) {
        int bx = tx + 3 + (int)i * tw;
        click_region(bx, w->y + 74, tw, 34);
        if (i == task_tab)
            rr(bx, w->y + 75, tw, 32, 16, night ? 0x92a1b9 : 0xd2d6dc, 120);
        label(bx + (tw - utf8_width(tabs[i])) / 2, w->y + 81, tabs[i], ink);
    }
    int sx = tx + tw * 5 + 26, searchw = x + width - sx;
    click_text_region(sx, w->y + 73, searchw, 36);
    rr(sx, w->y + 73, searchw, 36, 18, night ? 0x66738a : 0xffffff, task_search_active ? 150 : 80);
    symbol(sx + 11, w->y + 82, 18, ARKUI_SYMBOL_SEARCH, muted);
    clip(sx + 36, w->y + 82, task_search[0] ? task_search : "搜索", searchw - 47,
         task_search[0] ? ink : muted);
    label(x + 12, y, "进程名称", muted);
    click_region(x, y - 8, width - 422, 32);
    const char *primary = task_tab == 0   ? "CPU %"
                          : task_tab == 1 ? "内存"
                          : task_tab == 2 ? "功率"
                          : task_tab == 3 ? "读取 / 秒"
                                          : "网络权限";
    label(x + width - 416, y, primary, muted);
    label(x + width - 285, y, "线程", muted);
    label(x + width - 208, y, task_tab == 3 ? "写入 / 秒" : "PID", muted);
    label(x + width - 104, y, "用户", muted);
    for (unsigned c = 0; c < 4; c++)
        click_region(x + width - 430 + (int)c * 107, y - 8, 107, 32);
    rect(x, y + 27, width, 1, night ? 0x536278 : 0xcfd6de);
    if (task_scroll > (unsigned)max(0, (int)task_visible - rows))
        task_scroll = (unsigned)max(0, (int)task_visible - rows);
    for (unsigned j = task_scroll; j < task_visible && j < task_scroll + (unsigned)rows; j++) {
        TaskRow *r = &task_rows[task_order[j]];
        ArkTaskInfo *t = &r->metric.task;
        int yy = y + 35 + (int)(j - task_scroll) * 32;
        bool selected = t->pid == task_selected;
        click_region(x, yy - 3, width, 30);
        if (selected || j % 2)
            rr(x, yy - 3, width, 30, 9, selected ? 0x0765d6 : (night ? 0x5b6a7e : 0xe4e7eb),
               selected ? 255 : 80);
        uint32_t c = selected ? 0xffffff : ink;
        int app = -1;
        for (unsigned a = 0; a < ARK_CATALOG_COUNT; a++)
            if (!strcmp(t->name, ark_catalog[a].program)) {
                app = ark_catalog[a].desktop;
                break;
            }
        if (app >= 0)
            icon(app, x + 8, yy + 1, 22);
        else
            symbol(x + 8, yy + 3, 18,
                   (t->capabilities & ARK_CAP_SYSTEM) ? ARKUI_SYMBOL_SETTINGS
                                                      : ARKUI_SYMBOL_DOCUMENT,
                   c);
        clip(x + 39, yy + 3, task_name(t), width - 482, c);
        char b[40], u[64];
        if (task_tab == 0)
            num(x + width - 416, yy + 3, r->cpu, c);
        else if (task_tab == 1) {
            task_units(b, t->mapped_bytes);
            label(x + width - 416, yy + 3, b, c);
        } else if (task_tab == 2)
            label(x + width - 416, yy + 3, "—", c);
        else if (task_tab == 3) {
            task_units(b, r->read_rate);
            label(x + width - 416, yy + 3, b, c);
        } else
            label(x + width - 416, yy + 3,
                  (t->capabilities & (ARK_CAP_NETWORK | ARK_CAP_SYSTEM)) ? "允许" : "未授权", c);
        num(x + width - 285, yy + 3, r->threads, c);
        if (task_tab == 3) {
            task_units(b, r->write_rate);
            label(x + width - 208, yy + 3, b, c);
        } else
            num(x + width - 208, yy + 3, t->pid, c);
        task_user(t->uid, u);
        clip(x + width - 104, yy + 3, u, 98, c);
    }
    if (!task_visible)
        label(x + 18, y + 62, "没有符合条件的进程", muted);
    rect(x, footer, width, 1, night ? 0x536278 : 0xcfd6de);
    int gw = min(250, width / 3), gy = footer + 22, gh = 89;
    rr(x + 4, gy, gw, gh, 12, night ? 0x26384f : 0xffffff, 160);
    const char *graph_label = task_tab == 0   ? "CPU 使用率"
                              : task_tab == 1 ? "内存使用率"
                              : task_tab == 2 ? "能耗计量"
                                              : "传输速率";
    label(x + 17, gy + 8, graph_label, muted);
    unsigned peak = 100;
    if (task_tab >= 3) {
        peak = 1;
        for (unsigned i = 0; i < task_graph_count; i++)
            if (task_graph[task_tab][i] > peak)
                peak = task_graph[task_tab][i];
    }
    for (unsigned i = 1; i < task_graph_count && task_tab != 2; i++) {
        int px = x + 16 + (gw - 28) * (int)(i - 1) / 59, nx = x + 16 + (gw - 28) * (int)i / 59;
        line(px, gy + gh - 9 - (int)((uint64_t)task_graph[task_tab][i - 1] * (gh - 38) / peak), nx,
             gy + gh - 9 - (int)((uint64_t)task_graph[task_tab][i] * (gh - 38) / peak),
             task_tab == 1 && task_memory_pressure > 90 ? 0xe6a442 : accent);
    }
    int dx = x + gw + 24, gap = max(125, (width - gw - 40) / 2), column = dx + gap;
    char b[40];
    if (task_tab == 1) {
        task_metric_label(dx, gy, "物理内存", (uint64_t)boot.memory_mib * 1024 * 1024, 118);
        task_metric_label(dx, gy + 27, "可用内存", task_perf.free_bytes, 118);
        task_metric_label(dx, gy + 54, "已交换内存", task_perf.swapped_bytes, 118);
        task_metric_label(column, gy, "应用内存", task_perf.user_bytes, 100);
        task_metric_label(column, gy + 27, "系统缓冲", task_perf.kernel_bytes, 100);
        task_metric_label(column, gy + 54, "已提交内存", task_perf.committed_bytes, 100);
    } else if (task_tab == 0) {
        label(dx, gy, "使用率", muted);
        num(dx + 105, gy, task_total_cpu, ink);
        label(dx + 136, gy, "%", ink);
        label(dx, gy + 29, "在线 CPU", muted);
        num(dx + 105, gy + 29, task_cores, ink);
        label(column, gy, "进程", muted);
        num(column + 100, gy, task_count, ink);
        label(column, gy + 29, "线程", muted);
        num(column + 100, gy + 29, task_perf.tasks, ink);
    } else if (task_tab == 2) {
        clip(dx, gy, "当前设备暂未提供能耗计量。", width - gw - 42, ink);
        clip(dx, gy + 31, "CPU 页面可查看正在运行的负载。", width - gw - 42, muted);
    } else {
        task_metric_label(dx, gy, task_tab == 3 ? "读取 / 秒" : "接收 / 秒",
                          task_tab == 3 ? task_read_rate : task_rx_rate, 118);
        task_metric_label(dx, gy + 29, task_tab == 3 ? "写入 / 秒" : "发送 / 秒",
                          task_tab == 3 ? task_write_rate : task_tx_rate, 118);
        task_metric_label(column, gy, task_tab == 3 ? "累计读取" : "累计接收",
                          task_tab == 3 ? task_perf.disk_read_bytes : task_perf.network_rx_bytes,
                          100);
        task_metric_label(column, gy + 29, task_tab == 3 ? "累计写入" : "累计发送",
                          task_tab == 3 ? task_perf.disk_write_bytes : task_perf.network_tx_bytes,
                          100);
    }
    uint_to_str(task_visible, b);
    cat(b, 40, " 个进程");
    label(x + 12, w->y + w->h - 25, b, muted);
    if (task_more) {
        int bx = x + 84, by = w->y + 116;
        shadow(bx, by, 210, 144);
        glass(bx, by, 210, 144, 16, 230);
        const char *items[] = {"所有进程", "我的应用", "系统进程", "立即刷新"};
        for (int i = 0; i < 4; i++) {
            click_region(bx, by + i * 36, 210, 36);
            if (i < 3 && task_filter == (unsigned)i)
                symbol(bx + 12, by + i * 36 + 9, 18, ARKUI_SYMBOL_CHECK, accent);
            label(bx + 39, by + i * 36 + 8, items[i], ink);
        }
    }
    task_dialog(w);
}
static void tasks_click(int px, int py) {
    Window *w = app_window(10);
    int x, y, width, rows, footer;
    task_layout(w, &x, &y, &width, &rows, &footer);
    (void)footer;
    TaskRow *r = task_chosen();
    if (task_information || task_stop) {
        if (!r)
            return;
        int dw = min(480, w->w - 48), dh = task_stop ? 224 : 386, bx = w->x + (w->w - dw) / 2,
            by = w->y + 60 + (w->h - 60 - dh) / 2;
        if (task_stop) {
            if (in(px, py, bx + dw - 236, by + dh - 61, 98, 36))
                task_stop = false;
            else if (in(px, py, bx + dw - 126, by + dh - 61, 102, 36)) {
                ArkTaskRequest q = {.op = ARK_TASK_KILL, .tid = r->metric.task.tid};
                if (ark_tasks(&q) < 0)
                    notice("此进程受系统保护或已经退出");
                task_stop = false;
                task_next = 0;
                tasks_tick(ark_ticks());
            }
        } else if (in(px, py, bx + dw - 126, by + dh - 57, 102, 36))
            task_information = false;
        dirty = true;
        return;
    }
    if (task_more) {
        task_more = false;
        if (in(px, py, x + 84, w->y + 116, 210, 144)) {
            int row = (py - w->y - 116) / 36;
            if (row < 3)
                task_filter = (unsigned)row;
            else
                task_next = 0;
            task_scroll = 0;
            task_filter_rows();
        }
        dirty = true;
        return;
    }
    if (in(px, py, x, w->y + 73, 36, 36)) {
        if (r && !(r->metric.task.capabilities & ARK_CAP_SYSTEM))
            task_stop = true;
    } else if (in(px, py, x + 45, w->y + 73, 36, 36)) {
        task_information = r != 0;
    } else if (in(px, py, x + 90, w->y + 73, 36, 36))
        task_more = true;
    else if (in(px, py, x + 147, w->y + 74, 285, 34)) {
        task_tab = (unsigned)(px - x - 147) / 57;
        task_sort = task_tab == 0 ? 0 : task_tab == 3 ? 4 : 1;
        task_descending = true;
        task_filter_rows();
    } else if (in(px, py, x + 455, w->y + 73, width - 455, 36))
        task_search_active = true;
    else {
        task_search_active = false;
        if (in(px, py, x, y - 8, width, 32)) {
            unsigned sort;
            if (px < x + width - 430)
                sort = 6;
            else if (px < x + width - 323)
                sort = task_tab == 0 ? 0 : task_tab == 3 ? 4 : 1;
            else if (px < x + width - 216)
                sort = 2;
            else
                sort = task_tab == 3 ? 5 : 3;
            if (sort == task_sort)
                task_descending = !task_descending;
            else {
                task_sort = sort;
                task_descending = true;
            }
            task_filter_rows();
        } else if (in(px, py, x, y + 32, width, rows * 32)) {
            unsigned row = task_scroll + (unsigned)(py - y - 32) / 32;
            if (row < task_visible)
                task_selected = task_rows[task_order[row]].metric.task.pid;
        }
    }
    dirty = true;
}
static void tasks_scroll_by(int delta) {
    task_scroll = (unsigned)clamp((int)task_scroll + delta, 0, max(0, (int)task_visible - 1));
    dirty = true;
}
static bool tasks_key(int key) {
    if (focused_app() != 10 || dialog_kind)
        return false;
    if (key == 6) {
        task_search_active = true;
        dirty = true;
        return true;
    }
    if (key == KEY_ESCAPE) {
        task_information = task_stop = task_more = task_search_active = false;
        dirty = true;
        return true;
    }
    if (task_search_active) {
        size_t n = strlen(task_search);
        if (key == KEY_BACKSPACE) {
            task_search[utf8_prev(task_search, n)] = 0;
        } else if (key >= 32 && key < 127 && n + 1 < sizeof task_search) {
            task_search[n] = (char)key;
            task_search[n + 1] = 0;
        } else if (key == KEY_ENTER)
            task_search_active = false;
        task_scroll = 0;
        task_filter_rows();
        dirty = true;
        return true;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        unsigned i = 0;
        for (; i < task_visible; i++)
            if (task_rows[task_order[i]].metric.task.pid == task_selected)
                break;
        if (task_visible) {
            i = (unsigned)clamp((int)i + (key == KEY_UP ? -1 : 1), 0, (int)task_visible - 1);
            task_selected = task_rows[task_order[i]].metric.task.pid;
            if (i < task_scroll)
                task_scroll = i;
            else {
                int x, y, width, rows, footer;
                task_layout(app_window(10), &x, &y, &width, &rows, &footer);
                if (i >= task_scroll + (unsigned)rows)
                    task_scroll = i - (unsigned)rows + 1;
            }
        }
        dirty = true;
        return true;
    }
    return false;
}
static void tasks_insert(const char *s) {
    size_t n = strlen(task_search), bytes = strlen(s);
    if (n + bytes < sizeof task_search) {
        memcpy(task_search + n, s, bytes + 1);
        task_scroll = 0;
        task_filter_rows();
        dirty = true;
    }
}

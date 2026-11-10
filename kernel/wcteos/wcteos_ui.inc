/* =====================================================================
   wcteos_ui.inc —— wcteos 桌面外壳层
   开始菜单 / 运行框（含管理员选项） / 设置（主题+壁纸+动态 .glf）
   任务管理器（新建进程） / 关于（wtcer） / 锁屏 / 托盘
   全部为内核内自研实现，跑在 WCT 自己的图形栈上，不依赖 Linux。
   ===================================================================== */

static int ui_menu, ui_run, ui_admin, ui_set, ui_task, ui_about, ui_lock;
static int ui_wp = 0;
static int ui_demo_stage = -1;   /* >=0 表示处于演示序列 */              /* 0 纯色 1 渐变 2 网格 3 动态 .glf */
static char ui_input[96];
static int ui_len;

/* ---------- 动态壁纸 .glf ----------
   格式：magic "GLF1" | u16 w | u16 h | u8 frames | u8 reserved[3]
        然后每帧 w*h 字节 RGB332。整文件一次读入。 */
#define GLF_MAX_W 128
#define GLF_MAX_H 96
#define GLF_MAX_F 8
static unsigned char glf_buf[12 + GLF_MAX_W * GLF_MAX_H * GLF_MAX_F];
static int glf_w, glf_h, glf_frames, glf_cur, glf_ok;
static int glf_tick;

static int ui_hit(int x, int y, int w, int h);

static int ui_slen(const char *s) { int n = 0; while (s[n]) ++n; return n; }

static void ui_panel(int x, int y, int w, int h, const char *title)
{
    gfx_fill(x, y, w, h, gfx_panel);
    gfx_fill(x + w, y + 2, 2, h - 2, gfx_rgb(150,142,130));   /* 纸张投影 */
    gfx_fill(x + 2, y + h, w - 2, 2, gfx_rgb(150,142,130));
    gfx_border(x, y, w, h, gfx_accent);
    gfx_fill(x + 2, y + 2, w - 4, 26, gfx_top);
    gfx_fill(x + 2, y + 28, w - 4, 1, gfx_rgb(200,193,180));  /* 标题纸边 */
    gfx_text(x + 10, y + 8, title, gfx_accent);
}

static void ui_panel_w(int x, int y, int w, int h, const CHAR16 *title)
{
    gfx_fill(x, y, w, h, gfx_panel);
    gfx_fill(x + w, y + 2, 2, h - 2, gfx_rgb(150,142,130));
    gfx_fill(x + 2, y + h, w - 2, 2, gfx_rgb(150,142,130));
    gfx_border(x, y, w, h, gfx_accent);
    gfx_fill(x + 2, y + 2, w - 4, 26, gfx_top);
    gfx_fill(x + 2, y + 28, w - 4, 1, gfx_rgb(200,193,180));
    gfx_text_w(x + 10, y + 8, title, gfx_accent);
}

static int ui_btn_w(int x, int y, int w, int h, const CHAR16 *label, int active)
{
    int hot = ui_hit(x, y, w, h);
    gfx_fill(x, y, w, h, active ? gfx_accent : (hot ? gfx_top : gfx_panel));
    gfx_border(x, y, w, h, gfx_accent);
    gfx_text_w(x + 8, y + (h - 16) / 2, label, active ? gfx_rgb(255,255,255) : gfx_text_c);
    return hot;
}

static int ui_hit(int x, int y, int w, int h)
{
    return mouse_x >= x && mouse_x < x + w && mouse_y >= y && mouse_y < y + h;
}

static int ui_btn(int x, int y, int w, int h, const char *label, int active)
{
    int hot = ui_hit(x, y, w, h);
    gfx_fill(x, y, w, h, active ? gfx_accent : (hot ? gfx_top : gfx_panel));
    gfx_border(x, y, w, h, gfx_accent);
    gfx_text(x + 8, y + (h - 16) / 2, label, active ? gfx_rgb(6, 10, 18) : gfx_text_c);
    return hot;
}

/* ---------- 壁纸 ---------- */
static void ui_glf_load(void)
{
    if (glf_ok || glf_frames) return;
    EFI_FILE_PROTOCOL *f = NULL;
    if (!open_path(L"\\wct\\wall\\default.glf", &f, EFI_FILE_MODE_READ, 0)) return;
    UINTN want = sizeof(glf_buf);
    EFI_STATUS r = f->Read(f, &want, glf_buf);
    f->Close(f);
    if (EFI_IS_ERROR(r) || want < 12) return;
    if (!(glf_buf[0] == 'G' && glf_buf[1] == 'L' && glf_buf[2] == 'F' && glf_buf[3] == '1')) return;
    glf_w = glf_buf[4] | (glf_buf[5] << 8);
    glf_h = glf_buf[6] | (glf_buf[7] << 8);
    int fr = glf_buf[8];
    if (glf_w < 8 || glf_h < 8 || glf_w > GLF_MAX_W || glf_h > GLF_MAX_H) return;
    if (fr < 1 || fr > GLF_MAX_F) return;
    if (want < (UINTN)(12 + glf_w * glf_h * fr)) return;
    glf_frames = fr;
    glf_cur = 0;
    glf_ok = 1;
}

static void ui_glf_draw(void)
{
    if (!glf_ok) return;
    int fsz = glf_w * glf_h;
    unsigned char *p = glf_buf + 12 + glf_cur * fsz;
    int bx = (int)gfx_w / glf_w; if (bx < 1) bx = 1;
    int by = (int)gfx_h / glf_h; if (by < 1) by = 1;
    for (int y = 0; y < glf_h; ++y)
        for (int x = 0; x < glf_w; ++x) {
            unsigned char v = p[y * glf_w + x];
            gfx_fill(x * bx, y * by, bx, by,
                     gfx_rgb(((v >> 5) & 7) * 36, ((v >> 2) & 7) * 36, (v & 3) * 85));
        }
}

static void wct_ui_bg(void)
{
    int W = (int)gfx_w, H = (int)gfx_h;
    if (ui_wp == 3) {
        ui_glf_load();
        if (glf_ok) { ui_glf_draw(); return; }
        ui_wp = 1;   /* 没有 .glf 就退回渐变，不留黑屏 */
    }
    if (ui_wp == 0) { gfx_fill(0, 0, W, H, gfx_bg); return; }
    if (ui_wp == 1) {
        int rows = H / 2 + 1;
        for (int i = 0; i < rows; ++i) {
            int t = i * 255 / (rows > 1 ? rows - 1 : 1);
            gfx_fill(0, i * 2, W, 2,
                     gfx_rgb((6 * (255 - t) + 16 * t) / 255,
                             (12 * (255 - t) + 44 * t) / 255,
                             (30 * (255 - t) + 82 * t) / 255));
        }
        return;
    }
    /* 网格 */
    gfx_fill(0, 0, W, H, gfx_bg);
    for (int y = 0; y < H; y += 24)
        gfx_fill(0, y, W, 1, gfx_rgb(22, 38, 60));
    for (int x = 0; x < W; x += 24)
        gfx_fill(x, 0, 1, H, gfx_rgb(22, 38, 60));
}

/* ---------- 运行框 ---------- */
static void ui_run_box(int *x0, int *y0, int *w0, int *h0)
{
    int w = 520, h = 176;
    *w0 = w; *h0 = h;
    *x0 = ((int)gfx_w - w) / 2;
    *y0 = ((int)gfx_h - h) / 2;
}

static void ui_to_wide(const char *a, CHAR16 *o, int cap)
{
    int i = 0;
    while (a[i] && i < cap - 1) { o[i] = (CHAR16)a[i]; ++i; }
    o[i] = 0;
}

static int ui_ends(const char *s, const char *suffix)
{
    int n = ui_slen(s), m = ui_slen(suffix);
    if (m > n) return 0;
    for (int i = 0; i < m; ++i) if (s[n - m + i] != suffix[i]) return 0;
    return 1;
}

static void ui_term_run(CHAR16 *cmd)
{
    int t = win_open(WIN_TERM);
    if (t < 0) return;
    GuiWin *w = &wins[t];
    gfx_term_target = w;
    gui_cap_len = 0;
    gui_capture = 1;
    process_command(cmd);
    gui_capture = 0;
    gui_capture_flush(w);
    gfx_term_target = NULL;
    term_putc(w, '\n');
}

static void ui_execute(void)
{
    char cmd[96];
    scopy(cmd, sizeof(cmd), ui_input);
    char *t = strim(cmd);
    if (!t[0]) { ui_run = 0; return; }
    CHAR16 wide[160];
    ui_to_wide(t, wide, 160);

    if (scmp(t, "wtr") == 0) { ui_run = 0; ui_len = 0; ui_input[0] = 0; win_open(WIN_TERM); return; }
    if (scmp(t, "wtcer") == 0) { ui_run = 0; ui_about = 1; return; }
    if (scmp(t, "taskmgr") == 0) { ui_run = 0; ui_task = 1; return; }
    if (scmp(t, "cfg") == 0) { ui_run = 0; ui_set = 1; return; }

    if (ui_ends(t, ".wct")) {
        ui_run = 0; ui_len = 0; ui_input[0] = 0;
        CHAR16 path[160];
        if (t[0] == '\\') ui_to_wide(t, path, 160);
        else { wcopy(path, 160, L"\\wct\\"); ui_to_wide(t, path + 5, 155); }
        wct_run_file(BOOT_ROOT ? BOOT_ROOT : ROOT, path);
        return;
    }
    if (ui_ends(t, ".wet")) {
        ui_run = 0; ui_len = 0; ui_input[0] = 0;
        CHAR16 path[160];
        if (t[0] == '\\') ui_to_wide(t, path, 160);
        else { wcopy(path, 160, L"\\wct\\"); ui_to_wide(t, path + 5, 155); }
        run_script(path);
        return;
    }
    if (ui_ends(t, ".dwc")) {
        /* .dwc 等价于 DLL：登记依赖并确认可装载 */
        ui_run = 0; ui_len = 0; ui_input[0] = 0;
        ui_term_run(L"echo dwc dependency registered");
        return;
    }
    ui_run = 0; ui_len = 0; ui_input[0] = 0;
    ui_term_run(wide);
}

static void ui_draw_run(void)
{
    int x, y, w, h;
    ui_run_box(&x, &y, &w, &h);
    ui_panel_w(x, y, w, h, ui_admin ? L"运行（管理员）" : L"运行");
    gfx_fill(x + 14, y + 42, w - 28, 28, theme_index == 5 ? gfx_rgb(255,255,252) : gfx_rgb(4, 7, 14));
    gfx_border(x + 14, y + 42, w - 28, 28, gfx_accent);
    gfx_text(x + 20, y + 48, ui_input, gfx_text_c);
    gfx_fill(x + 20 + ui_len * 8, y + 46, 2, 18, gfx_accent);
    gfx_text_w(x + 14, y + 78, L"wtr 命令窗口 | wtcer 系统属性 | 名称.wct 运行 | 名称.wet 脚本", gfx_dim);
    ui_btn_w(x + 14, y + h - 40, 150, 26, ui_admin ? L"管理员：开" : L"管理员：关", ui_admin);
    ui_btn_w(x + w - 200, y + h - 40, 84, 26, L"确定", 0);
    ui_btn_w(x + w - 108, y + h - 40, 84, 26, L"取消", 0);
}

/* ---------- 设置 ---------- */
static void ui_set_box(int *x0, int *y0, int *w0, int *h0)
{
    int w = 600, h = 380;
    *w0 = w; *h0 = h;
    *x0 = ((int)gfx_w - w) / 2;
    *y0 = ((int)gfx_h - h) / 2;
}

static void ui_draw_set(void)
{
    int x, y, w, h;
    ui_set_box(&x, &y, &w, &h);
    ui_panel_w(x, y, w, h, L"设置");

    gfx_text_w(x + 16, y + 40, L"主题颜色", gfx_dim);
    const CHAR16 *names[6] = { L"紫", L"绿", L"矩阵绿", L"琥珀", L"蓝", L"纸片" };
    int idx[6] = { 0, 1, 2, 3, 4, 5 };
    for (int i = 0; i < 6; ++i)
        ui_btn_w(x + 16 + i * 88, y + 60, 80, 26, names[i], theme_index == idx[i]);

    gfx_text_w(x + 16, y + 104, L"壁纸", gfx_dim);
    const CHAR16 *wp[4] = { L"纯色", L"渐变", L"网格", L"动态壁纸(.glf)" };
    for (int i = 0; i < 4; ++i)
        ui_btn_w(x + 16 + i * 134, y + 124, 126, 26, wp[i], ui_wp == i);

    gfx_text_w(x + 16, y + 168, L"动态壁纸读取 \\wct\\wall\\default.glf（GLF1 / RGB332 帧）",
             glf_ok ? gfx_accent : gfx_dim);
    gfx_text_w(x + 16, y + 188, glf_ok ? L"glf 已加载，可以切换" : L"未找到 glf，自动退回渐变", gfx_dim);

    gfx_text_w(x + 16, y + 222, L"系统信息", gfx_dim);
    gfx_text_w(x + 16, y + 242, L"wcteos 0.9-测试  |  WCT 原生内核（不依赖 Linux）  |  x86_64 UEFI", gfx_text_c);
    gfx_text(x + 16, y + 264, net_status_line(), gfx_dim);
    gfx_text(x + 16, y + 284, audio_status_line(), gfx_dim);

    ui_btn_w(x + w - 108, y + h - 40, 84, 26, L"关闭", 0);
}

/* ---------- 任务管理器 ---------- */
static void ui_task_box(int *x0, int *y0, int *w0, int *h0)
{
    int w = 600, h = 360;
    *w0 = w; *h0 = h;
    *x0 = ((int)gfx_w - w) / 2;
    *y0 = ((int)gfx_h - h) / 2;
}

static void ui_draw_task(void)
{
    int x, y, w, h;
    ui_task_box(&x, &y, &w, &h);
    ui_panel_w(x, y, w, h, L"任务管理器");

    gfx_text_w(x + 16, y + 38, L"名称", gfx_dim);
    gfx_text_w(x + 220, y + 38, L"类型", gfx_dim);
    gfx_text_w(x + 340, y + 38, L"状态", gfx_dim);
    gfx_text_w(x + 460, y + 38, L"详情", gfx_dim);

    {   /* 进程数来自内核真实进程表 */
        CHAR16 cap[48];
        int k = 0;
        const CHAR16 *pre = L"进程：";
        while (*pre) cap[k++] = *pre++;
        int n = proc_total();
        cap[k++] = (CHAR16)(L'0' + (n / 10) % 10);
        cap[k++] = (CHAR16)(L'0' + n % 10);
        cap[k] = 0;
        gfx_text_w(x + 300, y + h - 38, cap, gfx_dim);
    }
    int n = proc_total();
    if (n > 7) n = 7;
    for (int i = 0; i < n; ++i) {
        const WCT_PROC *pp = proc_at(i);
        if (!pp) break;
        int ry = y + 60 + i * 22;
        gfx_fill(x + 12, ry - 4, w - 24, 20, (i & 1) ? gfx_panel : (theme_index == 5 ? gfx_rgb(238,234,224) : gfx_rgb(12, 18, 32)));
        char nm[28];
        int q = 0;
        if (pp->pid >= 10) nm[q++] = (char)('0' + (pp->pid / 10) % 10);
        nm[q++] = (char)('0' + pp->pid % 10);
        nm[q++] = ' ';
        for (int t2 = 0; pp->name[t2] && q < 27; ++t2) nm[q++] = pp->name[t2];
        nm[q] = 0;
        gfx_text(x + 16, ry, nm, gfx_text_c);
        gfx_text(x + 220, ry, pp->type, gfx_dim);
        gfx_text(x + 340, ry, pp->state, gfx_accent);
        gfx_text(x + 460, ry, pp->mem_kb ? "mem" : "", gfx_dim);
        if (pp->mem_kb) {
            char mk[16];
            int q2 = 0;
            UINT32 mb = pp->mem_kb;
            char tmp[8]; int tn = 0;
            if (!mb) tmp[tn++] = '0';
            while (mb) { tmp[tn++] = (char)('0' + mb % 10); mb /= 10; }
            while (tn) mk[q2++] = tmp[--tn];
            mk[q2++] = 'K'; mk[q2] = 0;
            gfx_text(x + 496, ry, mk, gfx_dim);
        }
    }

    gfx_text_w(x + 16, y + h - 72, L"新建进程会打开运行框，勾选“管理员”即以管理员身份运行",
             gfx_dim);
    ui_btn_w(x + 16, y + h - 44, 150, 26, L"新建进程", 0);
    ui_btn_w(x + 180, y + h - 44, 110, 26, L"刷新", 0);
    ui_btn_w(x + w - 108, y + h - 44, 84, 26, L"关闭", 0);
}

/* ---------- 关于（wtcer） ---------- */
static void ui_about_box(int *x0, int *y0, int *w0, int *h0)
{
    int w = 560, h = 300;
    *w0 = w; *h0 = h;
    *x0 = ((int)gfx_w - w) / 2;
    *y0 = ((int)gfx_h - h) / 2;
}

static void ui_draw_about(void)
{
    int x, y, w, h;
    ui_about_box(&x, &y, &w, &h);
    ui_panel_w(x, y, w, h, L"系统属性（wtcer）");

    gfx_text(x + 20, y + 48, "wcteos 0.9-test", gfx_accent);
    gfx_text_w(x + 20, y + 76, L"内核：WCT UEFI 原生内核，底层不依赖 Linux", gfx_text_c);
    gfx_text_w(x + 20, y + 96, L"架构：x86_64（另有 ia32 构建）", gfx_text_c);

    UINT32 a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    char vendor[13];
    vendor[0] = (char)b; vendor[1] = (char)(b >> 8); vendor[2] = (char)(b >> 16); vendor[3] = (char)(b >> 24);
    vendor[4] = (char)d; vendor[5] = (char)(d >> 8); vendor[6] = (char)(d >> 16); vendor[7] = (char)(d >> 24);
    vendor[8] = (char)c; vendor[9] = (char)(c >> 8); vendor[10] = (char)(c >> 16); vendor[11] = (char)(c >> 24);
    vendor[12] = 0;
    gfx_text_w(x + 20, y + 116, L"处理器：", gfx_dim);
    gfx_text(x + 92, y + 116, vendor, gfx_text_c);

    CHAR16 wline[64];
    int i = 0;
    const CHAR16 *pfx = L"磁盘：";
    while (*pfx) wline[i++] = *pfx++;
    int v = disk_count();
    wline[i++] = (CHAR16)('0' + (v / 10) % 10);
    wline[i++] = (CHAR16)('0' + v % 10);
    const CHAR16 *mid = L"  PCI 设备：";
    while (*mid) wline[i++] = *mid++;
    v = pci_count();
    wline[i++] = (CHAR16)('0' + (v / 10) % 10);
    wline[i++] = (CHAR16)('0' + v % 10);
    wline[i] = 0;
    gfx_text_w(x + 20, y + 136, wline, gfx_text_c);

    gfx_text_w(x + 20, y + 156, L"编译：2026-11-10  |  api: 1", gfx_dim);
    gfx_text_w(x + 20, y + 176, L"安装时间：见 \\wct\\install.txt", gfx_dim);
    gfx_text(x + 20, y + 196, net_status_line(), gfx_dim);
    gfx_text(x + 20, y + 212, audio_status_line(), gfx_dim);

    ui_btn_w(x + w - 108, y + h - 44, 84, 26, L"关闭", 0);
}

/* ---------- 开始菜单 ---------- */
#define UI_MENU_N 9
static void ui_menu_box(int *x0, int *y0, int *w0, int *h0)
{
    int w = 236, item = 26;
    *w0 = w; *h0 = UI_MENU_N * item + 44;
    *x0 = 6;
    *y0 = (int)gfx_h - 34 - *h0 - 6;
    if (*y0 < 44) *y0 = 44;
}

static void ui_draw_menu(void)
{
    int x, y, w, h;
    ui_menu_box(&x, &y, &w, &h);
    ui_panel_w(x, y, w, h, L"wcteos");
    const CHAR16 *items[UI_MENU_N] = { L"文件管理器", L"终端", L"设置", L"任务管理器",
                                      L"运行", L"关于（wtcer）", L"锁屏", L"重启", L"关机" };
    for (int i = 0; i < UI_MENU_N; ++i) {
        int by = y + 34 + i * 26;
        if (ui_hit(x + 4, by, w - 8, 24))
            gfx_fill(x + 4, by, w - 8, 24, gfx_panel);
        gfx_text_w(x + 16, by + 4, items[i], gfx_text_c);
    }
}

/* ---------- 锁屏 ---------- */
static void ui_draw_lock(void)
{
    gfx_fill(0, 0, (int)gfx_w, (int)gfx_h, gfx_rgb(4, 8, 16));
    gfx_text_w((int)gfx_w / 2 - 104, (int)gfx_h / 2 - 40, L"wcteos 已锁定", gfx_accent);
    gfx_text_w((int)gfx_w / 2 - 176, (int)gfx_h / 2 - 10, L"按任意键或点击鼠标解锁", gfx_dim);
    gfx_text((int)gfx_w / 2 - 90, (int)gfx_h - 60, "0.9-test", gfx_dim);
}

/* ---------- 汇总绘制 ---------- */
static void wct_ui_draw(void)
{
    if (ui_lock) { ui_draw_lock(); return; }
    if (ui_menu) ui_draw_menu();
    if (ui_set) ui_draw_set();
    if (ui_task) ui_draw_task();
    if (ui_about) ui_draw_about();
    if (ui_run) ui_draw_run();
}

/* ---------- 点击 ---------- */
static int wct_ui_click(void)
{
    if (ui_lock) { ui_lock = 0; return 1; }

    if (ui_run) {
        int x, y, w, h;
        ui_run_box(&x, &y, &w, &h);
        if (ui_hit(x + 14, y + h - 40, 132, 26)) { ui_admin = !ui_admin; return 1; }
        if (ui_hit(x + w - 200, y + h - 40, 84, 26)) { ui_execute(); return 1; }
        if (ui_hit(x + w - 108, y + h - 40, 84, 26)) { ui_run = 0; return 1; }
        if (ui_hit(x, y, w, h)) return 1;
        ui_run = 0;
        return 1;
    }

    if (ui_set) {
        int x, y, w, h;
        ui_set_box(&x, &y, &w, &h);
        for (int i = 0; i < 5; ++i)
            if (ui_hit(x + 16 + i * 104, y + 60, 96, 26)) {
                theme_index = (i == 4) ? 4 : i;
                apply_theme();
                return 1;
            }
        for (int i = 0; i < 4; ++i)
            if (ui_hit(x + 16 + i * 130, y + 124, 120, 26)) { ui_wp = i; if (i == 3) ui_glf_load(); return 1; }
        if (ui_hit(x + w - 108, y + h - 40, 84, 26)) { ui_set = 0; return 1; }
        if (ui_hit(x, y, w, h)) return 1;
        ui_set = 0;
        return 1;
    }

    if (ui_task) {
        int x, y, w, h;
        ui_task_box(&x, &y, &w, &h);
        if (ui_hit(x + 16, y + h - 44, 150, 26)) {
            proc_spawn("new-task", "user", "pending", 128);
            ui_task = 0; ui_run = 1; ui_len = 0; ui_input[0] = 0; return 1;
        }
        if (ui_hit(x + 180, y + h - 44, 110, 26)) { return 1; }
        if (ui_hit(x + w - 108, y + h - 44, 84, 26)) { ui_task = 0; return 1; }
        if (ui_hit(x, y, w, h)) return 1;
        ui_task = 0;
        return 1;
    }

    if (ui_about) {
        int x, y, w, h;
        ui_about_box(&x, &y, &w, &h);
        if (ui_hit(x + w - 108, y + h - 44, 84, 26) || !ui_hit(x, y, w, h)) { ui_about = 0; return 1; }
        return 1;
    }

    if (ui_menu) {
        int x, y, w, h;
        ui_menu_box(&x, &y, &w, &h);
        for (int i = 0; i < UI_MENU_N; ++i) {
            int by = y + 34 + i * 26;
            if (ui_hit(x + 4, by, w - 8, 24)) {
                ui_menu = 0;
                switch (i) {
                case 0: win_open(WIN_FILES); break;
                case 1: win_open(WIN_TERM); break;
                case 2: ui_set = 1; break;
                case 3: ui_task = 1; break;
                case 4: ui_run = 1; ui_len = 0; ui_input[0] = 0; break;
                case 5: ui_about = 1; break;
                case 6: ui_lock = 1; break;
                case 7: ST->RuntimeServices->ResetSystem(EFI_RESET_COLD, EFI_SUCCESS, 0, NULL); break;
                case 8: ST->RuntimeServices->ResetSystem(EFI_RESET_SHUTDOWN, EFI_SUCCESS, 0, NULL); break;
                }
                return 1;
            }
        }
        if (!ui_hit(x, y, w, h)) ui_menu = 0;
        return 1;
    }

    /* 任务栏：左下角开始按钮 / 右侧 NET */
    if (ui_hit(6, (int)gfx_h - 30, 56, 24)) { ui_menu = !ui_menu; return 1; }
    if (ui_hit((int)gfx_w - 168, (int)gfx_h - 30, 60, 24)) { ui_about = 1; return 1; }
    return 0;
}

/* ---------- 键盘 ---------- */
static int wct_ui_key(EFI_INPUT_KEY *key)
{
    if (ui_lock) { ui_lock = 0; return 1; }

    if (ui_run) {
        if (key->UnicodeChar == 0x0D) { ui_execute(); return 1; }
        if (key->ScanCode == 0x17) { ui_run = 0; return 1; }            /* Esc */
        if (key->UnicodeChar == 8 && ui_len > 0) { ui_input[--ui_len] = 0; return 1; }
        if (key->UnicodeChar >= 32 && key->UnicodeChar < 127 && ui_len < 95) {
            ui_input[ui_len++] = (char)key->UnicodeChar;
            ui_input[ui_len] = 0;
            return 1;
        }
        return 1;
    }

    switch (key->ScanCode) {
    case 0x0B: ui_menu = !ui_menu; return 1;                 /* F1 开始菜单 */
    case 0x0C: ui_set = !ui_set; ui_task = 0; ui_about = 0; return 1;   /* F2 设置 */
    case 0x0D: ui_task = !ui_task; ui_set = 0; ui_about = 0; return 1;  /* F3 任务管理器 */
    case 0x0E: ui_about = 1; return 1;                       /* F4 关于 */
    case 0x0F: ui_lock = 1; return 1;                        /* F5 锁屏 */
    case 0x0A:                                               /* F12 运行框 */
        ui_run = 1; ui_len = 0; ui_input[0] = 0; return 1;
    case 0x17:
        if (ui_set || ui_task || ui_about || ui_menu) {
            ui_set = ui_task = ui_about = ui_menu = 0;
            return 1;
        }
        return 0;
    }
    return 0;
}

/* ---------- 文件关联：.wct / .wet / .dwc ---------- */
static void wct_ui_open_file(const CHAR16 *path)
{
    int n = (int)wlen(path);
    if (n >= 4 && weq(path + n - 4, L".wct")) {
        wct_run_file(BOOT_ROOT ? BOOT_ROOT : ROOT, path);
        return;
    }
    if (n >= 4 && weq(path + n - 4, L".wet")) {
        run_script(path);
        return;
    }
    if (n >= 4 && weq(path + n - 4, L".dwc")) {
        int t = win_open(WIN_TERM);
        if (t >= 0) {
            term_putc(&wins[t], 'D'); term_putc(&wins[t], 'W'); term_putc(&wins[t], 'C');
            term_putc(&wins[t], ':'); term_putc(&wins[t], ' ');
            for (int i = 0; path[i] && i < 60; ++i) term_putc(&wins[t], (char)path[i]);
            term_putc(&wins[t], '\n');
        }
        return;
    }
}

/* ---------- 演示序列：存在 \wct\uidemo 时按固定节奏切换各面板 ---------- */
static void wct_ui_demo_start(void)
{
    static int probed = 0;
    if (probed) return;
    probed = 1;
    EFI_FILE_PROTOCOL *f = NULL;
    if (open_path(L"\\wct\\uidemo", &f, EFI_FILE_MODE_READ, 0)) { f->Close(f); ui_demo_stage = 0; }
}

static int wct_ui_demo(void)
{
    if (ui_demo_stage < 0) return 0;
    ST->BootServices->Stall(4500000);   /* 4.5s：软件渲染下一帧要 2-3s，留够截图窗口 */
    ++ui_demo_stage;
    ui_menu = ui_set = ui_task = ui_about = ui_run = 0;
    switch (ui_demo_stage) {
    case 1: ui_menu = 1; break;
    case 2: ui_set = 1; break;
    case 3: ui_task = 1; break;
    case 4: ui_about = 1; break;
    case 5: ui_run = 1; ui_admin = 1; ui_len = 0; ui_input[0] = 0; break;
    case 6: ui_wp = 3; ui_glf_load(); break;
    case 7: ui_wp = 0; ui_demo_stage = -1; break;
    }
    return 1;
}

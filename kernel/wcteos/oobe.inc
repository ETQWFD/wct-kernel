/* oobe.inc - 首次开机设置向导（OOBE）

   进入条件：\wct\oobe.cfg 不存在，且没有 \wct\nooobe 标记文件。
   完成后写 \wct\oobe.cfg，供后续启动读取。
   键盘操作：↑↓ 选择，Enter 下一步，Esc 返回上一步。
   注意：本版本只做了 简体中文 / 繁體中文 / English 三种界面文案，
   其余语言可选中并记录到配置，界面文案回落到 English。 */

#define OOBE_LANGS 48
#define OOBE_VISIBLE 10

typedef struct { const CHAR16 *name; const char *code; } OobeLang;

static const OobeLang oobe_langs[OOBE_LANGS] = {
    { L"简体中文", "zh-CN" }, { L"繁體中文", "zh-TW" }, { L"English (US)", "en-US" },
    { L"English (UK)", "en-GB" }, { L"日本語", "ja-JP" }, { L"한국어", "ko-KR" },
    { L"Deutsch", "de-DE" }, { L"Français", "fr-FR" }, { L"Español", "es-ES" },
    { L"Italiano", "it-IT" }, { L"Português (BR)", "pt-BR" }, { L"Português (PT)", "pt-PT" },
    { L"Русский", "ru-RU" }, { L"Українська", "uk-UA" }, { L"Polski", "pl-PL" },
    { L"Čeština", "cs-CZ" }, { L"Slovenčina", "sk-SK" }, { L"Magyar", "hu-HU" },
    { L"Română", "ro-RO" }, { L"Български", "bg-BG" }, { L"Ελληνικά", "el-GR" },
    { L"Türkçe", "tr-TR" }, { L"العربية", "ar-SA" }, { L"עברית", "he-IL" },
    { L"فارسی", "fa-IR" }, { L"हिन्दी", "hi-IN" }, { L"বাংলা", "bn-BD" },
    { L"ไทย", "th-TH" }, { L"Tiếng Việt", "vi-VN" }, { L"Bahasa Indonesia", "id-ID" },
    { L"Bahasa Melayu", "ms-MY" }, { L"Filipino", "fil-PH" }, { L" Nederlands", "nl-NL" },
    { L"Svenska", "sv-SE" }, { L"Norsk", "nb-NO" }, { L"Dansk", "da-DK" },
    { L"Suomi", "fi-FI" }, { L"Íslenska", "is-IS" }, { L"Eesti", "et-EE" },
    { L"Latviešu", "lv-LV" }, { L"Lietuvių", "lt-LT" }, { L"Slovenščina", "sl-SI" },
    { L"Hrvatski", "hr-HR" }, { L"Српски", "sr-RS" }, { L"Македонски", "mk-MK" },
    { L"Shqip", "sq-AL" }, { L"ქართული", "ka-GE" }, { L"Afrikaans", "af-ZA" }
};

static int oobe_lang;          /* 0 简中 1 繁中 2 en，其余回落 en */
static int oobe_lang_index;
static int oobe_ime;           /* 0 内置默认输入法 1 从文件添加 */
static CHAR16 oobe_user[32];
static int oobe_user_len;
static CHAR16 oobe_pass[32];
static int oobe_pass_len;
static int oobe_agree;
static int oobe_admin;
static int oobe_wifi_skip;
static int oobe_warn;
static int oobe_step;
static int oobe_scroll;
static int oobe_ticks;

/* 文案表：简中 / 繁中 / English 三套，索引见 OO_* 常量 */
enum {
    OO_TITLE, OO_SUB, OO_NEXT, OO_BACK, OO_SKIP, OO_STEP1, OO_STEP2, OO_STEP3,
    OO_STEP4, OO_STEP5, OO_STEP6, OO_STEP7, OO_LANG_HEAD, OO_LANG_TIP,
    OO_IME_HEAD, OO_IME_A, OO_IME_B, OO_IME_TIP, OO_USER_HEAD, OO_USER_TIP,
    OO_PASS_HEAD, OO_PASS_TIP, OO_WIFI_HEAD, OO_WIFI_NONE, OO_WIFI_TIP,
    OO_LIC_HEAD, OO_LIC_BODY, OO_LIC_AGREE, OO_PROG, OO_PROG2, OO_DONE, OO_KEYS
};
static const CHAR16 *oo_zhcn[] = {
    L"首次开机设置", L"设置完成后进入桌面", L"下一步", L"上一步", L"跳过",
    L"语言", L"输入法", L"账户", L"密码", L"网络", L"许可", L"完成",
    L"选择系统语言", L"↑↓ 选择，Enter 确认",
    L"选择输入法", L"使用系统内置输入法（默认）", L"从文件添加 .wct 输入法", L"内置输入法可直接输入中文；外部输入法需要厂商提供 .wct 模块",
    L"创建账户", L"输入你的用户名（普通用户组，可在设置里加入管理员组）",
    L"设置密码", L"可留空，直接下一步",
    L"连接网络", L"未检测到无线网卡（本版本没有无线驱动）", L"可跳过，进入系统后再配置",
    L"许可协议", L"本项目由 ETQWFD 开发，内核基础来自 wct-kernel（作者 aqtwWCT95）。按 MIT 许可证提供，按现状交付，不附带担保。", L"我已阅读并同意",
    L"正在应用设置…", L"正在创建用户配置…", L"设置完成", L"↑↓ 选择 · Enter 下一步 · Esc 返回"
};
static const CHAR16 *oo_zhtw[] = {
    L"首次開機設定", L"設定完成後進入桌面", L"下一步", L"上一步", L"跳過",
    L"語言", L"輸入法", L"帳戶", L"密碼", L"網路", L"授權", L"完成",
    L"選擇系統語言", L"↑↓ 選擇，Enter 確認",
    L"選擇輸入法", L"使用系統內建輸入法（預設）", L"從檔案加入 .wct 輸入法", L"內建輸入法可直接輸入中文；外部輸入法需要廠商提供 .wct 模組",
    L"建立帳戶", L"輸入你的使用者名稱（一般使用者群組，可在設定裡加入管理員群組）",
    L"設定密碼", L"可留空，直接下一步",
    L"連線網路", L"未偵測到無線網卡（本版本沒有無線驅動）", L"可跳過，進入系統後再設定",
    L"授權條款", L"本專案由 ETQWFD 開發，內核基礎來自 wct-kernel（作者 aqtwWCT95）。依 MIT 授權提供，依現況交付，不附帶擔保。", L"我已閱讀並同意",
    L"正在套用設定…", L"正在建立使用者設定…", L"設定完成", L"↑↓ 選擇 · Enter 下一步 · Esc 返回"
};
static const CHAR16 *oo_en[] = {
    L"First-time setup", L"Setup finishes, then the desktop starts", L"Next", L"Back", L"Skip",
    L"Language", L"Input method", L"Account", L"Password", L"Network", L"License", L"Finish",
    L"Choose system language", L"Up/Down to choose, Enter to confirm",
    L"Input method", L"Use built-in input method (default)", L"Add a .wct input method from file", L"Built-in IME types Chinese directly; external IME needs a vendor .wct module",
    L"Create account", L"Type a user name (standard user; can join Administrators later in Settings)",
    L"Set password", L"May be left empty",
    L"Connect network", L"No wireless adapter detected (no Wi-Fi driver in this build)", L"Skip and configure later",
    L"License", L"Developed by ETQWFD; kernel base from wct-kernel by aqtwWCT95. MIT License, as-is without warranty.", L"I have read and agree",
    L"Applying settings...", L"Creating user profile...", L"Setup complete", L"Up/Down choose · Enter next · Esc back"
};

static const CHAR16 *oo_str(int id)
{
    if (oobe_lang == 0) return oo_zhcn[id];
    if (oobe_lang == 1) return oo_zhtw[id];
    return oo_en[id];
}

/* ---------- 小工具 ---------- */

static int oo_w(const CHAR16 *s)
{
    int w = 0;
    while (s && *s) { w += (*s <= 126) ? 8 : 16; ++s; }
    return w;
}

static void oo_text(int x, int y, const CHAR16 *s, UINT32 c)
{
    gfx_text_w(x, y, s, c);
}

static void oo_text_c(int cx, int y, const CHAR16 *s, UINT32 c)
{
    oo_text(cx - oo_w(s) / 2, y, s, c);
}

static void oo_text_wrap(int x, int y, int maxw, const CHAR16 *s, UINT32 c)
{
    int cx = x, cy = y;
    while (s && *s) {
        int w = (*s <= 126) ? 8 : 16;
        int punct = (*s == L',' || *s == 0xFF0C || *s == 0x3002 || *s == 0xFF1B || *s == 0xFF1A);
        if (!punct && cx + w > x + maxw) { cx = x; cy += 22; }
        CHAR16 one[2]; one[0] = *s; one[1] = 0;
        gfx_text_w(cx, cy, one, c);
        cx += w;
        ++s;
    }
}

static void oo_box(int x, int y, int w, int h, UINT32 c)
{
    gfx_fill(x, y, w, h, c);
}

static void oo_round(int x, int y, int w, int h, UINT32 c)
{
    /* 用矩形拼圆角，避免引入新绘制原语 */
    gfx_fill(x + 3, y, w - 6, h, c);
    gfx_fill(x, y + 3, w, h - 6, c);
}

static void oo_key_poll(CHAR16 *uni, UINT16 *scan)
{
    EFI_INPUT_KEY k;
    *uni = 0; *scan = 0;
    if (!EFI_IS_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &k))) {
        *uni = k.UnicodeChar;
        *scan = k.ScanCode;
    }
}

/* ---------- 绘制 ---------- */

static void oobe_draw(void)
{
    UINT32 bg = gfx_rgb(246, 244, 238);
    UINT32 card = gfx_rgb(255, 255, 255);
    UINT32 ink = gfx_rgb(38, 40, 44);
    UINT32 sub = gfx_rgb(110, 112, 120);
    UINT32 accent = gfx_rgb(52, 86, 138);
    UINT32 line = gfx_rgb(222, 218, 208);
    int W = (int)gfx_w, H = (int)gfx_h;
    int cx = W / 2;

    oo_box(0, 0, W, H, bg);

    if (oobe_step == 6) {
        static const CHAR16 *poems[] = {
            L"千门万户曈曈日，总把新桃换旧符",
            L"海内存知己，天涯若比邻",
            L"我们正在帮你完成最后的操作",
            L"这个操作可能需要一两分钟，请耐心等待，请勿关闭电脑",
            L"正在应用时间、区域与账户设置"
        };
        oo_text_c(cx, 60, oo_str(OO_TITLE), ink);
        oo_text_c(cx, H / 2 - 130, L"正在完成最后的设置", ink);
        oo_text_c(cx, H / 2 - 70, poems[(oobe_ticks / 240) % 5], accent);
        int bx = cx - 200, by = H / 2 + 10, bw = 400;
        gfx_fill(bx, by, bw, 8, line);
        int fill = (int)((long long)oobe_ticks * bw / 4500);
        if (fill > bw) fill = bw;
        gfx_fill(bx, by, fill, 8, accent);
        oo_text_c(cx, by + 34, L"请勿关闭电脑，完成后将自动进入桌面", sub);
        gfx_present();
        return;
    }

    oo_text(cx - 110, 46, oo_str(OO_TITLE), ink);
    oo_text_c(cx, 74, oo_str(OO_SUB), sub);

    /* 步骤条 */
    {
        int sx = cx - 7 * 46;
        for (int i = 0; i < 7; ++i) {
            static const int ids[7] = { OO_STEP1, OO_STEP2, OO_STEP3, OO_STEP4, OO_STEP5, OO_STEP6, OO_STEP7 };
            UINT32 c = (i <= oobe_step) ? accent : line;
            oo_round(sx + i * 46, 104, 34, 26, c);
            CHAR16 num[2]; num[0] = (CHAR16)(L'1' + i); num[1] = 0;
            gfx_text_w(sx + i * 46 + 13, 109, num, i <= oobe_step ? gfx_rgb(255,255,255) : sub);
            oo_text(sx + i * 46 + 22, 136, oo_str(ids[i]), i == oobe_step ? accent : sub);
        }
    }

    /* 主卡片 */
    int px = (int)gfx_w / 8, py = 176, pw = (int)gfx_w * 3 / 4, ph = (int)gfx_h - 300;
    oo_box(px, py, pw, ph, card);
    gfx_border(px, py, pw, ph, line);

    int tx = px + 40, ty = py + 34;

    if (oobe_step == 0) {
        oo_text(tx, ty, oo_str(OO_LANG_HEAD), ink);
        oo_text(tx, ty + 26, oo_str(OO_LANG_TIP), sub);
        int lx = tx, ly = ty + 64, lw = pw - 80;
        gfx_fill(lx, ly, lw, 26 * OOBE_VISIBLE + 2, gfx_rgb(250, 250, 252));
        gfx_border(lx, ly, lw, 26 * OOBE_VISIBLE + 2, line);
        for (int i = 0; i < OOBE_VISIBLE; ++i) {
            int idx = oobe_scroll + i;
            if (idx >= OOBE_LANGS) break;
            int y = ly + 1 + i * 26;
            if (idx == oobe_lang_index) gfx_fill(lx + 1, y, lw - 2, 26, gfx_rgb(225, 232, 245));
            oo_text(lx + 12, y + 5, oobe_langs[idx].name, idx == oobe_lang_index ? accent : ink);
            /* 语言代码用 ASCII 画，避免缺字 */
            gfx_text(lx + lw - 12 - 5 * 8, y + 5, (const char *)oobe_langs[idx].code, sub);
        }
    } else if (oobe_step == 1) {
        oo_text(tx, ty, oo_str(OO_IME_HEAD), ink);
        for (int i = 0; i < 2; ++i) {
            int y = ty + 46 + i * 46;
            int sel = (oobe_ime == i);
            oo_box(tx, y, pw - 80, 38, sel ? gfx_rgb(225, 232, 245) : gfx_rgb(250, 250, 252));
            gfx_border(tx, y, pw - 80, 38, sel ? accent : line);
            gfx_fill(tx + 10, y + 14, 10, 10, sel ? accent : line);
            oo_text(tx + 30, y + 11, oo_str(i ? OO_IME_B : OO_IME_A), ink);
        }
        oo_text(tx, ty + 150, oo_str(OO_IME_TIP), sub);
    } else if (oobe_step == 2) {
        oo_text(tx, ty, oo_str(OO_USER_HEAD), ink);
        oo_text(tx, ty + 26, oo_str(OO_USER_TIP), sub);
        int y = ty + 74;
        oo_box(tx, y, pw - 80, 36, gfx_rgb(250, 250, 252));
        gfx_border(tx, y, pw - 80, 36, accent);
        CHAR16 shown[34]; int n = oobe_user_len; if (n > 30) n = 30;
        for (int i = 0; i < n; ++i) shown[i] = oobe_user[i];
        shown[n] = 0;
        oo_text(tx + 10, y + 9, shown, ink);
        if ((oobe_ticks / 10) % 2 == 0) gfx_fill(tx + 10 + oo_w(shown) + 2, y + 9, 2, 16, accent);
        int ay = ty + 132;
        oo_box(tx, ay + 2, 16, 16, gfx_rgb(255, 255, 255));
        gfx_border(tx, ay + 2, 16, 16, accent);
        if (oobe_admin) gfx_fill(tx + 4, ay + 6, 8, 8, accent);
        oo_text(tx + 26, ay, L"将此账户加入管理员组（Administrators）", ink);
        oo_text(tx, ay + 30, L"不勾选则为普通用户组，之后可在设置里改", sub);
    } else if (oobe_step == 3) {
        oo_text(tx, ty, oo_str(OO_PASS_HEAD), ink);
        oo_text(tx, ty + 26, oo_str(OO_PASS_TIP), sub);
        int y = ty + 74;
        oo_box(tx, y, pw - 80, 36, gfx_rgb(250, 250, 252));
        gfx_border(tx, y, pw - 80, 36, accent);
        CHAR16 stars[34]; int n = oobe_pass_len; if (n > 30) n = 30;
        for (int i = 0; i < n; ++i) stars[i] = L'*';
        stars[n] = 0;
        oo_text(tx + 10, y + 9, stars, ink);
        if ((oobe_ticks / 10) % 2 == 0) gfx_fill(tx + 10 + oo_w(stars) + 2, y + 9, 2, 16, accent);
    } else if (oobe_step == 4) {
        oo_text(tx, ty, oo_str(OO_WIFI_HEAD), ink);
        int y = ty + 40;
        oo_box(tx, y, pw - 80, 60, gfx_rgb(250, 244, 235));
        gfx_border(tx, y, pw - 80, 60, line);
        oo_text(tx + 14, y + 20, oo_str(OO_WIFI_NONE), sub);
        oo_text_wrap(tx, ty + 120, pw - 80, oo_str(OO_WIFI_TIP), sub);
        oo_text(tx, ty + 154, L"本版本尚未识别到可用的无线网卡驱动，", sub);
        oo_text(tx, ty + 178, L"请按空格选择右侧“我没有WIFI”，再按 Enter 继续。", sub);
        if (oobe_wifi_skip) {
            oo_text(tx, ty + 206, L"已选择：我没有WIFI，将跳过网络设置", accent);
        }
        if (oobe_warn) oo_text(tx, ty + 230, L"没有可用的无线网络，请先按空格选择“我没有WIFI”", gfx_rgb(180, 60, 50));
    } else if (oobe_step == 5) {
        oo_text(tx, ty, oo_str(OO_LIC_HEAD), ink);
        int y = ty + 36;
        oo_box(tx, y, pw - 80, 120, gfx_rgb(250, 250, 252));
        gfx_border(tx, y, pw - 80, 120, line);
        oo_text_wrap(tx + 14, y + 18, pw - 80 - 28, oo_str(OO_LIC_BODY), ink);
        int cy = y + 146;
        gfx_fill(tx, cy + 2, 16, 16, gfx_rgb(255, 255, 255));
        gfx_border(tx, cy + 2, 16, 16, accent);
        if (oobe_agree) {
            gfx_fill(tx + 3, cy + 5, 10, 10, accent);
        }
        oo_text(tx + 26, cy, oo_str(OO_LIC_AGREE), ink);
    } else {
        static const CHAR16 *poems[] = {
            L"千门万户曈曈日，总把新桃换旧符",
            L"海内存知己，天涯若比邻",
            L"我们正在帮你完成最后的操作",
            L"这个操作可能需要一两分钟，请耐心等待，请勿关闭电脑",
            L"正在应用时间、区域与账户设置"
        };
        oo_text_c(cx, py + ph / 2 - 110, oo_str(OO_PROG), ink);
        oo_text_c(cx, py + ph / 2 - 60, poems[(oobe_ticks / 240) % 5], accent);
        int bx = cx - 180, by = py + ph / 2 + 10, bw = 360;
        gfx_fill(bx, by, bw, 8, line);
        int fill = (int)((long long)oobe_ticks * bw / 4500);
        if (fill > bw) fill = bw;
        gfx_fill(bx, by, fill, 8, accent);
        oo_text_c(cx, by + 26, oo_str(OO_PROG2), sub);
    }

    oo_text_c(cx, H - 62, oo_str(OO_KEYS), sub);
    /* 按钮 */
    int bw = 120, bh = 34, by = H - 108;
    int bx2 = cx + 90;
    int next_ok = (oobe_step == 4) ? oobe_wifi_skip : 1;
    oo_round(bx2, by, bw, bh, next_ok ? accent : gfx_rgb(190, 186, 176));
    oo_text_c(bx2 + bw / 2, by + 9, oo_str(OO_NEXT), gfx_rgb(255, 255, 255));
    if (oobe_step == 4) {
        oo_text(bx2 + bw + 18, by + 9, L"我没有WIFI", accent);
        if (oobe_wifi_skip) gfx_fill(bx2 + bw + 18, by + 30, 96, 2, accent);
    }
    if (oobe_step > 0) {
        int bx1 = cx - 210;
        oo_round(bx1, by, bw, bh, gfx_rgb(255, 255, 255));
        gfx_border(bx1, by, bw, bh, line);
        oo_text_c(bx1 + bw / 2, by + 9, oo_str(OO_BACK), ink);
    }

    gfx_present();
}

/* ---------- 配置落盘 ---------- */

static void oobe_write_cfg(void)
{
    char buf[512];
    int n = 0;
    const char *k1 = "lang="; for (int i = 0; k1[i]; ++i) buf[n++] = k1[i];
    const char *code = oobe_langs[oobe_lang_index].code;
    for (int i = 0; code[i] && n < 500; ++i) buf[n++] = code[i];
    buf[n++] = '\n';
    const char *k2 = "ime="; for (int i = 0; k2[i]; ++i) buf[n++] = k2[i];
    buf[n++] = (char)('0' + oobe_ime); buf[n++] = '\n';
    const char *k3 = "user="; for (int i = 0; k3[i]; ++i) buf[n++] = k3[i];
    for (int i = 0; i < oobe_user_len && n < 500; ++i) {
        if (oobe_user[i] < 128) buf[n++] = (char)oobe_user[i];
    }
    buf[n++] = '\n';
    const char *k4 = "pass="; for (int i = 0; k4[i]; ++i) buf[n++] = k4[i];
    buf[n++] = oobe_pass_len ? '1' : '0'; buf[n++] = '\n';
    const char *k5 = "wifi="; for (int i = 0; k5[i]; ++i) buf[n++] = k5[i];
    buf[n++] = oobe_wifi_skip ? 's' : 'n';
    buf[n++] = '\n';
    const char *k6 = "admin="; for (int i = 0; k6[i]; ++i) buf[n++] = k6[i];
    buf[n++] = oobe_admin ? '1' : '0';
    buf[n++] = '\n';
    write_bytes(L"\\wct\\oobe.cfg", buf, (UINTN)n);
}

/* ---------- 主循环 ---------- */

static int oobe_run(void)
{
    {
        EFI_FILE_PROTOCOL *nf = NULL;
        if (open_path(L"\\wct\\nooobe", &nf, EFI_FILE_MODE_READ, 0)) { nf->Close(nf); return 0; }
    }

    /* 图形初始化（与 cmd_gui 相同的 GOP 等待策略，但独立一套，避免改桌面代码） */
    EFI_STATUS gs = EFI_IS_ERROR(ST->BootServices->LocateProtocol(&gop_guid, NULL, (VOID **)&gfx));
    for (int wait = 0; wait < 30 && (gs || !gfx || !gfx->Mode || !gfx->Mode->FrameBufferBase); ++wait) {
        ST->BootServices->Stall(100000);
        gs = EFI_IS_ERROR(ST->BootServices->LocateProtocol(&gop_guid, NULL, (VOID **)&gfx));
    }
    if (gs || !gfx || !gfx->Mode || !gfx->Mode->FrameBufferBase) {
        outln(L"oobe: GOP framebuffer not available, skip");
        return 0;
    }
    gfx_w = gfx->Mode->Info->HorizontalResolution;
    gfx_h = gfx->Mode->Info->VerticalResolution;
    gfx_stride = gfx->Mode->Info->PixelsPerScanLine;
    gfx_hw = (UINT32 *)(UINTN)gfx->Mode->FrameBufferBase;
    gfx_back = NULL;
    {
        VOID *buf = NULL;
        UINTN bytes = gfx_stride * gfx_h * 4;
        if (!EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, bytes, &buf)) && buf) {
            gfx_back = (UINT32 *)buf;
            for (UINTN i = 0; i < gfx_stride * gfx_h; ++i) gfx_back[i] = 0;
        }
    }
    gfx_fb = gfx_back ? gfx_back : gfx_hw;
    ST->ConOut->EnableCursor(ST->ConOut, FALSE);

    oobe_step = 0; oobe_scroll = 0; oobe_lang_index = 0; oobe_lang = 0;
    oobe_ime = 0; oobe_user_len = 0; oobe_pass_len = 0; oobe_agree = 0; oobe_ticks = 0;

    oobe_draw();
    int redraw = 0;
    for (;;) {
        CHAR16 uni; UINT16 scan;
        oo_key_poll(&uni, &scan);
        if (uni || scan) {
            redraw = 1;
            if (oobe_step == 6) {
                /* 进度页不接受输入，走完自动完成 */
            } else if (scan == 0x17) {           /* Esc 返回 */
                if (oobe_step > 0) --oobe_step;
            } else if (scan == 0x01) {           /* ↑ */
                if (oobe_step == 0) {
                    if (oobe_lang_index > 0) --oobe_lang_index;
                    if (oobe_lang_index < oobe_scroll) oobe_scroll = oobe_lang_index;
                } else if (oobe_step == 1) oobe_ime = 0;
            } else if (scan == 0x02) {           /* ↓ */
                if (oobe_step == 0) {
                    if (oobe_lang_index + 1 < OOBE_LANGS) ++oobe_lang_index;
                    if (oobe_lang_index >= oobe_scroll + OOBE_VISIBLE) oobe_scroll = oobe_lang_index - OOBE_VISIBLE + 1;
                } else if (oobe_step == 1) oobe_ime = 1;
            } else if (uni == 0x0D) {            /* Enter */
                if (oobe_step == 0) {
                    oobe_lang_index = oobe_lang_index; /* 保留选择 */
                    if (oobe_lang_index == 0) oobe_lang = 0;
                    else if (oobe_lang_index == 1) oobe_lang = 1;
                    else if (oobe_lang_index == 2) oobe_lang = 2;
                    else oobe_lang = 2;
                    ++oobe_step;
                } else if (oobe_step == 1) {
                    ++oobe_step;
                } else if (oobe_step == 2) {
                    if (oobe_user_len > 0) ++oobe_step;
                } else if (oobe_step == 3) {
                    ++oobe_step;
                } else if (oobe_step == 4) {
                    if (oobe_wifi_skip) ++oobe_step; else oobe_warn = 1;
                } else if (oobe_step == 5) {
                    if (oobe_agree) { ++oobe_step; oobe_ticks = 0; }
                }
            } else if (oobe_step == 2 && uni == L' ') {
                oobe_admin = !oobe_admin;
            } else if (oobe_step == 4 && uni == L' ') {
                oobe_wifi_skip = 1;
                oobe_warn = 0;
            } else if (oobe_step == 5 && uni == L' ') {
                oobe_agree = !oobe_agree;
            } else if (uni >= 0x20 && uni < 0x7F) {
                if (oobe_step == 2 && oobe_user_len < 20) oobe_user[oobe_user_len++] = uni;
                else if (oobe_step == 3 && oobe_pass_len < 20) oobe_pass[oobe_pass_len++] = uni;
            } else if (uni == 0x08) {            /* 退格 */
                if (oobe_step == 2 && oobe_user_len > 0) --oobe_user_len;
                else if (oobe_step == 3 && oobe_pass_len > 0) --oobe_pass_len;
            }
        }

        if (oobe_step == 6) {
            ++oobe_ticks;
            redraw = 1;
            if (oobe_ticks > 4500) {
                oobe_write_cfg();
                break;
            }
        }
        if (redraw) { oobe_draw(); redraw = 0; }
        ST->BootServices->Stall(20000);
    }

    ST->ConOut->EnableCursor(ST->ConOut, TRUE);
    if (gfx_back) { ST->BootServices->FreePool(gfx_back); gfx_back = NULL; gfx_fb = NULL; }
    return 1;
}

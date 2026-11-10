#include "font8x16.h"

/* WCT-OS windowed GUI with Win95-style drag-outline.

   Mouse priority:
     1) EFI_SIMPLE_POINTER_PROTOCOL
     2) EFI_ABSOLUTE_POINTER_PROTOCOL
     3) EFI_USB_IO_PROTOCOL HID boot (last resort)

   Drag: window stays put, a 1px white outline follows the mouse; the window
   is actually moved (and one full redraw happens) only when the button is
   released. F11 toggles X/Y inversion for firmware that reports them reversed. */

typedef struct { UINT8 RequestType, Request; UINT16 Value, Index, Length; } UsbDeviceRequest;
typedef struct __attribute__((packed)) {
    UINT8 Length, DescriptorType, InterfaceNumber, AlternateSetting, NumEndpoints;
    UINT8 InterfaceClass, InterfaceSubClass, InterfaceProtocol, Interface;
} UsbInterfaceDescriptor;
typedef struct __attribute__((packed)) {
    UINT8 Length, DescriptorType, EndpointAddress, Attributes;
    UINT16 MaxPacketSize; UINT8 Interval;
} UsbEndpointDescriptor;
typedef struct EFI_USB_IO_PROTOCOL EFI_USB_IO_PROTOCOL;
struct EFI_USB_IO_PROTOCOL {
    EFI_STATUS (EFIAPI *UsbControlTransfer)(EFI_USB_IO_PROTOCOL *, UsbDeviceRequest *, UINT32, UINT32, VOID *, UINTN, UINT32 *);
    EFI_STATUS (EFIAPI *UsbBulkTransfer)(EFI_USB_IO_PROTOCOL *, UINT8, VOID *, UINTN *, UINTN, UINT32 *);
    EFI_STATUS (EFIAPI *UsbAsyncInterruptTransfer)(EFI_USB_IO_PROTOCOL *, UINT8, BOOLEAN, UINTN, UINTN, VOID *, VOID *);
    EFI_STATUS (EFIAPI *UsbSyncInterruptTransfer)(EFI_USB_IO_PROTOCOL *, UINT8, VOID *, UINTN *, UINTN, UINT32 *);
    EFI_STATUS (EFIAPI *UsbIsochronousTransfer)(EFI_USB_IO_PROTOCOL *, UINT8, VOID *, UINTN, UINTN, UINT32 *);
    EFI_STATUS (EFIAPI *UsbAsyncIsochronousTransfer)(EFI_USB_IO_PROTOCOL *, UINT8, BOOLEAN, UINTN, UINTN, VOID *, VOID *);
    EFI_STATUS (EFIAPI *UsbGetDeviceDescriptor)(EFI_USB_IO_PROTOCOL *, VOID *);
    EFI_STATUS (EFIAPI *UsbGetConfigDescriptor)(EFI_USB_IO_PROTOCOL *, VOID *);
    EFI_STATUS (EFIAPI *UsbGetInterfaceDescriptor)(EFI_USB_IO_PROTOCOL *, UsbInterfaceDescriptor *);
    EFI_STATUS (EFIAPI *UsbGetEndpointDescriptor)(EFI_USB_IO_PROTOCOL *, UINT8, UsbEndpointDescriptor *);
    EFI_STATUS (EFIAPI *UsbGetStringDescriptor)(EFI_USB_IO_PROTOCOL *, UINT16, UINT16, VOID **);
    EFI_STATUS (EFIAPI *UsbGetSupportedLanguages)(EFI_USB_IO_PROTOCOL *, UINT16 **, UINT16 *);
    EFI_STATUS (EFIAPI *UsbPortReset)(EFI_USB_IO_PROTOCOL *);
};

#define USB_ERR_STALL    0x02
#define USB_ERR_BUFFER   0x04
#define USB_ERR_BABBLE   0x08
#define USB_ERR_NAK      0x10
#define USB_ERR_CRC      0x20
#define USB_ERR_TIMEOUT  0x40
#define USB_ERR_BITSTUFF 0x80
#define USB_ERR_SYSTEM   0x100

#ifndef GUI_DEBUG
#define GUI_DEBUG 0
#endif

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gfx;
static EFI_SIMPLE_POINTER_PROTOCOL *mouse;
static EFI_ABSOLUTE_POINTER_PROTOCOL *abs_ptr;
static EFI_USB_IO_PROTOCOL *hid_io;
static UINT8 hid_endpoint;
static int hid_ready;
static UINT32 *gfx_fb, *gfx_hw, *gfx_back;
static UINTN gfx_w, gfx_h, gfx_stride;
static UINT32 gfx_bg, gfx_top, gfx_panel, gfx_text_c, gfx_dim, gfx_accent;
static UINTN gfx_original_mode;
static int mouse_x, mouse_y, mouse_down, mouse_was_down, mouse_click, mouse_moved, mouse_wheel;
static int gfx_done;

/* F11: mirror X/Y for firmware that reports them inverted. */
static int invert_mouse = 0;

/* Cursor incremental drawing. */
static UINT32 cursor_bg[16][10];
static int cursor_bg_x = -1, cursor_bg_y = -1;

/* Clip rect (used by draw_region). */
static int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;
static int clip_enabled = 0;

#define WIN_FILES 0
#define WIN_TERM 1
#define WIN_EDIT 2
#define WIN_MAX 5
#define WIN_MAX_LINES 200
#define WIN_VIEW_LINES 24
#define WIN_COLS 68
#define WIN_ENTRIES 32
typedef struct { CHAR16 name[64]; int dir; UINT64 size; } GuiEnt;

typedef struct {
    int type, x, y, w, h, z, alive;
    char input[96];
    int input_len;
    char text[WIN_MAX_LINES][WIN_COLS + 1];
    int trow, tcol, scroll;
    GuiEnt entries[WIN_ENTRIES];
    int entry_count, loaded;
    char edit[80][80];
    int edit_lines, edit_row, edit_col;
    CHAR16 edit_path[256];
    int edit_loaded;
} GuiWin;

static GuiWin wins[WIN_MAX];

/* wcteos 桌面外壳层（实现见 wcteos_ui.inc） */
static void wct_ui_draw(void);
static int  wct_ui_click(void);
static int  wct_ui_key(EFI_INPUT_KEY *key);
static void wct_ui_bg(void);
static void wct_ui_open_file(const CHAR16 *path);
static int  wct_ui_demo(void);
static void wct_ui_demo_start(void);
static GuiWin *gfx_term_target;
static int focus_win = -1, z_top = 10;
static int dragging = -1, drag_dx, drag_dy;

/* ---------- Win95 drag outline ---------- */
static int df_active = 0;
static int df_x, df_y, df_w, df_h;
static UINT32 *df_top_bg, *df_bottom_bg, *df_left_bg, *df_right_bg;

/* ---------- framebuffer ---------- */

static UINT32 gfx_rgb(UINT8 r, UINT8 g, UINT8 b)
{
    /* QEMU/OVMF 报告 PixelRedGreenBlue 时，实际扫描线字节序是 BGRX；
       按实测结果打包，否则蓝主题会显示成棕褐色（红蓝互换）。 */
    UINT32 fmt = gfx->Mode->Info->PixelFormat;
    if (fmt == 0) return ((UINT32)b << 16) | ((UINT32)g << 8) | r;
    return ((UINT32)r << 16) | ((UINT32)g << 8) | b;
}

static void gfx_pixel(int x, int y, UINT32 c)
{
    if (clip_enabled && (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1)) return;
    if (x < 0 || y < 0 || (UINTN)x >= gfx_w || (UINTN)y >= gfx_h) return;
    gfx_fb[(UINTN)y * gfx_stride + (UINTN)x] = c;
}

static void gfx_fill(int x, int y, int w, int h, UINT32 c)
{
    if (clip_enabled) {
        if (x < clip_x0) { w -= clip_x0 - x; x = clip_x0; }
        if (y < clip_y0) { h -= clip_y0 - y; y = clip_y0; }
        if (x + w > clip_x1) w = clip_x1 - x;
        if (y + h > clip_y1) h = clip_y1 - y;
        if (w <= 0 || h <= 0) return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)gfx_w) w = (int)gfx_w - x;
    if (y + h > (int)gfx_h) h = (int)gfx_h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; ++yy) {
        UINT32 *p = gfx_fb + (UINTN)(y + yy) * gfx_stride + x;
        for (int xx = 0; xx < w; ++xx) p[xx] = c;
    }
}

static void gfx_border(int x, int y, int w, int h, UINT32 c)
{
    gfx_fill(x, y, w, 2, c); gfx_fill(x, y + h - 2, w, 2, c);
    gfx_fill(x, y, 2, h, c); gfx_fill(x + w - 2, y, 2, h, c);
}

static void gfx_char(int x, int y, unsigned char ch, UINT32 c)
{
    if (ch < 32 || ch > 126) ch = '?';
    for (int r = 0; r < 16; ++r) {
        unsigned char bits = font8x16[ch][r];
        for (int col = 0; col < 8; ++col)
            if (bits & (0x80 >> col)) gfx_pixel(x + col, y + r, c);
    }
}
static void gfx_text(int x, int y, const char *s, UINT32 c) { while (*s) gfx_char(x, y, (unsigned char)*s++, c), x += 8; }
static void gfx_text_n(int x, int y, const char *s, int max, UINT32 c) { while (*s && max-- > 0) gfx_char(x, y, (unsigned char)*s++, c), x += 8; }
static void gfx_text_w(int x, int y, const CHAR16 *s, UINT32 c) { while (*s) gfx_char(x, y, (*s <= 126) ? (unsigned char)*s : '?', c), x += 8, ++s; }

static void gfx_cursor(void)
{
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 10; ++x) {
            int px = mouse_x + x, py = mouse_y + y;
            cursor_bg[y][x] = (px >= 0 && py >= 0 && px < (int)gfx_w && py < (int)gfx_h)
                ? gfx_fb[(UINTN)py * gfx_stride + px] : 0;
        }
    cursor_bg_x = mouse_x; cursor_bg_y = mouse_y;
    static const unsigned char arrow[16][2] = {
        {0x80,0},{0xC0,0},{0xE0,0},{0xF0,0},{0xF8,0},{0xFC,0},{0xFE,0},{0xFF,0},
        {0xFF,0x80},{0xF8,0},{0xD8,0},{0x8C,0},{0x0C,0},{0x06,0},{0x06,0},{0}
    };
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 10; ++x)
            if (arrow[y][x / 8] & (0x80 >> (x % 8)))
                gfx_pixel(mouse_x + x, mouse_y + y, gfx_rgb(255, 255, 255));
}

static void cursor_restore_bg(void)
{
    if (cursor_bg_x < 0) return;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 10; ++x) {
            int px = cursor_bg_x + x, py = cursor_bg_y + y;
            if (px >= 0 && py >= 0 && px < (int)gfx_w && py < (int)gfx_h)
                gfx_fb[(UINTN)py * gfx_stride + px] = cursor_bg[y][x];
        }
    cursor_bg_x = -1;
}

static void gfx_present_rect(int x, int y, int w, int h)
{
    if (!gfx_back || !gfx_hw) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)gfx_w) w = (int)gfx_w - x;
    if (y + h > (int)gfx_h) h = (int)gfx_h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; ++yy)
        memcpy(gfx_hw + (UINTN)(y + yy) * gfx_stride + x,
               gfx_back + (UINTN)(y + yy) * gfx_stride + x, (UINTN)w * 4);
}

static void gfx_present(void)
{
    if (!gfx_back || !gfx_hw) return;
    for (UINTN y = 0; y < gfx_h; ++y)
        memcpy(gfx_hw + y * gfx_stride, gfx_back + y * gfx_stride, gfx_w * 4);
}

/* ---------- HID fallback ---------- */

static void hid_init(void)
{
    EFI_GUID guid = { 0x2B2F68D6, 0x0CD2, 0x44CF, { 0x8E, 0x8B, 0xBB, 0xA2, 0x0B, 0x1B, 0x5B, 0x75 } };
    UINTN count = 0; EFI_HANDLE *handles = NULL;
    if (EFI_IS_ERROR(ST->BootServices->LocateHandleBuffer(2, &guid, NULL, &count, &handles))) return;
    for (UINTN i = 0; i < count && !hid_ready; ++i) {
        EFI_USB_IO_PROTOCOL *io = NULL;
        if (EFI_IS_ERROR(ST->BootServices->HandleProtocol(handles[i], &guid, (VOID **)&io)) || !io) continue;
        UsbInterfaceDescriptor d;
        if (EFI_IS_ERROR(io->UsbGetInterfaceDescriptor(io, &d))) continue;
        if (d.InterfaceClass != 3 || d.InterfaceSubClass != 1 || d.InterfaceProtocol != 2) continue;
        for (UINT8 e = 0; e < d.NumEndpoints; ++e) {
            UsbEndpointDescriptor ep;
            if (EFI_IS_ERROR(io->UsbGetEndpointDescriptor(io, e, &ep))) continue;
            if ((ep.EndpointAddress & 0x80) == 0 || (ep.Attributes & 3) != 3) continue;
            UsbDeviceRequest req = { 0x21, 0x0B, 0, d.InterfaceNumber, 0 };
            UINT32 st = 0;
            io->UsbControlTransfer(io, &req, 2, 500, NULL, 0, &st);
            hid_io = io; hid_endpoint = ep.EndpointAddress; hid_ready = 1;
            break;
        }
    }
    if (handles) ST->BootServices->FreePool(handles);
}

static void hid_clear_stall(void)
{
    if (!hid_io) return;
    UsbDeviceRequest req = { 0x02, 0x01, 0, hid_endpoint, 0 };
    UINT32 st = 0;
    hid_io->UsbControlTransfer(hid_io, &req, 2, 500, NULL, 0, &st);
}

static void hid_poll(void)
{
    UINT8 rep[8]; UINTN len = sizeof(rep); UINT32 st = 0;
    EFI_STATUS status = hid_io->UsbSyncInterruptTransfer(hid_io, hid_endpoint, rep, &len, 500, &st);
    if (st & USB_ERR_STALL) { hid_clear_stall(); return; }
    if (st & ~(USB_ERR_NAK | USB_ERR_TIMEOUT)) { hid_ready = 0; hid_io = NULL; return; }
    if (EFI_IS_ERROR(status) || (st & (USB_ERR_NAK | USB_ERR_TIMEOUT)) || len < 3) return;
    int dx = (signed char)rep[1], dy = (signed char)rep[2];
    if (invert_mouse) { dx = -dx; dy = -dy; }
    if (len >= 4) mouse_wheel = (signed char)rep[3];
    mouse_x += dx; mouse_y += dy;
    mouse_down = (rep[0] & 1) != 0;
    if (dx || dy) mouse_moved = 1;
}

/* ---------- pointer poll ---------- */

static void mouse_poll(void)
{
    mouse_click = 0; mouse_moved = 0; mouse_wheel = 0;

    if (mouse) {
        EFI_SIMPLE_POINTER_STATE st;
        if (!EFI_IS_ERROR(mouse->GetState(mouse, &st))) {
            int dx = st.RelativeMovementX, dy = st.RelativeMovementY;
            if (invert_mouse) { dx = -dx; dy = -dy; }
            if (dx || dy) { mouse_x += dx; mouse_y += dy; mouse_moved = 1; }
            mouse_down = st.LeftButton;
            mouse_wheel = (int)st.RelativeMovementZ;
        }
    } else if (abs_ptr && abs_ptr->Mode) {
        EFI_ABSOLUTE_POINTER_STATE st;
        if (!EFI_IS_ERROR(abs_ptr->GetState(abs_ptr, &st))) {
            UINT64 mx = abs_ptr->Mode->AbsoluteMaxX, my = abs_ptr->Mode->AbsoluteMaxY;
            if (mx && my) {
                UINT64 cx = st.CurrentX, cy = st.CurrentY;
                if (invert_mouse) {
                    cx = (mx > cx) ? mx - cx : 0;
                    cy = (my > cy) ? my - cy : 0;
                }
                int nx = (int)((cx * (gfx_w - 1)) / mx);
                int ny = (int)((cy * (gfx_h - 1)) / my);
                if (nx != mouse_x || ny != mouse_y) mouse_moved = 1;
                mouse_x = nx; mouse_y = ny;
            }
            mouse_down = (st.ActiveButtons & 1) != 0;
        }
    } else if (hid_ready) {
        hid_poll();
    }

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= (int)gfx_w) mouse_x = (int)gfx_w - 1;
    if (mouse_y >= (int)gfx_h) mouse_y = (int)gfx_h - 1;
    if (mouse_down && !mouse_was_down) mouse_click = 1;
    mouse_was_down = mouse_down;
}

/* ---------- window manager ---------- */

static GuiWin *win_from_index(int i) { return (i >= 0 && i < WIN_MAX && wins[i].alive) ? &wins[i] : NULL; }

static void win_focus(int i) { if (!win_from_index(i)) return; focus_win = i; wins[i].z = ++z_top; }

static int win_at(int x, int y)
{
    int best = -1, bz = -1;
    for (int i = 0; i < WIN_MAX; ++i) {
        if (!wins[i].alive) continue;
        if (x >= wins[i].x && x < wins[i].x + wins[i].w && y >= wins[i].y && y < wins[i].y + wins[i].h)
            if (wins[i].z > bz) { bz = wins[i].z; best = i; }
    }
    return best;
}

static void win_close(int i)
{
    if (!win_from_index(i)) return;
    wins[i].alive = 0;
    if (focus_win == i) {
        focus_win = -1;
        for (int k = 0; k < WIN_MAX; ++k) if (wins[k].alive) { focus_win = k; break; }
    }
}

static int win_open(int type)
{
    for (int i = 0; i < WIN_MAX; ++i)
        if (wins[i].alive && wins[i].type == type) { win_focus(i); return i; }
    for (int i = 0; i < WIN_MAX; ++i) {
        if (wins[i].alive) continue;
        for (UINTN k = 0; k < sizeof(wins[i]); ++k) ((UINT8 *)&wins[i])[k] = 0;
        wins[i].type = type; wins[i].alive = 1;
        int n = 0; for (int k = 0; k < WIN_MAX; ++k) if (wins[k].alive) ++n;
        wins[i].x = 70 + n * 45; wins[i].y = 90 + n * 35;
        wins[i].w = (int)gfx_w - 160; wins[i].h = (int)gfx_h - 220;
        if (wins[i].w > 760) wins[i].w = 760;
        if (wins[i].h > 460) wins[i].h = 460;
        if (wins[i].x + wins[i].w > (int)gfx_w) wins[i].x = (int)gfx_w - wins[i].w;
        if (wins[i].y + wins[i].h > (int)gfx_h - 34) wins[i].y = (int)gfx_h - 34 - wins[i].h;
        if (wins[i].x < 0) wins[i].x = 0;
        if (wins[i].y < 0) wins[i].y = 0;
        wins[i].z = ++z_top; focus_win = i;
        return i;
    }
    return -1;
}

static void term_putc(GuiWin *w, char c)
{
    if (c == '\r') { w->tcol = 0; return; }
    if (c == '\n') {
        w->text[w->trow][w->tcol] = 0; ++w->trow; w->tcol = 0;
        if (w->trow >= WIN_MAX_LINES) {
            for (int r = 1; r < WIN_MAX_LINES; ++r) memcpy(w->text[r - 1], w->text[r], WIN_COLS + 1);
            w->trow = WIN_MAX_LINES - 1; w->text[w->trow][0] = 0;
        }
        return;
    }
    if (c == 8) { if (w->tcol) w->text[w->trow][--w->tcol] = 0; return; }
    if (c < 32 || c > 126) c = '?';
    if (w->tcol >= WIN_COLS) {
        w->text[w->trow][WIN_COLS] = 0; ++w->trow; w->tcol = 0;
        if (w->trow >= WIN_MAX_LINES) { w->trow = WIN_MAX_LINES - 1; w->text[w->trow][0] = 0; }
    }
    w->text[w->trow][w->tcol++] = c;
    w->text[w->trow][w->tcol] = 0;
}

static char gui_cap[8192]; static int gui_cap_len;
static void gui_capture_put(const CHAR16 *s)
{
    while (*s && gui_cap_len + 2 < (int)sizeof(gui_cap)) {
        CHAR16 c = *s++;
        if (c == '\r') continue;
        if (c == '\n') { gui_cap[gui_cap_len++] = '\n'; continue; }
        if (c == '\t') { gui_cap[gui_cap_len++] = ' '; continue; }
        if (c >= 32 && c <= 126) gui_cap[gui_cap_len++] = (char)c;
        else gui_cap[gui_cap_len++] = '?';
    }
    gui_cap[gui_cap_len] = 0;
}
static void gui_capture_flush(GuiWin *w) { for (int i = 0; i < gui_cap_len; ++i) term_putc(w, gui_cap[i]); gui_cap_len = 0; }

static void gfx_out_wide(const CHAR16 *s)
{
    GuiWin *w = gfx_term_target ? gfx_term_target : win_from_index(focus_win);
    if (!w || w->type != WIN_TERM) return;
    while (*s) {
        CHAR16 c = *s++;
        if (c == '\r' || c == '\n' || c == 8 || (c >= 32 && c <= 126)) term_putc(w, (char)c);
        else term_putc(w, '?');
    }
}

static void term_execute(GuiWin *w)
{
    w->input[w->input_len] = 0;
    for (int i = 0; i < w->input_len; ++i) term_putc(w, w->input[i]);
    term_putc(w, '\n');
    if (!scmp(w->input, "exit")) { win_close(focus_win); return; }
    if (!scmp(w->input, "exitgui")) { gfx_done = 1; return; }
    if (w->input_len) {
        CHAR16 wide[96];
        for (int i = 0; i <= w->input_len; ++i) wide[i] = (CHAR16)(unsigned char)w->input[i];
        int duplicate = hist_count > 0 && weq(wide, hist[hist_count - 1]);
        if (!duplicate) {
            if (hist_count < HIST_MAX) wcopy(hist[hist_count++], 192, wide);
            else { for (int i = 1; i < HIST_MAX; ++i) wcopy(hist[i - 1], 192, hist[i]); wcopy(hist[HIST_MAX - 1], 192, wide); }
        }
        hist_pos = hist_count; active_tty = 2; gfx_active = 1;
        gfx_term_target = w; gui_cap_len = 0; gui_capture = 1;
        process_command(wide);
        gui_capture = 0; gui_capture_flush(w); w->scroll = 0;
        gfx_term_target = NULL; active_tty = 1;
        term_putc(w, '\n');
    }
    w->input_len = 0; w->input[0] = 0;
}

static void win_load_files(GuiWin *w)
{
    w->entry_count = 0;
    EFI_FILE_PROTOCOL *dir = NULL;
    if (!open_path(CWD, &dir, EFI_FILE_MODE_READ, 0)) { w->loaded = 1; return; }
    static UINT8 buf[2048];
    for (int guard = 0; guard < WIN_ENTRIES + 4; ++guard) {
        UINTN sz = sizeof(buf);
        if (EFI_IS_ERROR(dir->Read(dir, &sz, buf)) || sz == 0) break;
        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;
        if (!info->FileName[0]) break;
        if (weq(info->FileName, L".") || weq(info->FileName, L"..")) continue;
        GuiEnt *e = &w->entries[w->entry_count++];
        wcopy(e->name, 64, info->FileName);
        e->dir = (info->Attribute & EFI_FILE_DIRECTORY) ? 1 : 0;
        e->size = info->FileSize;
        if (w->entry_count >= WIN_ENTRIES) break;
    }
    dir->Close(dir); w->loaded = 1;
}

static void draw_files(GuiWin *w)
{
    if (!w->loaded) win_load_files(w);
    int x = w->x + 8, y = w->y + 36;
    gfx_text(x, y, "Path:", gfx_dim);
    gfx_text_w(x + 48, y, CWD, gfx_text_c);
    gfx_fill(x, y + 24, 64, 24, gfx_panel);
    gfx_border(x, y + 24, 64, 24, gfx_accent);
    gfx_text(x + 10, y + 30, "Back", gfx_text_c);
    for (int row = 0; row < w->entry_count; ++row) {
        int yy = y + 62 + row * 20;
        if (yy + 16 > w->y + w->h - 10) break;
        gfx_text_w(x + 18, yy, w->entries[row].name, w->entries[row].dir ? gfx_accent : gfx_text_c);
        if (w->entries[row].dir) gfx_text(x + 18 + (int)wlen(w->entries[row].name) * 8, yy, "/", gfx_accent);
    }
}

static int win_rows(GuiWin *w) { int r = (w->h - 80) / 16; if (r < 2) r = 2; if (r > WIN_VIEW_LINES) r = WIN_VIEW_LINES; return r; }

static void draw_term(GuiWin *w)
{
    int rows = win_rows(w);
    int start = w->trow - rows + 1 - w->scroll;
    if (start < 0) start = 0; if (start > w->trow) start = w->trow;
    for (int r = 0; r < rows; ++r) {
        int line = start + r;
        if (line >= 0 && line <= w->trow)
            gfx_text_n(w->x + 8, w->y + 38 + r * 16, w->text[line], WIN_COLS, gfx_text_c);
    }
    int yy = w->y + w->h - 24;
    gfx_text(w->x + 8, yy, "tty2>", gfx_accent);
    gfx_text(w->x + 80, yy, w->input, gfx_text_c);
    gfx_fill(w->x + 80 + w->input_len * 8, yy + 14, 7, 2, gfx_text_c);
}

static void gedit_load(GuiWin *w)
{
    w->edit_lines = 1; w->edit_row = w->edit_col = 0; w->edit[0][0] = 0;
    static UINT8 data[8192];
    UINTN n = read_whole_file(w->edit_path, data, sizeof(data) - 1);
    if (!n) { w->edit_loaded = 1; return; }
    int line = 0, col = 0;
    for (UINTN i = 0; i < n && line < 80; ++i) {
        char c = (char)data[i];
        if (c == '\r') continue;
        if (c == '\n') { w->edit[line][col] = 0; ++line; col = 0; if (line < 80) w->edit[line][0] = 0; continue; }
        if (col < 79) w->edit[line][col++] = c;
    }
    w->edit[line][col] = 0; w->edit_lines = line + 1; w->edit_loaded = 1;
}

static void gedit_save(GuiWin *w)
{
    static char out[8192]; UINTN n = 0;
    for (int r = 0; r < w->edit_lines && n + 82 < sizeof(out); ++r) {
        for (int c = 0; w->edit[r][c] && n + 1 < sizeof(out); ++c) out[n++] = w->edit[r][c];
        out[n++] = '\n';
    }
    if (write_bytes(w->edit_path, out, n)) w->edit_loaded = 2;
}

static void gedit_draw(GuiWin *w)
{
    gfx_fill(w->x + 4, w->y + 32, w->w - 8, 18, gfx_panel);
    gfx_text_w(w->x + 8, w->y + 36, w->edit_path, gfx_accent);
    for (int r = 0; r < w->edit_lines && r < 26; ++r) {
        int yy = w->y + 58 + r * 16;
        if (yy + 16 > w->y + w->h - 24) break;
        gfx_text(w->x + 8, yy, w->edit[r], gfx_text_c);
        if (r == w->edit_row) gfx_fill(w->x + 8 + w->edit_col * 8, yy + 14, 7, 2, gfx_accent);
    }
    gfx_text(w->x + 8, w->y + w->h - 20,
             w->edit_loaded == 2 ? "Saved. Ctrl+S save   Esc close" : "Ctrl+S save   Esc close", gfx_dim);
}

static int gedit_len(const char *s) { int n = 0; while (s[n]) ++n; return n; }

static void gedit_key(GuiWin *w, EFI_INPUT_KEY key)
{
    if (key.UnicodeChar == 19) { gedit_save(w); return; }
    if (key.ScanCode == 0x01) { if (w->edit_row > 0) --w->edit_row; w->edit_col = gedit_len(w->edit[w->edit_row]); return; }
    if (key.ScanCode == 0x02) { if (w->edit_row + 1 < w->edit_lines) ++w->edit_row; w->edit_col = gedit_len(w->edit[w->edit_row]); return; }
    if (key.ScanCode == 0x03) { if (w->edit_col > 0) --w->edit_col; return; }
    if (key.ScanCode == 0x04) { if (w->edit_col < gedit_len(w->edit[w->edit_row])) ++w->edit_col; return; }
    if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n' || key.ScanCode == 0x0D) {
        if (w->edit_lines < 80) {
            w->edit[w->edit_row][w->edit_col] = 0;
            ++w->edit_row; w->edit_col = 0; w->edit[w->edit_row][0] = 0;
            if (w->edit_row >= w->edit_lines) w->edit_lines = w->edit_row + 1;
        }
        return;
    }
    if (key.UnicodeChar == 8 && w->edit_col > 0) { w->edit[w->edit_row][--w->edit_col] = 0; return; }
    if (key.UnicodeChar >= 32 && key.UnicodeChar < 127 && w->edit_col < 79) {
        w->edit[w->edit_row][w->edit_col++] = (char)key.UnicodeChar;
        w->edit[w->edit_row][w->edit_col] = 0;
    }
}

static void draw_window(GuiWin *w)
{
    UINT32 border = (win_from_index(focus_win) == w) ? gfx_accent : gfx_dim;
    const char *title = w->type == WIN_FILES ? "Files" : (w->type == WIN_TERM ? "Terminal" : "Editor");
    gfx_fill(w->x, w->y, w->w, w->h, gfx_rgb(8, 8, 14));
    gfx_border(w->x, w->y, w->w, w->h, border);
    gfx_fill(w->x + 2, w->y + 2, w->w - 4, 28, gfx_top);
    gfx_text(w->x + 10, w->y + 8, title, border);
    gfx_fill(w->x + w->w - 34, w->y + 6, 24, 20, gfx_panel);
    gfx_border(w->x + w->w - 34, w->y + 6, 24, 20, border);
    gfx_text(w->x + w->w - 26, w->y + 9, "X", gfx_text_c);
    if (w->type == WIN_FILES) draw_files(w);
    else if (w->type == WIN_TERM) draw_term(w);
    else gedit_draw(w);
}

static void draw_desktop(void)
{
    wct_ui_bg();
    gfx_fill(0, 0, (int)gfx_w, 38, gfx_top);
    gfx_text(14, 11, "wcteos Desktop", gfx_text_c);
    gfx_text((int)gfx_w - 220, 11, "wcteos 0.9-test  |  WCT kernel", gfx_dim);
    if (invert_mouse) gfx_text((int)gfx_w - 60, 11, "INV", gfx_accent);
    const char *names[5] = { "Files", "Terminal", "Editor", "Theme", "Exit" };
    for (int i = 0; i < 5; ++i) {
        int x = 40 + i * 180, y = 75;
        gfx_fill(x, y, 160, 74, gfx_panel);
        gfx_border(x, y, 160, 74, gfx_accent);
        gfx_text(x + 14, y + 14, names[i], gfx_text_c);
        gfx_text(x + 14, y + 42, i == 2 ? "cycle" : (i == 3 ? "leave" : "open"), gfx_dim);
    }
    gfx_text(40, 180, "Drag window title bars. F11 toggles mouse X/Y.", gfx_dim);
    gfx_text(40, 202, "Drag shows an outline (Win95 style); release to drop.", gfx_dim);
    gfx_fill(0, (int)gfx_h - 34, (int)gfx_w, 34, gfx_top);
    /* 左下角开始按钮 */
    gfx_fill(6, (int)gfx_h - 30, 56, 24, gfx_panel);
    gfx_border(6, (int)gfx_h - 30, 56, 24, gfx_accent);
    gfx_text(20, (int)gfx_h - 26, "MENU", gfx_accent);
    /* 右侧托盘：网络状态 + 时钟 */
    gfx_fill((int)gfx_w - 168, (int)gfx_h - 30, 60, 24, gfx_panel);
    gfx_border((int)gfx_w - 168, (int)gfx_h - 30, 60, 24, gfx_dim);
    gfx_text((int)gfx_w - 162, (int)gfx_h - 26, "NET", gfx_dim);
    int bx = 72;
    for (int i = 0; i < WIN_MAX; ++i) {
        if (!wins[i].alive) continue;
        gfx_fill(bx, (int)gfx_h - 30, 150, 24, i == focus_win ? gfx_accent : gfx_panel);
        gfx_text(bx + 8, (int)gfx_h - 26, wins[i].type == WIN_FILES ? "Files" : (wins[i].type == WIN_TERM ? "Terminal" : "Editor"), gfx_text_c);
        bx += 160;
    }
}

static void draw_all(void)
{
    draw_desktop();
    for (int z = 1; z <= z_top; ++z)
        for (int i = 0; i < WIN_MAX; ++i)
            if (wins[i].alive && wins[i].z == z) draw_window(&wins[i]);
    wct_ui_draw();
    gfx_cursor();
}

static void apply_theme(void)
{
    switch (theme_index) {
    case 0: gfx_bg=gfx_rgb(15,10,25); gfx_top=gfx_rgb(60,20,80); gfx_panel=gfx_rgb(30,25,45); gfx_accent=gfx_rgb(205,125,255); break;
    case 1: gfx_bg=gfx_rgb(5,15,10); gfx_top=gfx_rgb(10,45,25); gfx_panel=gfx_rgb(15,32,22); gfx_accent=gfx_rgb(55,220,115); break;
    case 2: gfx_bg=gfx_rgb(0,10,0); gfx_top=gfx_rgb(0,40,0); gfx_panel=gfx_rgb(0,25,0); gfx_accent=gfx_rgb(0,255,0); break;
    case 3: gfx_bg=gfx_rgb(15,10,0); gfx_top=gfx_rgb(60,35,0); gfx_panel=gfx_rgb(35,25,5); gfx_accent=gfx_rgb(255,190,60); break;
    default: gfx_bg=gfx_rgb(5,10,20); gfx_top=gfx_rgb(20,45,70); gfx_panel=gfx_rgb(15,28,45); gfx_accent=gfx_rgb(100,210,255); break;
    }
    gfx_text_c = gfx_rgb(235,235,245);
    gfx_dim = gfx_rgb(145,145,165);
}

/* ---------- drag outline primitives ---------- */

static void dframe_pixel(int x, int y, UINT32 c)
{
    if (x < 0 || y < 0 || (UINTN)x >= gfx_w || (UINTN)y >= gfx_h) return;
    gfx_fb[(UINTN)y * gfx_stride + (UINTN)x] = c;
}

static void dframe_save_bg(void)
{
    int x = df_x, y = df_y, w = df_w, h = df_h;
    int x1 = x + w - 1, y1 = y + h - 1;
    for (int i = 0; i < w; ++i) {
        int px = x + i;
        df_top_bg[i]    = (px >= 0 && y  >= 0 && px < (int)gfx_w && y  < (int)gfx_h) ? gfx_fb[(UINTN)y  * gfx_stride + px] : 0;
        df_bottom_bg[i] = (px >= 0 && y1 >= 0 && px < (int)gfx_w && y1 < (int)gfx_h) ? gfx_fb[(UINTN)y1 * gfx_stride + px] : 0;
    }
    for (int i = 0; i < h; ++i) {
        int py = y + i;
        df_left_bg[i]  = (x  >= 0 && py >= 0 && x  < (int)gfx_w && py < (int)gfx_h) ? gfx_fb[(UINTN)py * gfx_stride + x ] : 0;
        df_right_bg[i] = (x1 >= 0 && py >= 0 && x1 < (int)gfx_w && py < (int)gfx_h) ? gfx_fb[(UINTN)py * gfx_stride + x1] : 0;
    }
}

static void dframe_restore_bg(void)
{
    int x = df_x, y = df_y, w = df_w, h = df_h;
    int x1 = x + w - 1, y1 = y + h - 1;
    for (int i = 0; i < w; ++i) {
        int px = x + i;
        if (px >= 0 && y  >= 0 && px < (int)gfx_w && y  < (int)gfx_h) gfx_fb[(UINTN)y  * gfx_stride + px] = df_top_bg[i];
        if (px >= 0 && y1 >= 0 && px < (int)gfx_w && y1 < (int)gfx_h) gfx_fb[(UINTN)y1 * gfx_stride + px] = df_bottom_bg[i];
    }
    for (int i = 0; i < h; ++i) {
        int py = y + i;
        if (x  >= 0 && py >= 0 && x  < (int)gfx_w && py < (int)gfx_h) gfx_fb[(UINTN)py * gfx_stride + x ] = df_left_bg[i];
        if (x1 >= 0 && py >= 0 && x1 < (int)gfx_w && py < (int)gfx_h) gfx_fb[(UINTN)py * gfx_stride + x1] = df_right_bg[i];
    }
}

static void dframe_draw_border(void)
{
    UINT32 c = gfx_rgb(255, 255, 255);
    int x = df_x, y = df_y, w = df_w, h = df_h;
    int x1 = x + w - 1, y1 = y + h - 1;
    for (int i = 0; i < w; ++i) { dframe_pixel(x + i, y, c); dframe_pixel(x + i, y1, c); }
    for (int i = 0; i < h; ++i) { dframe_pixel(x, y + i, c); dframe_pixel(x1, y + i, c); }
}

static void dframe_present(int x, int y)
{
    int w = df_w, h = df_h;
    gfx_present_rect(x, y, w, 1);
    gfx_present_rect(x, y + h - 1, w, 1);
    gfx_present_rect(x, y, 1, h);
    gfx_present_rect(x + w - 1, y, 1, h);
}

static void dframe_end(void)
{
    if (df_top_bg)    { ST->BootServices->FreePool(df_top_bg);    df_top_bg = NULL; }
    if (df_bottom_bg) { ST->BootServices->FreePool(df_bottom_bg); df_bottom_bg = NULL; }
    if (df_left_bg)   { ST->BootServices->FreePool(df_left_bg);   df_left_bg = NULL; }
    if (df_right_bg)  { ST->BootServices->FreePool(df_right_bg);  df_right_bg = NULL; }
    df_active = 0;
}

static int dframe_begin(int x, int y, int w, int h)
{
    dframe_end();
    df_w = w; df_h = h;
    if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, (UINTN)w * 4, (VOID **)&df_top_bg))    || !df_top_bg)    { dframe_end(); return 0; }
    if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, (UINTN)w * 4, (VOID **)&df_bottom_bg)) || !df_bottom_bg) { dframe_end(); return 0; }
    if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, (UINTN)h * 4, (VOID **)&df_left_bg))   || !df_left_bg)   { dframe_end(); return 0; }
    if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, (UINTN)h * 4, (VOID **)&df_right_bg))  || !df_right_bg)  { dframe_end(); return 0; }
    df_x = x; df_y = y;
    dframe_save_bg();
    dframe_draw_border();
    df_active = 1;
    dframe_present(df_x, df_y);
    return 1;
}

/* Move the outline to (nx, ny). Erases the old one, draws the new one, and
   presents only the 8 affected line segments plus the cursor. */
static void dframe_move(int nx, int ny, int old_cx, int old_cy)
{
    int ox = df_x, oy = df_y;
    cursor_restore_bg();
    dframe_restore_bg();
    df_x = nx; df_y = ny;
    dframe_save_bg();
    dframe_draw_border();
    gfx_cursor();
    dframe_present(ox, oy);
    if (ox != nx || oy != ny) dframe_present(nx, ny);
    gfx_present_rect(old_cx, old_cy, 10, 16);
    gfx_present_rect(mouse_x, mouse_y, 10, 16);
}

/* Drop: erase outline, apply final position, then caller does a full redraw. */
static int dframe_drop(void)
{
    int fx = df_x, fy = df_y;
    cursor_restore_bg();
    dframe_restore_bg();
    dframe_present(df_x, df_y);
    dframe_end();
    gfx_cursor();
    gfx_present_rect(mouse_x, mouse_y, 10, 16);
    return fx << 16 | (fy & 0xFFFF); /* unused, see caller */
}

static void handle_term_key(GuiWin *w, EFI_INPUT_KEY key)
{
    if (key.ScanCode == 0x0B || key.ScanCode == 0x01) {
        w->scroll += 4;
        int maxs = w->trow - win_rows(w) + 1; if (maxs < 0) maxs = 0;
        if (w->scroll > maxs) w->scroll = maxs;
        return;
    }
    if (key.ScanCode == 0x0C || key.ScanCode == 0x02) { w->scroll -= 4; if (w->scroll < 0) w->scroll = 0; return; }
    if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n' || key.ScanCode == 0x0D) { term_execute(w); return; }
    if (key.UnicodeChar == 8) { if (w->input_len) w->input[--w->input_len] = 0; return; }
    if (key.UnicodeChar >= 32 && key.UnicodeChar < 127 && w->input_len < 95) {
        w->input[w->input_len++] = (char)key.UnicodeChar;
        w->input[w->input_len] = 0;
    }
}

static void handle_click(void)
{
    if (wct_ui_click()) return;
    int wi = win_at(mouse_x, mouse_y);
    if (wi >= 0) {
        GuiWin *w = &wins[wi];
        win_focus(wi);
        int lx = mouse_x - w->x, ly = mouse_y - w->y;
        if (lx >= w->w - 36 && ly >= 4 && ly <= 28) { win_close(wi); return; }
        if (ly < 30) { dragging = wi; drag_dx = mouse_x - w->x; drag_dy = mouse_y - w->y; return; }
        if (w->type == WIN_FILES) {
            if (lx >= 8 && lx < 72 && ly >= 60 && ly < 84) {
                CHAR16 parent[256];
                normalize_path(L"..", parent, sizeof(parent) / sizeof(parent[0]));
                wcopy(CWD, sizeof(CWD) / sizeof(CWD[0]), parent);
                w->loaded = 0; return;
            }
            if (ly >= 98) {
                int row = (ly - 98) / 20;
                if (row < w->entry_count) {
                    CHAR16 next[256];
                    wcopy(next, sizeof(next) / sizeof(next[0]), CWD);
                    if (wlen(next) && next[wlen(next) - 1] != L'\\')
                        wcat(next, sizeof(next) / sizeof(next[0]), L"\\");
                    wcat(next, sizeof(next) / sizeof(next[0]), w->entries[row].name);
                    if (w->entries[row].dir) {
                        EFI_FILE_PROTOCOL *chk = NULL;
                        if (open_path(next, &chk, EFI_FILE_MODE_READ, 0)) {
                            chk->Close(chk);
                            if (wlen(next) < 255) { wcopy(CWD, sizeof(CWD) / sizeof(CWD[0]), next); w->loaded = 0; }
                        }
                    } else {
                        int elen = (int)wlen(next);
                        if ((elen >= 4 && weq(next + elen - 4, L".wct")) ||
                            (elen >= 4 && weq(next + elen - 4, L".wet")) ||
                            (elen >= 4 && weq(next + elen - 4, L".dwc"))) {
                            wct_ui_open_file(next);
                        } else if ((elen >= 4 && weq(next + elen - 4, L".txt")) ||
                                   (elen >= 7 && weq(next + elen - 7, L".wctpkg"))) {
                            int e = win_open(WIN_EDIT);
                            if (e >= 0) { wcopy(wins[e].edit_path, 256, next); gedit_load(&wins[e]); }
                        } else {
                            int t = win_open(WIN_TERM);
                            if (t >= 0) {
                                term_putc(&wins[t], 'F'); term_putc(&wins[t], ':'); term_putc(&wins[t], ' ');
                                for (int k = 0; next[k] && k < 60; ++k) term_putc(&wins[t], (char)next[k]);
                                term_putc(&wins[t], '\n');
                            }
                        }
                    }
                }
            }
        }
        return;
    }
    if (mouse_y >= (int)gfx_h - 34) {
        int bx = 8;
        for (int i = 0; i < WIN_MAX; ++i) {
            if (!wins[i].alive) continue;
            if (mouse_x >= bx && mouse_x < bx + 150) { win_focus(i); return; }
            bx += 160;
        }
    }
    for (int i = 0; i < 5; ++i) {
        int x = 40 + i * 180, y = 75;
        if (mouse_x >= x && mouse_x < x + 160 && mouse_y >= y && mouse_y < y + 74) {
            if (i == 0) win_open(WIN_FILES);
            else if (i == 1) win_open(WIN_TERM);
            else if (i == 2) {
                int e = win_open(WIN_EDIT);
                if (e >= 0) { wcopy(wins[e].edit_path, 256, L"\\home\\wct\\untitled.txt"); gedit_load(&wins[e]); }
            }
            else if (i == 3) theme_index = (theme_index + 1) % 5;
            else gfx_done = 1;
            return;
        }
    }
}

/* ---------- main loop ---------- */

static void gui_loop(void)
{
    gfx_done = 0; focus_win = -1; z_top = 10;
    theme_index = 4;      /* wcteos 默认蓝色主题 */
    apply_theme();
    mouse_x = (int)gfx_w / 2; mouse_y = (int)gfx_h / 2;
    mouse_down = mouse_was_down = 0;
    cursor_bg_x = cursor_bg_y = -1;
    for (int i = 0; i < WIN_MAX; ++i) wins[i].alive = 0;
    dframe_end();
    int dirty = 1, force_full = 1;

    while (!gfx_done) {
        int prev_mx = mouse_x, prev_my = mouse_y;
        int prev_cx = cursor_bg_x, prev_cy = cursor_bg_y;

        mouse_poll();
        if (mouse_moved || mouse_click || mouse_wheel) dirty = 1;

        int full = 0;
        int drag_handled = 0;

        /* 演示模式：\wct\uidemo 存在时按节奏自动切换面板（截图回归用） */
        wct_ui_demo_start();
        if (wct_ui_demo()) { dirty = 1; full = 1; }

        if (mouse_wheel && focus_win >= 0 && wins[focus_win].alive && wins[focus_win].type == WIN_TERM) {
            wins[focus_win].scroll += mouse_wheel * 3;
            int maxs = wins[focus_win].trow - win_rows(&wins[focus_win]) + 1;
            if (maxs < 0) maxs = 0;
            if (wins[focus_win].scroll > maxs) wins[focus_win].scroll = maxs;
            if (wins[focus_win].scroll < 0) wins[focus_win].scroll = 0;
            full = 1;
        }

        if (mouse_click) { handle_click(); full = 1; }

        if (dragging >= 0 && wins[dragging].alive) {
            if (mouse_down) {
                GuiWin *dw = &wins[dragging];
                int nx = mouse_x - drag_dx, ny = mouse_y - drag_dy;
                if (nx < 0) nx = 0; if (ny < 0) ny = 0;
                if (nx + dw->w > (int)gfx_w) nx = (int)gfx_w - dw->w;
                if (ny + dw->h > (int)gfx_h - 34) ny = (int)gfx_h - 34 - dw->h;
                if (nx < 0) nx = 0; if (ny < 0) ny = 0;

                if (!df_active) dframe_begin(dw->x, dw->y, dw->w, dw->h);

                if (df_active) {
                    if (nx != df_x || ny != df_y) {
                        dframe_move(nx, ny, prev_cx, prev_cy);
                        drag_handled = 1; dirty = 0; force_full = 0;
                    } else if (mouse_x != prev_mx || mouse_y != prev_my) {
                        cursor_restore_bg();
                        gfx_cursor();
                        gfx_present_rect(prev_cx, prev_cy, 10, 16);
                        gfx_present_rect(mouse_x, mouse_y, 10, 16);
                        drag_handled = 1; dirty = 0;
                    }
                } else {
                    /* dframe_begin failed: fall back to live move */
                    dw->x = nx; dw->y = ny; full = 1;
                }
            } else {
                /* Mouse released: drop */
                if (df_active) {
                    int fx = df_x, fy = df_y;
                    cursor_restore_bg();
                    dframe_restore_bg();
                    dframe_present(df_x, df_y);
                    gfx_cursor();
                    gfx_present_rect(mouse_x, mouse_y, 10, 16);
                    dframe_end();
                    wins[dragging].x = fx;
                    wins[dragging].y = fy;
                    full = 1; dirty = 1;
                }
                dragging = -1;
            }
        }

        EFI_INPUT_KEY key;
        if (!EFI_IS_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &key))) {
            if (wct_ui_key(&key)) { dirty = 1; full = 1; continue; }
            if (key.ScanCode == 0x15) {
                invert_mouse = !invert_mouse;
                dirty = 1; full = 1;
            } else {
                if (dragging >= 0) {
                    if (df_active) {
                        int fx = df_x, fy = df_y;
                        cursor_restore_bg();
                        dframe_restore_bg();
                        dframe_present(df_x, df_y);
                        dframe_end();
                        wins[dragging].x = fx;
                        wins[dragging].y = fy;
                    }
                    dragging = -1;
                }
                dirty = 1; full = 1;
                if (key.ScanCode == 0x14) gfx_done = 1;
                else if (key.ScanCode == 0x17) {
                    if (focus_win >= 0) win_close(focus_win);
                    else gfx_done = 1;
                } else if (focus_win >= 0 && wins[focus_win].alive && wins[focus_win].type == WIN_TERM) {
                    handle_term_key(&wins[focus_win], key);
                } else if (focus_win >= 0 && wins[focus_win].alive && wins[focus_win].type == WIN_EDIT) {
                    gedit_key(&wins[focus_win], key);
                }
            }
        }

        if (!drag_handled && dirty) {
            if (full || force_full) {
                apply_theme();
                draw_all();
                gfx_present();
            } else if (mouse_x != prev_mx || mouse_y != prev_my) {
                cursor_restore_bg();
                gfx_cursor();
                gfx_present_rect(prev_cx, prev_cy, 10, 16);
                gfx_present_rect(mouse_x, mouse_y, 10, 16);
            }
            dirty = 0; force_full = 0;
        }

        ST->BootServices->Stall(1000);
    }
    dframe_end();
}

static void cmd_gui(void)
{
    if (EFI_IS_ERROR(ST->BootServices->LocateProtocol(&gop_guid, NULL, (VOID **)&gfx)) ||
        !gfx || !gfx->Mode || !gfx->Mode->FrameBufferBase) {
        outln(L"gui: GOP framebuffer not available"); return;
    }
    gfx_w = gfx->Mode->Info->HorizontalResolution;
    gfx_h = gfx->Mode->Info->VerticalResolution;
    gfx_stride = gfx->Mode->Info->PixelsPerScanLine;
    gfx_hw = (UINT32 *)(UINTN)gfx->Mode->FrameBufferBase;
    gfx_fb = gfx_hw; gfx_back = NULL;
    /* 直写帧缓冲模式：省掉每帧 3MB 的软件拷贝，低配/软件渲染下更跟手。
       若需要双缓冲，把下面一行改为分配 gfx_back 即可。 */
    gfx_back = NULL;
    gfx_fb = gfx_hw;

    EFI_SIMPLE_POINTER_PROTOCOL *sp = NULL;
    EFI_GUID spg = { 0x31878C87, 0x0B75, 0x11D5, { 0x9A, 0x4F, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } };
    if (!EFI_IS_ERROR(ST->BootServices->LocateProtocol(&spg, NULL, (VOID **)&sp))) mouse = sp; else mouse = NULL;
    EFI_ABSOLUTE_POINTER_PROTOCOL *ap = NULL;
    EFI_GUID apg = { 0x8D59D32B, 0xC655, 0x4AE9, { 0x9B, 0x15, 0xF2, 0x59, 0x04, 0x99, 0x2A, 0x43 } };
    if (!EFI_IS_ERROR(ST->BootServices->LocateProtocol(&apg, NULL, (VOID **)&ap))) abs_ptr = ap; else abs_ptr = NULL;

    hid_ready = 0; hid_io = NULL;
    if (!mouse && !abs_ptr) hid_init();
    invert_mouse = 0;

    gfx_original_mode = ST->ConOut->Mode->Mode;
    gfx_active = 1;
    ST->ConOut->EnableCursor(ST->ConOut, FALSE);
    gui_loop();
    gfx_active = 0;
    dframe_end();
    if (gfx_back) { ST->BootServices->FreePool(gfx_back); gfx_back = NULL; }
    ST->ConOut->SetMode(ST->ConOut, gfx_original_mode);
    ST->ConOut->EnableCursor(ST->ConOut, TRUE);
    ST->ConOut->ClearScreen(ST->ConOut);
    reset_color();
    print_banner();
}

static void cmd_exitgui(void) { outln(L"exitgui: not in GUI (run gui first)"); }
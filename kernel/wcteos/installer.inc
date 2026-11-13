/* installer.inc - wcteos Pro 安装程序 v3
 * 流程：扫描磁盘 -> 选目标 -> 建 MBR/FAT32 -> 重新挂载 -> 复制系统 -> 写引导项+BootOrder -> 回读校验
 * 依赖 kernel.c 的 out/outln/read_line/ROOT/g_loaded/gfx_active，在 oobe.inc 之后 include。
 *
 * v3 修掉的实测缺陷（v2 在 QEMU 上真装过一遍后定位）：
 *  1) 旧版把安装介质的 \wct\au_early（无人值守脚本）也复制进目标盘，装好的系统每次开机都会
 *     再跑一次 install，把自己的系统盘重新格式化。现在安装介质专属文件全部排除。
 *  2) 引导项固定写 Boot0001 且不更新 BootOrder：会覆盖别人的启动项，新装盘也可能不被固件选中。
 *     现在查空槽位写 BootXXXX，并把该槽位插到 BootOrder 首位。
 *  3) 挂载目标卷时按“最大卷”挑，源盘 ≥400MB 时可能挑中安装介质本身，同盘复制会毁掉安装器。
 *     现在按设备路径前缀把卷绑到刚格式化的那块盘。
 *  4) FAT32 只写了 BPB 卷标、没写根目录卷标项，fsck.fat 报 label 不一致。现在补根目录卷标项。
 *  5) 复制完没有校验；现在回读目标盘 BOOTX64.EFI 与源文件比大小。
 *  6) 中文在 80x25 文本控制台会变成问号；图形终端给中文，文本控制台给 ASCII。
 */

typedef struct {
    UINT32 MediaId;
    UINT8  RemovableMedia;
    UINT8  MediaPresent;
    UINT8  LogicalPartition;
    UINT8  ReadOnly;
    UINT8  WriteCaching;
    UINT32 BlockSize;
    UINT32 IoAlign;
    UINT64 LastBlock;
    UINT32 LowestAlignedLba;
    UINT32 LogicalBlocksPerPhysicalBlock;
    UINT32 OptimalTransferLengthGranularity;
} INS_MEDIA;

typedef struct {
    UINT64 Revision;
    INS_MEDIA *Media;
    EFI_STATUS (EFIAPI *Reset)(void *);
    EFI_STATUS (EFIAPI *ReadBlocks)(void *, UINT32, UINT64, UINTN, void *);
    EFI_STATUS (EFIAPI *WriteBlocks)(void *, UINT32, UINT64, UINTN, void *);
    EFI_STATUS (EFIAPI *FlushBlocks)(void *);
} INS_BIO;

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[1];
} INS_FINFO;

typedef EFI_STATUS (EFIAPI *INS_LOCATE_DP)(EFI_GUID *, void **, EFI_HANDLE *);
typedef EFI_STATUS (EFIAPI *INS_CONNECT)(EFI_HANDLE, EFI_HANDLE *, void *, void *, int);
typedef EFI_STATUS (EFIAPI *INS_DISCONNECT)(EFI_HANDLE, EFI_HANDLE, EFI_HANDLE);
typedef EFI_STATUS (EFIAPI *INS_SETVAR)(CHAR16 *, EFI_GUID *, UINT32, UINTN, void *);
typedef EFI_STATUS (EFIAPI *INS_GETVAR)(CHAR16 *, EFI_GUID *, UINT32 *, UINTN *, void *);

static EFI_GUID ins_bio_guid = { 0x964e5b21, 0x6459, 0x11d2,
    { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID ins_global_guid = { 0x8be4df61, 0x93ca, 0x11d2,
    { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } };

#define INS_MAX_DISKS 8
#define INS_PART_LBA 2048ULL
#define INS_MIN_BYTES (400ULL * 1024 * 1024)
#define INS_VAR_ATTR 7ULL
#define INS_NOT_FOUND 0x800000000000000EULL
#define INS_BUFFER_SMALL 0x8000000000000005ULL
#define INS_BOOT_FILE L"\\EFI\\BOOT\\BOOTX64.EFI"
#define INS_LABEL "WCTEOS"

typedef struct {
    EFI_HANDLE h;
    INS_BIO *bio;
    UINT64 bytes;
    UINT64 sectors;
    int removable;
    int boot;
} INS_DISK;

static INS_DISK ins_disks[INS_MAX_DISKS];
static int ins_disk_count;
static INS_DISK *ins_target_disk;
static UINT8 *ins_sec;
static UINT8 *ins_zero;
static int ins_zero_blocks;
static int ins_bs;
static char ins_last_error[24];
static UINT64 ins_files;
static UINT64 ins_bytes;

static int fat_ready(void);
static int fat_make_dir(UINT32 parent, const char *name, UINT32 *out);
static int fat_write_file(UINT32 dir, const char *name, const UINT8 *data, UINT32 size);
static int fat_lookup(UINT32 dir, const char *name, UINT32 *first, UINT32 *size);
static EFI_FILE_PROTOCOL *ins_step_open(EFI_FILE_PROTOCOL *from, const CHAR16 *name, int is_dir);
static UINT8 *ins_load_boot(UINT32 *size_out);
static int ins_native_install(UINT32 *size_out);

static void ins_num(UINT64 v);
static int ins_ui_confirm(const CHAR16 *prompt);
static int ins_pick_disk(void);

/* 图形终端（中文可渲染）给中文，裸文本控制台/串口给 ASCII，两条路都看得懂 */
static void ins_msg(const char *ascii, const CHAR16 *zh)
{
    if (gfx_active) outln((CHAR16 *)zh);
    else outln_cstr(ascii);
}

static void ins_name_out(const CHAR16 *name)
{
    char b[260];
    int n = 0;
    for (int i = 0; name[i] && n < 255; ++i) {
        CHAR16 c = name[i];
        b[n++] = (char)((c >= 32 && c < 127) ? c : '?');
    }
    b[n] = 0;
    out_ascii(b);
}

static UINT16 ins_rd16(const UINT8 *p) { return (UINT16)(p[0] | (p[1] << 8)); }
static UINT32 ins_rd32(const UINT8 *p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}
static void ins_wr16(UINT8 *p, UINT16 v) { p[0] = (UINT8)v; p[1] = (UINT8)(v >> 8); }
static void ins_wr32(UINT8 *p, UINT32 v)
{
    p[0] = (UINT8)v; p[1] = (UINT8)(v >> 8); p[2] = (UINT8)(v >> 16); p[3] = (UINT8)(v >> 24);
}
static void ins_zeromem(UINT8 *p, UINTN n) { for (UINTN i = 0; i < n; ++i) p[i] = 0; }
static void ins_memcopy(UINT8 *d, const UINT8 *s, UINTN n) { for (UINTN i = 0; i < n; ++i) d[i] = s[i]; }

static void ins_num(UINT64 v)
{
    CHAR16 b[24];
    int n = 0;
    if (!v) { out(L"0"); return; }
    while (v && n < 23) { b[n++] = (CHAR16)('0' + (int)(v % 10)); v /= 10; }
    CHAR16 t[24];
    for (int i = 0; i < n; ++i) t[i] = b[n - 1 - i];
    t[n] = 0;
    out(t);
}

static void ins_fail(const char *tag)
{
    int i = 0;
    while (tag[i] && i < 23) { ins_last_error[i] = tag[i]; ++i; }
    ins_last_error[i] = 0;
    out_ascii("[install] FAIL ");
    outln_cstr(ins_last_error);
}

/* 页对齐缓冲区：满足 UEFI Block I/O 的 IoAlign 要求 */
static UINT8 *ins_align(UINT8 *p, UINTN align)
{
    UINTN a = (UINTN)p;
    return (UINT8 *)((a + align - 1) & ~(align - 1));
}

static int ins_buffers_alloc(INS_BIO *bio)
{
    if (bio->Media->BlockSize < 512 || bio->Media->BlockSize > 4096) {
        ins_fail("blocksize");
        return 0;
    }
    ins_bs = (int)bio->Media->BlockSize;
    if (!ins_sec) {
        void *raw = NULL;
        EFI_STATUS ast = ST->BootServices->AllocatePool(EFI_LOADER_DATA, 8192, &raw);
        if (EFI_IS_ERROR(ast) || !raw) {
            out_ascii("[install] pool st="); ins_num((UINT64)(unsigned)ast); outln(L"");
            ins_fail("nopool");
            return 0;
        }
        ins_sec = ins_align((UINT8 *)raw, 4096);
    }
    if (!ins_zero) {
        void *raw = NULL;
        if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, 8192, &raw)) || !raw) {
            ins_fail("nopool");
            return 0;
        }
        ins_zero = ins_align((UINT8 *)raw, 4096);
        ins_zeromem(ins_zero, 4096);
        ins_zero_blocks = 4096 / ins_bs;
    }
    else if (!ins_zero_blocks)
        ins_zero_blocks = 4096 / ins_bs;
    return 1;
}

static int ins_write(INS_BIO *bio, UINT64 lba, UINTN blocks, void *buf)
{
    EFI_STATUS st = bio->WriteBlocks(bio, bio->Media->MediaId, lba, (UINTN)ins_bs * blocks, buf);
    if (EFI_IS_ERROR(st)) {
        out_ascii("[install] write lba="); ins_num(lba);
        out_ascii(" st="); ins_num((UINT64)(unsigned)st); outln(L"");
        return 0;
    }
    return 1;
}

static int ins_read(INS_BIO *bio, UINT64 lba, UINTN blocks, void *buf)
{
    EFI_STATUS st = bio->ReadBlocks(bio, bio->Media->MediaId, lba, (UINTN)ins_bs * blocks, buf);
    if (EFI_IS_ERROR(st)) {
        out_ascii("[install] read lba="); ins_num(lba);
        out_ascii(" st="); ins_num((UINT64)(unsigned)st); outln(L"");
        return 0;
    }
    return 1;
}

static int ins_zero_range(INS_BIO *bio, UINT64 lba, UINT64 blocks)
{
    if (ins_zero_blocks < 1) return 0;
    while (blocks) {
        UINTN n = (blocks > (UINT64)ins_zero_blocks) ? (UINTN)ins_zero_blocks : (UINTN)blocks;
        if (!ins_write(bio, lba, n, ins_zero)) return 0;
        lba += n;
        blocks -= n;
    }
    return 1;
}

static int ins_is_boot_disk(EFI_HANDLE h)
{
    if (!g_loaded || !g_loaded->DeviceHandle) return 0;
    void *dp = NULL;
    if (EFI_IS_ERROR(ST->BootServices->HandleProtocol(g_loaded->DeviceHandle, &device_path_guid, &dp)) || !dp)
        return 0;
    INS_LOCATE_DP locate = (INS_LOCATE_DP)ST->BootServices->LocateDevicePath;
    EFI_HANDLE diskh = NULL;
    void *rem = dp;
    if (EFI_IS_ERROR(locate(&device_path_guid, &rem, &diskh)) || !diskh) return 0;
    return diskh == h;
}

static int ins_scan(void)
{
    EFI_HANDLE *buf = NULL;
    UINTN n = 0;
    ins_disk_count = 0;
    ins_target_disk = NULL;
    if (EFI_IS_ERROR(ST->BootServices->LocateHandleBuffer(2, &ins_bio_guid, NULL, &n, &buf)) || !buf)
        return 0;
    for (UINTN i = 0; i < n && ins_disk_count < INS_MAX_DISKS; ++i) {
        INS_BIO *bio = NULL;
        if (EFI_IS_ERROR(ST->BootServices->HandleProtocol(buf[i], &ins_bio_guid, (void **)&bio)) || !bio)
            continue;
        if (!bio->Media || !bio->Media->MediaPresent) continue;
        if (bio->Media->LogicalPartition) continue;
        if (bio->Media->BlockSize == 0) continue;
        if (bio->Media->LastBlock == 0) continue;
        if (bio->Revision < 0x00010000ULL) continue;
        INS_DISK *d = &ins_disks[ins_disk_count];
        d->h = buf[i];
        d->bio = bio;
        d->removable = bio->Media->RemovableMedia ? 1 : 0;
        d->bytes = (bio->Media->LastBlock + 1) * (UINT64)bio->Media->BlockSize;
        d->sectors = bio->Media->LastBlock + 1;
        d->boot = ins_is_boot_disk(buf[i]);
        ++ins_disk_count;
    }
    ST->BootServices->FreePool(buf);
    return ins_disk_count;
}

static void ins_print_disks(void)
{
    for (int i = 0; i < ins_disk_count; ++i) {
        INS_DISK *d = &ins_disks[i];
        out_ascii("  ["); ins_num((UINT64)i); out_ascii("] ");
        ins_num(d->bytes / (1024ULL * 1024)); out_ascii(" MB");
        if (d->removable) out_ascii(" [usb]");
        if (d->boot) out_ascii(" [boot-medium]");
        if (!d->removable && !d->boot && d->bytes >= INS_MIN_BYTES) out_ascii(" [installable]");
        outln(L"");
    }
}

/* 自动选盘的条件：内置盘、不是当前安装介质、容量够 */
static int ins_selectable(INS_DISK *d)
{
    return !d->removable && !d->boot && d->bytes >= INS_MIN_BYTES;
}

static int ins_pick_best(void)
{
    int best = -1;
    for (int i = 0; i < ins_disk_count; ++i) {
        if (!ins_selectable(&ins_disks[i])) continue;
        if (best < 0 || ins_disks[i].bytes > ins_disks[best].bytes) best = i;
    }
    if (best < 0) return 0;
    ins_target_disk = &ins_disks[best];
    return 1;
}

static int ins_ui_confirm(const CHAR16 *prompt)
{
    CHAR16 line[32];
    outln(prompt);
    out(L"> ");
    read_line(line, sizeof(line) / sizeof(line[0]));
    return (line[0] == 'y' || line[0] == 'Y');
}

static int ins_pick_disk(void)
{
    ins_msg("Disk list:", L"磁盘列表：");
    ins_print_disks();
    if (!ins_disk_count) { ins_msg("no disk found", L"没有可用磁盘"); return 0; }
    ins_msg("Enter disk number, blank line = biggest installable disk",
            L"输入可安装磁盘编号，直接回车自动选最大的可安装磁盘");
    out(L"> ");
    CHAR16 line[16];
    read_line(line, sizeof(line) / sizeof(line[0]));
    if (!line[0]) return ins_pick_best();
    int idx = -1;
    for (int i = 0; line[i]; ++i) {
        if (line[i] < '0' || line[i] > '9') { idx = -1; break; }
        if (idx < 0) idx = 0;
        idx = idx * 10 + (int)(line[i] - '0');
    }
    if (idx < 0 || idx >= ins_disk_count) { ins_msg("invalid number", L"编号无效"); return 0; }
    INS_DISK *d = &ins_disks[idx];
    if (d->boot) {
        ins_msg("that is the running install medium, pick another disk",
                L"这块是当前安装介质，请另选一块磁盘");
        return 0;
    }
    if (d->bytes < INS_MIN_BYTES) { ins_msg("disk too small", L"磁盘容量不足"); return 0; }
    if (d->removable) {
        ins_msg("removable target selected", L"已选择可移动介质作为目标");
    }
    ins_target_disk = d;
    return 1;
}

/* ---------- FAT32 ---------- */

typedef struct {
    UINT64 part_lba;
    UINT64 part_sectors;
    UINT32 spc;
    UINT32 fatsz;
    UINT32 clusters;
    UINT32 reserved;
} INS_FAT;

static INS_FAT g_fat;
static INS_BIO *g_fbio;
static UINT32 g_fat_used;

static int ins_fat_plan(INS_FAT *f, UINT64 sectors)
{
    f->reserved = 32;
    f->part_lba = INS_PART_LBA;
    if (sectors <= INS_PART_LBA + 4096ULL) { ins_fail("small"); return 0; }
    f->part_sectors = sectors - INS_PART_LBA - 2048;
    /* MBR 的起始/长度字段都是 32 位；超过 2TiB 就截断到 2TiB，剩余空间留空 */
    if (f->part_sectors > 0xFFFFFFFFULL) f->part_sectors = 0xFFFFFFFFULL;
    if (f->part_sectors < 70000ULL) { ins_fail("small"); return 0; }
    f->spc = 8;
    while (f->spc > 1 && f->part_sectors / f->spc < 70000ULL) f->spc >>= 1;
    f->fatsz = (UINT32)(((f->part_sectors / f->spc + 2) * 4 + 511) / 512 + 2);
    for (int it = 0; it < 12; ++it) {
        f->clusters = (UINT32)((f->part_sectors - f->reserved - (UINT64)2 * f->fatsz) / f->spc);
        UINT32 need = (UINT32)(((UINT64)(f->clusters + 2) * 4 + 511) / 512 + 2);
        if (need <= f->fatsz) break;
        f->fatsz = need;
    }
    if (f->clusters < 65525U) { ins_fail("clusters"); return 0; }
    if (f->fatsz > (UINT32)(f->part_sectors / 4)) { ins_fail("fatbig"); return 0; }
    return 1;
}

static void ins_build_bpb(UINT8 *sec, INS_FAT *f)
{
    ins_zeromem(sec, 512);
    sec[0] = 0xEB; sec[1] = 0x58; sec[2] = 0x90;
    const char *oem = INS_LABEL "  ";
    for (int i = 0; i < 8; ++i) sec[3 + i] = (UINT8)oem[i];
    ins_wr16(sec + 11, 512);
    sec[13] = (UINT8)f->spc;
    ins_wr16(sec + 14, f->reserved);
    sec[16] = 2;
    ins_wr16(sec + 17, 0);
    ins_wr16(sec + 19, 0);
    sec[21] = 0xF8;
    ins_wr16(sec + 22, 0);
    ins_wr16(sec + 24, 63);
    ins_wr16(sec + 26, 255);
    ins_wr32(sec + 28, (UINT32)f->part_lba);
    ins_wr32(sec + 32, (UINT32)f->part_sectors);
    ins_wr32(sec + 36, f->fatsz);
    ins_wr32(sec + 44, 2);
    ins_wr16(sec + 48, 1);
    ins_wr16(sec + 50, 6);
    sec[64] = 0x80;
    sec[66] = 0x29;
    ins_wr32(sec + 67, 0x57435450);
    const char *label = INS_LABEL "     ";
    for (int i = 0; i < 11; ++i) sec[71 + i] = (UINT8)label[i];
    const char *fs = "FAT32   ";
    for (int i = 0; i < 8; ++i) sec[82 + i] = (UINT8)fs[i];
    sec[510] = 0x55; sec[511] = 0xAA;
}

static int ins_write_mbr(INS_BIO *bio, INS_FAT *f)
{
    ins_zeromem(ins_sec, ins_bs);
    ins_wr32(ins_sec + 440, 0x57435450);   /* 磁盘签名：引导项 HD() 节点按此匹配分区 */
    ins_sec[446 + 0] = 0x80;
    ins_sec[446 + 1] = 0x00; ins_sec[446 + 2] = 0x02; ins_sec[446 + 3] = 0x00;
    ins_sec[446 + 4] = 0x0C;
    ins_sec[446 + 5] = 0xFE; ins_sec[446 + 6] = 0xFF; ins_sec[446 + 7] = 0xFF;
    ins_wr32(ins_sec + 446 + 8, (UINT32)f->part_lba);
    ins_wr32(ins_sec + 446 + 12, (UINT32)f->part_sectors);
    ins_sec[510] = 0x55; ins_sec[511] = 0xAA;
    return ins_write(bio, 0, 1, ins_sec);
}

static int ins_write_fsinfo(INS_BIO *bio, UINT64 lba, INS_FAT *f)
{
    ins_zeromem(ins_sec, ins_bs);
    ins_wr32(ins_sec + 0, 0x41615252);
    ins_wr32(ins_sec + 484, 0x61417272);
    ins_wr32(ins_sec + 488, f->clusters - 1);
    ins_wr32(ins_sec + 492, 3);
    ins_wr16(ins_sec + 510, 0xAA55);
    return ins_write(bio, lba, 1, ins_sec);
}

/* FAT[0..2] 填保留簇，根目录簇清零，并补根目录卷标项（BPB 与根目录卷标必须一致，否则 fsck 报警） */
static int ins_build_fat(INS_BIO *bio, INS_FAT *f)
{
    UINT64 fat0 = f->part_lba + f->reserved;
    for (int copy = 0; copy < 2; ++copy) {
        UINT64 base = fat0 + (UINT64)copy * f->fatsz;
        UINT8 *first = ins_sec;
        ins_zeromem(first, ins_bs);
        ins_wr32(first + 0, 0x0FFFFFF8);
        ins_wr32(first + 4, 0x0FFFFFFF);
        ins_wr32(first + 8, 0x0FFFFFFF);
        if (!ins_write(bio, base, 1, first)) return 0;
        if (f->fatsz > 1 && !ins_zero_range(bio, base + 1, (UINT64)(f->fatsz - 1))) return 0;
    }
    UINT64 root = fat0 + (UINT64)2 * f->fatsz;
    if (!ins_zero_range(bio, root, (UINT64)f->spc)) return 0;
    ins_zeromem(ins_sec, ins_bs);
    const char *label = INS_LABEL "     ";
    for (int i = 0; i < 11; ++i) ins_sec[i] = (UINT8)label[i];
    ins_sec[11] = 0x08;              /* ATTR_VOLUME_ID */
    if (!ins_write(bio, root, 1, ins_sec)) return 0;
    return 1;
}

static int ins_format_fat32(INS_BIO *bio, UINT64 total_sectors)
{
    INS_FAT f;
    if (!ins_buffers_alloc(bio)) return 0;
    if (!ins_fat_plan(&f, total_sectors)) return 0;
    out_ascii("[install] fat32 spc="); ins_num(f.spc);
    out_ascii(" fatsz="); ins_num(f.fatsz);
    out_ascii(" clusters="); ins_num(f.clusters); outln(L"");

    if (!ins_write_mbr(bio, &f)) { ins_fail("mbr"); return 0; }
    ins_build_bpb(ins_sec, &f);
    if (!ins_write(bio, f.part_lba, 1, ins_sec)) { ins_fail("bpb"); return 0; }
    if (!ins_write_fsinfo(bio, f.part_lba + 1, &f)) { ins_fail("fsinfo"); return 0; }
    if (!ins_write_fsinfo(bio, f.part_lba + 7, &f)) { ins_fail("fsinfo2"); return 0; }
    ins_build_bpb(ins_sec, &f);
    if (!ins_write(bio, f.part_lba + 6, 1, ins_sec)) { ins_fail("bpb2"); return 0; }
    if (!ins_build_fat(bio, &f)) { ins_fail("fat"); return 0; }

    /* 记住布局，后面用原生 FAT32 写入，不依赖固件重新挂载目标分区 */
    g_fat = f;
    g_fbio = bio;

    if (bio->FlushBlocks) bio->FlushBlocks(bio);

    if (!ins_read(bio, f.part_lba, 1, ins_sec)) { ins_fail("readback"); return 0; }
    if (ins_rd16(ins_sec + 11) != 512 || ins_rd32(ins_sec + 36) != f.fatsz) {
        ins_fail("verify");
        return 0;
    }
    ins_msg("[install] format done, readback verified", L"[install] 格式化完成，已回读校验");
    return 1;
}

/* ---------- 原生 FAT32 写入（不依赖固件重新挂载） ---------- */

static INS_FAT g_fat;
static INS_BIO *g_fbio;
static UINT8 g_fatsec[512];
static UINT8 g_dirsec[512];

static int fat_ready(void)
{
    return g_fbio && ins_bs == 512 && g_fat.clusters > 16 && g_fat.spc > 0;
}

static UINT64 fat_cluster_lba(UINT32 c)
{
    return g_fat.part_lba + g_fat.reserved + (UINT64)g_fat.fatsz * 2 +
           (UINT64)(c - 2) * g_fat.spc;
}

static int fat_zero_cluster(UINT32 c)
{
    UINT64 lba = fat_cluster_lba(c);
    ins_zeromem(g_dirsec, 512);
    for (UINT32 s = 0; s < g_fat.spc; ++s)
        if (!ins_write(g_fbio, lba + s, 1, g_dirsec)) return 0;
    return 1;
}

static int fat_write_entry(UINT32 c, UINT32 v)
{
    UINT64 off = (UINT64)c * 4;
    UINT64 sec = off / 512;
    UINTN pos = (UINTN)(off % 512);
    UINT64 fat0 = g_fat.part_lba + g_fat.reserved;
    for (int copy = 0; copy < 2; ++copy) {
        if (!ins_read(g_fbio, fat0 + (UINT64)copy * g_fat.fatsz + sec, 1, g_fatsec)) return 0;
        ins_wr32(g_fatsec + pos, v & 0x0FFFFFFF);
        if (!ins_write(g_fbio, fat0 + (UINT64)copy * g_fat.fatsz + sec, 1, g_fatsec)) return 0;
    }
    return 1;
}

static UINT32 fat_alloc_cluster(void)
{
    UINT64 fat0 = g_fat.part_lba + g_fat.reserved;
    for (UINT64 s = 0; s < g_fat.fatsz; ++s) {
        if (!ins_read(g_fbio, fat0 + s, 1, g_fatsec)) return 0;
        for (int i = 0; i < 128; ++i) {
            UINT32 c = (UINT32)(s * 128 + i);
            if (c < 3 || c >= g_fat.clusters + 2) continue;
            if ((ins_rd32(g_fatsec + i * 4) & 0x0FFFFFFF) != 0) continue;
            ins_wr32(g_fatsec + i * 4, 0x0FFFFFFF);
            if (!ins_write(g_fbio, fat0 + s, 1, g_fatsec)) return 0;
            if (!ins_read(g_fbio, fat0 + g_fat.fatsz + s, 1, g_fatsec)) return 0;
            ins_wr32(g_fatsec + i * 4, 0x0FFFFFFF);
            if (!ins_write(g_fbio, fat0 + g_fat.fatsz + s, 1, g_fatsec)) return 0;
            if (!fat_zero_cluster(c)) return 0;
            ++g_fat_used;
            return c;
        }
    }
    return 0;
}

static void fat_name11(char out[11], const char *name)
{
    int i = 0;
    for (; i < 11; ++i) out[i] = ' ';
    int p = 0;
    while (name[p]) ++p;
    /* 支持 "NAME.EXT" 形式，本体最多 8 字符、扩展名最多 3 字符，全大写 */
    int dot = -1;
    for (int k = 0; k < p; ++k) if (name[k] == '.') dot = k;
    int mainlen = (dot >= 0) ? dot : p;
    if (mainlen > 8) mainlen = 8;
    for (int k = 0; k < mainlen; ++k) {
        char c = name[k];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[k] = c;
    }
    if (dot >= 0) {
        int e = 0;
        for (int k = dot + 1; k < p && e < 3; ++k, ++e) {
            char c = name[k];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[8 + e] = c;
        }
    }
}

static int fat_dir_add(UINT32 parent, const char *name, UINT8 attr, UINT32 first, UINT32 size)
{
    char n11[11];
    fat_name11(n11, name);
    UINT32 c = parent;
    while (c >= 2 && c < g_fat.clusters + 2) {
        UINT64 lba = fat_cluster_lba(c);
        for (UINT32 s = 0; s < g_fat.spc; ++s) {
            if (!ins_read(g_fbio, lba + s, 1, g_dirsec)) return 0;
            for (int e = 0; e < 16; ++e) {
                UINT8 *p = g_dirsec + e * 32;
                if (p[0] != 0x00 && p[0] != 0xE5) continue;
                ins_zeromem(p, 32);
                for (int k = 0; k < 11; ++k) p[k] = (UINT8)n11[k];
                p[11] = attr;
                ins_wr16(p + 20, (UINT16)(first >> 16));
                ins_wr16(p + 26, (UINT16)(first & 0xFFFF));
                ins_wr32(p + 28, size);
                return ins_write(g_fbio, lba + s, 1, g_dirsec);
            }
        }
        UINT32 next = 0;
        UINT64 off = (UINT64)c * 4;
        if (!ins_read(g_fbio, g_fat.part_lba + g_fat.reserved + off / 512, 1, g_fatsec)) return 0;
        next = ins_rd32(g_fatsec + (UINTN)(off % 512)) & 0x0FFFFFFF;
        if (next >= 0x0FFFFFF8 || next == 0) {
            UINT32 nc = fat_alloc_cluster();
            if (!nc) return 0;
            if (!fat_write_entry(c, nc)) return 0;
            c = nc;
        } else {
            c = next;
        }
    }
    return 0;
}

static int fat_make_dir(UINT32 parent, const char *name, UINT32 *out)
{
    UINT32 c = fat_alloc_cluster();
    if (!c) return 0;
    UINT64 lba = fat_cluster_lba(c);
    if (!ins_read(g_fbio, lba, 1, g_dirsec)) return 0;
    ins_zeromem(g_dirsec, 512);
    for (int k = 0; k < 11; ++k) g_dirsec[k] = (UINT8)(k == 0 ? '.' : ' ');
    g_dirsec[11] = 0x10;
    ins_wr16(g_dirsec + 20, (UINT16)(c >> 16));
    ins_wr16(g_dirsec + 26, (UINT16)(c & 0xFFFF));
    for (int k = 0; k < 11; ++k) g_dirsec[32 + k] = (UINT8)(k < 1 ? '.' : (k == 1 ? '.' : ' '));
    g_dirsec[32 + 11] = 0x10;
    ins_wr16(g_dirsec + 32 + 20, (UINT16)(parent >> 16));
    ins_wr16(g_dirsec + 32 + 26, (UINT16)(parent == 2 ? 0 : (parent & 0xFFFF)));
    if (!ins_write(g_fbio, lba, 1, g_dirsec)) return 0;
    if (!fat_dir_add(parent, name, 0x10, c, 0)) return 0;
    *out = c;
    return 1;
}

static int fat_write_file(UINT32 dir, const char *name, const UINT8 *data, UINT32 size)
{
    if (!size) return 0;
    UINT32 per = (UINT32)g_fat.spc * 512;
    UINT32 need = (size + per - 1) / per;
    UINT32 first = 0, prev = 0;
    UINT32 off = 0;
    for (UINT32 n = 0; n < need; ++n) {
        UINT32 c = fat_alloc_cluster();
        if (!c) return 0;
        if (!first) first = c; else if (!fat_write_entry(prev, c)) return 0;
        prev = c;
        UINT64 lba = fat_cluster_lba(c);
        for (UINT32 s = 0; s < g_fat.spc; ++s) {
            ins_zeromem(g_dirsec, 512);
            if (off < size) {
                UINT32 take = size - off;
                if (take > 512) take = 512;
                ins_memcopy(g_dirsec, data + off, take);
                off += take;
            }
            if (!ins_write(g_fbio, lba + s, 1, g_dirsec)) return 0;
        }
    }
    if (!fat_write_entry(prev, 0x0FFFFFFF)) return 0;
    return fat_dir_add(dir, name, 0x20, first, size);
}

static int fat_lookup(UINT32 dir, const char *name, UINT32 *first, UINT32 *size)
{
    char n11[11];
    fat_name11(n11, name);
    UINT32 c = dir;
    while (c >= 2 && c < g_fat.clusters + 2) {
        UINT64 lba = fat_cluster_lba(c);
        for (UINT32 s = 0; s < g_fat.spc; ++s) {
            if (!ins_read(g_fbio, lba + s, 1, g_dirsec)) return 0;
            for (int e = 0; e < 16; ++e) {
                UINT8 *p = g_dirsec + e * 32;
                if (p[0] == 0x00) return 0;
                if (p[0] == 0xE5) continue;
                if (p[11] == 0x0F) continue;
                int same = 1;
                for (int k = 0; k < 11; ++k) if (p[k] != (UINT8)n11[k]) { same = 0; break; }
                if (!same) continue;
                *first = (UINT32)(ins_rd16(p + 26) | ((UINT32)ins_rd16(p + 20) << 16));
                *size = ins_rd32(p + 28);
                return 1;
            }
        }
        UINT64 off = (UINT64)c * 4;
        if (!ins_read(g_fbio, g_fat.part_lba + g_fat.reserved + off / 512, 1, g_fatsec)) return 0;
        UINT32 next = ins_rd32(g_fatsec + (UINTN)(off % 512)) & 0x0FFFFFFF;
        if (next >= 0x0FFFFFF8) return 0;
        c = next;
    }
    return 0;
}

/* 从安装介质把 BOOTX64.EFI 整段读进内存 */
static UINT8 *ins_load_boot(UINT32 *size_out)
{
    EFI_FILE_PROTOCOL *e = ins_step_open(ROOT, L"EFI", 1);
    if (!e) return NULL;
    EFI_FILE_PROTOCOL *b = ins_step_open(e, L"BOOT", 1);
    e->Close(e);
    if (!b) return NULL;
    EFI_FILE_PROTOCOL *f = ins_step_open(b, L"BOOTX64.EFI", 0);
    b->Close(b);
    if (!f) return NULL;
    UINT8 *buf = NULL;
    if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, 4 * 1024 * 1024, (void **)&buf)) || !buf) {
        f->Close(f);
        return NULL;
    }
    UINT32 total = 0;
    for (;;) {
        UINTN n = 256 * 1024;
        if (total + n > 4 * 1024 * 1024) break;
        EFI_STATUS st = f->Read(f, &n, buf + total);
        if (EFI_IS_ERROR(st) || n == 0) break;
        total += (UINT32)n;
    }
    f->Close(f);
    if (!total) { ST->BootServices->FreePool(buf); return NULL; }
    *size_out = total;
    return buf;
}

/* 原生安装：建 EFI/BOOT 目录并写入 BOOTX64.EFI，再逐项校验目录项 */
static int ins_native_install(UINT32 *size_out)
{
    if (!fat_ready()) { ins_fail("fatstate"); return 0; }
    UINT32 sz = 0;
    UINT8 *data = ins_load_boot(&sz);
    if (!data) { ins_fail("nobootfile"); return 0; }
    out_ascii("[install] native fat32 write, boot file bytes="); ins_num(sz); outln(L"");

    UINT32 efi = 0, boot = 0;
    if (!fat_make_dir(2, "EFI", &efi)) { ST->BootServices->FreePool(data); ins_fail("mkefi"); return 0; }
    if (!fat_make_dir(efi, "BOOT", &boot)) { ST->BootServices->FreePool(data); ins_fail("mkboot"); return 0; }
    if (!fat_write_file(boot, "BOOTX64.EFI", data, sz)) {
        ST->BootServices->FreePool(data);
        ins_fail("writeboot");
        return 0;
    }
    ST->BootServices->FreePool(data);

    UINT32 e2 = 0, s2 = 0, b1 = 0, b2 = 0, f1 = 0, f2 = 0;
    if (!fat_lookup(2, "EFI", &e2, &s2)) { ins_fail("verify-efi"); return 0; }
    if (!fat_lookup(e2, "BOOT", &b1, &b2)) { ins_fail("verify-boot"); return 0; }
    if (!fat_lookup(b1, "BOOTX64.EFI", &f1, &f2)) { ins_fail("verify-file"); return 0; }
    out_ascii("[install] verify dir efi="); ins_num(e2);
    out_ascii(" boot="); ins_num(b1);
    out_ascii(" file="); ins_num(f1);
    out_ascii(" size="); ins_num(f2); outln(L"");
    if (f2 != sz || !f1) { ins_fail("verify-size"); return 0; }
    if (size_out) *size_out = f2;
    ins_msg("[install] files written to target volume", L"[install] 系统文件已写入目标分区");
    return 1;
}

/* FSInfo 的空闲簇计数必须与实际分配一致，否则 fsck 与部分固件会报异常 */
static void fat_sync_fsinfo(void)
{
    UINT32 freecount = (g_fat.clusters > g_fat_used + 4) ? g_fat.clusters - g_fat_used - 1 : 0;
    for (int copy = 0; copy < 2; ++copy) {
        UINT64 lba = g_fat.part_lba + 1 + (UINT64)copy * 6;
        ins_zeromem(ins_sec, ins_bs);
        ins_wr32(ins_sec + 0, 0x41615252);
        ins_wr32(ins_sec + 484, 0x61417272);
        ins_wr32(ins_sec + 488, freecount);
        ins_wr32(ins_sec + 492, 3);
        ins_wr16(ins_sec + 510, 0xAA55);
        ins_write(g_fbio, lba, 1, ins_sec);
    }
}

/* ---------- 复制系统文件 ---------- */

static void ins_connect_disks(void)
{
    EFI_HANDLE *buf = NULL; UINTN n = 0;
    INS_CONNECT cc = (INS_CONNECT)ST->BootServices->ConnectController;
    if (EFI_IS_ERROR(ST->BootServices->LocateHandleBuffer(2, &ins_bio_guid, NULL, &n, &buf)) || !buf)
        return;
    for (UINTN i = 0; i < n; ++i) {
        INS_BIO *bio = NULL;
        if (EFI_IS_ERROR(ST->BootServices->HandleProtocol(buf[i], &ins_bio_guid, (void **)&bio)) || !bio)
            continue;
        if (!bio->Media || !bio->Media->MediaPresent || !bio->Media->LogicalPartition) continue;
        cc(buf[i], NULL, NULL, NULL, 1);
    }
    ST->BootServices->FreePool(buf);
}

/* 设备路径前缀比较：卷句柄的路径前段必须与磁盘句柄完全一致（分区节点在其后） */
static int ins_dp_prefix(const UINT8 *disk_dp, const UINT8 *vol_dp)
{
    UINTN i = 0;
    while (i < 900) {
        if (disk_dp[i] == 0x7F) return 1;
        if (disk_dp[i] == 0x04) return 0;
        UINT16 la = ins_rd16(disk_dp + i + 2);
        UINT16 lb = ins_rd16(vol_dp + i + 2);
        if (!la || !lb || la != lb) return 0;
        for (UINT16 k = 0; k < la; ++k)
            if (disk_dp[i + k] != vol_dp[i + k]) return 0;
        i += la;
    }
    return 1;
}

static int ins_vol_on_disk(EFI_HANDLE diskh, EFI_HANDLE volh)
{
    void *d = NULL, *v = NULL;
    if (EFI_IS_ERROR(ST->BootServices->HandleProtocol(diskh, &device_path_guid, &d)) || !d) return 0;
    if (EFI_IS_ERROR(ST->BootServices->HandleProtocol(volh, &device_path_guid, &v)) || !v) return 0;
    return ins_dp_prefix((const UINT8 *)d, (const UINT8 *)v);
}

static EFI_FILE_PROTOCOL *ins_vol_try(EFI_HANDLE volh, UINT64 min_bytes, UINT64 *size_out)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    if (EFI_IS_ERROR(ST->BootServices->HandleProtocol(volh, &simple_fs_guid, (void **)&fs)) || !fs) return NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    if (EFI_IS_ERROR(fs->OpenVolume(fs, &root)) || !root) return NULL;
    UINT8 ib[1024];
    ins_zeromem(ib, sizeof(ib));
    EFI_FILE_SYSTEM_INFO *info = (EFI_FILE_SYSTEM_INFO *)ib;
    UINTN isz = sizeof(ib);
    if (EFI_IS_ERROR(root->GetInfo(root, &fs_info_guid, &isz, ib)) || !info->VolumeSize ||
        info->VolumeSize < min_bytes) {
        root->Close(root);
        return NULL;
    }
    if (size_out) *size_out = info->VolumeSize;
    return root;
}

/* 目标卷必须挂在那块刚格式化的盘上；找不到就重试几轮，实在找不到才退回“最大的够大卷” */
static EFI_FILE_PROTOCOL *ins_open_volume(INS_DISK *target, EFI_HANDLE *handle_out)
{
    EFI_FILE_PROTOCOL *best = NULL;
    EFI_HANDLE besth = NULL;
    UINT64 bestsize = 0;
    for (int round = 0; round < 3; ++round) {
        EFI_HANDLE *buf = NULL; UINTN n = 0;
        ins_connect_disks();
        if (EFI_IS_ERROR(ST->BootServices->LocateHandleBuffer(2, &simple_fs_guid, NULL, &n, &buf)) || !buf) {
            ST->BootServices->Stall(300000);
            continue;
        }
        for (UINTN i = 0; i < n; ++i) {
            UINT64 sz = 0;
            EFI_FILE_PROTOCOL *r = ins_vol_try(buf[i], INS_MIN_BYTES, &sz);
            if (!r) continue;
            if (ins_vol_on_disk(target->h, buf[i])) {
                if (best) best->Close(best);
                best = r;
                bestsize = sz;
                besth = buf[i];
                break;
            }
            if (sz > bestsize) {
                if (best) best->Close(best);
                best = r;
                bestsize = sz;
                besth = buf[i];
            } else {
                r->Close(r);
            }
        }
        ST->BootServices->FreePool(buf);
        if (best && ins_vol_on_disk(target->h, besth)) break;
        ST->BootServices->Stall(300000);
    }
    if (handle_out) *handle_out = besth;
    if (best) {
        out_ascii("[install] target volume "); ins_num(bestsize / (1024ULL * 1024)); out_ascii(" MB");
        outln(ins_vol_on_disk(target->h, besth) ? L" [bound]" : L" [fallback]");
    }
    return best;
}

static int ins_eq_ci(const CHAR16 *a, const CHAR16 *b)
{
    while (*a && *b) {
        CHAR16 x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (CHAR16)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (CHAR16)(y + 32);
        if (x != y) return 0;
    }
    return !*a && !*b;
}

static int ins_ends_ci(const CHAR16 *s, const CHAR16 *suf)
{
    UINTN n = 0, m = 0;
    while (s[n]) ++n;
    while (suf[m]) ++m;
    if (m > n) return 0;
    for (UINTN i = 0; i < m; ++i) {
        CHAR16 x = s[n - m + i], y = suf[i];
        if (x >= 'A' && x <= 'Z') x = (CHAR16)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (CHAR16)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

/* 安装介质专属文件不复制：
 *   au_early / autorun / autorun.early 是无人值守脚本，带进目标盘会让装好的系统每次开机重装自己；
 *   oobe.cfg 带过去就跳过了新机向导；inststate/install.txt 是安装器状态；
 *   *.log 是运行日志，属于介质不留到系统里。 */
static int ins_excluded(const CHAR16 *name)
{
    static const CHAR16 *skip[] = {
        L"oobe.cfg", L"autorun", L"autorun.early", L"au_early",
        L"inststate", L"install.txt"
    };
    for (int i = 0; i < 6; ++i)
        if (ins_eq_ci(name, skip[i])) return 1;
    if (ins_ends_ci(name, L".log")) return 1;
    return 0;
}

static int ins_mkdir(EFI_FILE_PROTOCOL *dir, const CHAR16 *name)
{
    EFI_FILE_PROTOCOL *d = NULL;
    EFI_STATUS st = dir->Open(dir, &d, (CHAR16 *)name,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY);
    if (EFI_IS_ERROR(st) || !d) return 0;
    d->Close(d);
    return 1;
}

static int ins_copy_file(EFI_FILE_PROTOCOL *src, const CHAR16 *sname,
                         EFI_FILE_PROTOCOL *dst, const CHAR16 *dname)
{
    EFI_FILE_PROTOCOL *f = NULL, *g = NULL;
    if (EFI_IS_ERROR(src->Open(src, &f, (CHAR16 *)sname, EFI_FILE_MODE_READ, 0)) || !f) return 0;
    /* 同名文件先删掉，避免重复安装时旧文件的尾部残留 */
    EFI_FILE_PROTOCOL *old = NULL;
    if (!EFI_IS_ERROR(dst->Open(dst, &old, (CHAR16 *)dname,
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && old) {
        old->Delete(old);
    }
    EFI_STATUS st = dst->Open(dst, &g, (CHAR16 *)dname,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_IS_ERROR(st) || !g) { f->Close(f); return 0; }
    UINT8 *buf = NULL;
    if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, 128 * 1024, (void **)&buf)) || !buf) {
        g->Close(g); f->Close(f); return 0;
    }
    int ok = 1;
    for (;;) {
        UINTN n = 128 * 1024;
        EFI_STATUS rs = f->Read(f, &n, buf);
        if (EFI_IS_ERROR(rs)) { ok = 0; break; }
        if (n == 0) break;
        UINTN w = n;
        if (EFI_IS_ERROR(g->Write(g, &w, buf)) || w != n) { ok = 0; break; }
        ins_bytes += n;
    }
    if (g->Flush) g->Flush(g);
    ST->BootServices->FreePool(buf);
    g->Close(g);
    f->Close(f);
    return ok;
}

static int ins_copy_tree(EFI_FILE_PROTOCOL *src, EFI_FILE_PROTOCOL *dst,
                         int depth, int *files)
{
    if (depth > 8) return 1;
    UINT8 *info = NULL;
    if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, 2048, (void **)&info)) || !info) return 0;
    src->SetPosition(src, 0);
    int ok = 1;
    for (;;) {
        UINTN size = 2048;
        ins_zeromem(info, 2048);
        EFI_STATUS st = src->Read(src, &size, info);
        if (st == INS_BUFFER_SMALL) continue;
        if (EFI_IS_ERROR(st) || size == 0) break;
        INS_FINFO *fi = (INS_FINFO *)info;
        if (!fi->FileName[0]) continue;
        if (fi->FileName[0] == '.' &&
            (!fi->FileName[1] || (fi->FileName[1] == '.' && !fi->FileName[2]))) continue;
        if (ins_excluded(fi->FileName)) continue;

        if (fi->Attribute & EFI_FILE_DIRECTORY) {
            if (!ins_mkdir(dst, fi->FileName)) { ok = 0; break; }
            EFI_FILE_PROTOCOL *sdir = NULL, *ddir = NULL;
            if (EFI_IS_ERROR(src->Open(src, &sdir, fi->FileName, EFI_FILE_MODE_READ, 0)) || !sdir) { ok = 0; break; }
            if (EFI_IS_ERROR(dst->Open(dst, &ddir, fi->FileName,
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, EFI_FILE_DIRECTORY)) || !ddir) {
                sdir->Close(sdir);
                ok = 0;
                break;
            }
            int sub = ins_copy_tree(sdir, ddir, depth + 1, files);
            ddir->Close(ddir);
            sdir->Close(sdir);
            if (!sub) { ok = 0; break; }
        } else {
            out(L"  ");
            ins_name_out(fi->FileName);
            if (ins_copy_file(src, fi->FileName, dst, fi->FileName)) {
                ++(*files);
                outln(L"");
            } else {
                out_ascii("  FAILED\r\n");
                ok = 0;
                break;
            }
        }
    }
    ST->BootServices->FreePool(info);
    return ok;
}

static int ins_copy_system(EFI_FILE_PROTOCOL *target)
{
    if (!ROOT) { ins_fail("nosource"); return 0; }
    int files = 0;
    ins_files = 0;
    ins_bytes = 0;
    int ok = ins_copy_tree(ROOT, target, 0, &files);
    ins_files = (UINT64)files;
    out_ascii("[install] copied files="); ins_num(ins_files);
    out_ascii(" bytes="); ins_num(ins_bytes); outln(L"");
    if (!ok || files == 0) { ins_fail("copy"); return 0; }
    return 1;
}

/* ---------- 回读校验 ---------- */

/* 逐级打开：EFI -> BOOT -> BOOTX64.EFI。
   不能拿 "\EFI\BOOT\BOOTX64.EFI" 这种绝对路径去 Open，各固件 FAT 驱动对前导反斜杠
   处理不一致（实测 OVMF 下直接失败，校验被误判成“源盘没有引导程序”）。
   文件大小也不能靠 GetInfo：部分固件对 EFI_FILE_INFO 返回 EFI_UNSUPPORTED，
   所以校验改成把两边内容全读一遍做哈希。 */
static EFI_FILE_PROTOCOL *ins_step_open(EFI_FILE_PROTOCOL *from, const CHAR16 *name, int is_dir)
{
    EFI_FILE_PROTOCOL *to = NULL;
    EFI_STATUS st = from->Open(from, &to, (CHAR16 *)name, EFI_FILE_MODE_READ,
                               is_dir ? EFI_FILE_DIRECTORY : 0);
    if (EFI_IS_ERROR(st) || !to) return NULL;
    return to;
}

static UINT64 ins_hash_open(EFI_FILE_PROTOCOL *f, UINT64 *bytes_out)
{
    UINT8 *buf = NULL;
    if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, 64 * 1024, (void **)&buf)) || !buf)
        return 0;
    UINT64 h = 0xcbf29ce484222325ULL;
    UINT64 total = 0;
    for (;;) {
        UINTN n = 64 * 1024;
        EFI_STATUS st = f->Read(f, &n, buf);
        if (EFI_IS_ERROR(st) || n == 0) break;
        for (UINTN i = 0; i < n; ++i) {
            h ^= (UINT64)buf[i];
            h *= 0x100000001b3ULL;
        }
        total += (UINT64)n;
    }
    ST->BootServices->FreePool(buf);
    if (bytes_out) *bytes_out = total;
    return h;
}

static int ins_bootfile_probe(EFI_FILE_PROTOCOL *root, UINT64 *hash, UINT64 *size)
{
    *hash = 0;
    *size = 0;
    EFI_FILE_PROTOCOL *e = ins_step_open(root, L"EFI", 1);
    if (!e) return 0;
    EFI_FILE_PROTOCOL *b = ins_step_open(e, L"BOOT", 1);
    e->Close(e);
    if (!b) return 0;
    EFI_FILE_PROTOCOL *f = ins_step_open(b, L"BOOTX64.EFI", 0);
    b->Close(b);
    if (!f) return 0;
    UINT64 sz = 0;
    UINT64 h = ins_hash_open(f, &sz);
    f->Close(f);
    if (!sz) return 0;
    *hash = h;
    *size = sz;
    return 1;
}

static int ins_verify(EFI_FILE_PROTOCOL *target)
{
    if (!ROOT) return 0;
    UINT64 sh = 0, ss = 0, th = 0, ts = 0;
    if (!ins_bootfile_probe(ROOT, &sh, &ss)) {
        ins_msg("[install] source media has no EFI/BOOT/BOOTX64.EFI, nothing would boot",
                L"[install] 安装介质缺少 EFI/BOOT/BOOTX64.EFI，装完无法启动");
        ins_fail("nobootfile");
        return 0;
    }
    if (!ins_bootfile_probe(target, &th, &ts)) { ins_fail("verify-open"); return 0; }
    out_ascii("[install] verify BOOTX64.EFI src="); ins_num(ss);
    out_ascii("/"); ins_num(sh);
    out_ascii(" dst="); ins_num(ts);
    out_ascii("/"); ins_num(th); outln(L"");
    if (ss != ts || sh != th) { ins_fail("verify-hash"); return 0; }
    ins_msg("[install] verified: bootloader copied byte-for-byte",
            L"[install] 校验通过：引导程序逐字节一致");
    return 1;
}

/* ---------- 引导项 ---------- */

static void ins_boot_name(CHAR16 *dst, int idx)
{
    dst[0] = 'B'; dst[1] = 'o'; dst[2] = 'o'; dst[3] = 't';
    for (int i = 7; i >= 4; --i) {
        int d = idx & 15; idx >>= 4;
        dst[i] = (CHAR16)(d < 10 ? ('0' + d) : ('A' + d - 10));
    }
    dst[8] = 0;
}

/* 找一个没人用的 BootXXXX 槽位，避免覆盖别的启动项（旧版硬写 Boot0001 就是覆盖别人的） */
static int ins_boot_slot(void)
{
    EFI_RUNTIME_SERVICES *RT = ST->RuntimeServices;
    if (!RT || !RT->GetVariable) return 1;
    INS_GETVAR get = (INS_GETVAR)RT->GetVariable;
    CHAR16 name[9];
    for (int i = 1; i <= 0xFF; ++i) {
        ins_boot_name(name, i);
        UINT32 attr = 0;
        UINTN sz = 0;
        EFI_STATUS st = get(name, &ins_global_guid, &attr, &sz, NULL);
        if (st == INS_NOT_FOUND) return i;
    }
    return 1;
}

/* 新装的盘要真的能被固件选中：把槽位插到 BootOrder 首位 */
static void ins_boot_order(int slot)
{
    EFI_RUNTIME_SERVICES *RT = ST->RuntimeServices;
    if (!RT || !RT->SetVariable || !RT->GetVariable) return;
    INS_SETVAR set = (INS_SETVAR)RT->SetVariable;
    INS_GETVAR get = (INS_GETVAR)RT->GetVariable;
    UINT16 old[128];
    int n = 0;
    UINT32 attr = 0;
    UINTN want = 0;
    EFI_STATUS qs = get(L"BootOrder", &ins_global_guid, &attr, &want, NULL);
    if (qs == INS_BUFFER_SMALL && want > 0 && want <= sizeof(old)) {
        UINTN rd = want;
        if (!EFI_IS_ERROR(get(L"BootOrder", &ins_global_guid, &attr, &rd, old))) n = (int)(rd / 2);
    }
    UINT16 out2[130];
    int m = 0;
    out2[m++] = (UINT16)slot;
    for (int i = 0; i < n && m < 129; ++i) {
        if (old[i] == (UINT16)slot) continue;
        out2[m++] = old[i];
    }
    EFI_STATUS st = set(L"BootOrder", &ins_global_guid, INS_VAR_ATTR, (UINTN)(m * 2), out2);
    out_ascii("[install] BootOrder entries="); ins_num((UINT64)m);
    out_ascii(" st="); ins_num((UINT64)(unsigned)st); outln(L"");
}

static int ins_boot_entry(EFI_HANDLE volh)
{
    EFI_RUNTIME_SERVICES *RT = ST->RuntimeServices;
    if (!RT || !RT->SetVariable) return 0;
    INS_SETVAR set = (INS_SETVAR)RT->SetVariable;
    void *dp = NULL;
    if (EFI_IS_ERROR(ST->BootServices->HandleProtocol(volh, &device_path_guid, &dp)) || !dp) return 0;
    UINT8 *node = NULL;
    if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, 2048, (void **)&node)) || !node) return 0;
    ins_zeromem(node, 1024);

    UINT8 *p = (UINT8 *)dp;
    UINTN used = 0;
    while (used < 640) {
        if (p[0] == 0x7F) break;
        UINT16 len = ins_rd16(p + 2);
        if (!len || len > 400) break;
        ins_memcopy(node + used, p, len);
        used += len;
        p += len;
    }
    UINT8 *hd = node + used;
    hd[0] = 0x04; hd[1] = 0x01;                    /* Media / HardDrive (HD) */
    ins_wr16(hd + 2, 42);
    ins_wr32(hd + 4, 1);                           /* PartitionNumber */
    ins_wr32(hd + 8, (UINT32)INS_PART_LBA);         /* PartitionStart 低 32 位 */
    ins_wr32(hd + 12, 0);
    ins_wr32(hd + 16, (UINT32)g_fat.part_sectors);  /* PartitionSize 低 32 位 */
    ins_wr32(hd + 20, 0);
    hd[24] = 0x50; hd[25] = 0x54; hd[26] = 0x43; hd[27] = 0x57;  /* MBR 签名 0x57435450 */
    hd[40] = 0x01;                                 /* MBRType: MBR 分区表 */
    hd[41] = 0x01;                                 /* SignatureType: MBR */
    used += 42;

    UINT8 *fp = node + used;
    fp[0] = 0x04; fp[1] = 0x04;
    UINTN pl = 0;
    while (INS_BOOT_FILE[pl] && pl < 120) {
        fp[4 + pl * 2] = (UINT8)INS_BOOT_FILE[pl];
        fp[4 + pl * 2 + 1] = (UINT8)(INS_BOOT_FILE[pl] >> 8);
        ++pl;
    }
    fp[4 + pl * 2] = 0; fp[4 + pl * 2 + 1] = 0;
    UINT16 nlen = (UINT16)(4 + (pl + 1) * 2);
    ins_wr16(fp + 2, nlen);
    UINT8 *endp = fp + nlen;
    endp[0] = 0x7F; endp[1] = 0xFF; endp[2] = 0x04; endp[3] = 0x00;
    UINTN dplen = (UINTN)((endp + 4) - node);

    UINT8 *opt = NULL;
    if (EFI_IS_ERROR(ST->BootServices->AllocatePool(EFI_LOADER_DATA, 2048, (void **)&opt)) || !opt) {
        ST->BootServices->FreePool(node);
        return 0;
    }
    ins_zeromem(opt, 2048);
    ins_wr32(opt, 1);                    /* LOAD_OPTION_ACTIVE */
    ins_wr16(opt + 4, (UINT16)dplen);    /* FilePathListLength */
    const CHAR16 *desc = L"wcteos Pro";
    UINTN dn = 0;
    while (desc[dn] && dn < 40) {
        opt[6 + dn * 2] = (UINT8)desc[dn];
        opt[6 + dn * 2 + 1] = (UINT8)(desc[dn] >> 8);
        ++dn;
    }
    UINTN off = 6 + dn * 2;
    opt[off] = 0; opt[off + 1] = 0;
    off += 2;
    ins_memcopy(opt + off, node, dplen);

    int slot = ins_boot_slot();
    CHAR16 name[9];
    ins_boot_name(name, slot);
    EFI_STATUS st = set(name, &ins_global_guid, INS_VAR_ATTR, off + dplen, opt);
    out_ascii("[install] boot entry ");
    ins_name_out(name);
    out_ascii(" st="); ins_num((UINT64)(unsigned)st); outln(L"");
    ST->BootServices->FreePool(opt);
    ST->BootServices->FreePool(node);
    if (EFI_IS_ERROR(st)) return 0;
    ins_boot_order(slot);
    return 1;
}

/* ---------- 主流程 ---------- */

static int ins_install(int unattended, int reboot_after)
{
    ins_last_error[0] = 0;
    ins_msg("================================================",
            L"================================================");
    ins_msg("  wcteos Pro installer", L"  wcteos Pro 安装程序");
    ins_msg("================================================",
            L"================================================");

    int n = ins_scan();
    out_ascii("[install] disks="); ins_num((UINT64)n); outln(L"");
    if (!n) { ins_fail("nodisk"); return 0; }
    ins_print_disks();

    if (!unattended) {
        if (!ins_pick_disk()) { ins_msg("cancelled", L"已取消"); return 0; }
    } else if (!ins_pick_best()) {
        ins_fail("notarget");
        return 0;
    }
    INS_DISK *t = ins_target_disk;
    if (!t) { ins_fail("notarget"); return 0; }
    out_ascii("[install] target "); ins_num(t->bytes / (1024ULL * 1024)); out_ascii(" MB");
    outln(t->removable ? L" [usb]" : L"");

    if (!unattended) {
        ins_msg("This will erase the target disk. Type yes to continue, no to cancel.",
                L"该操作会清空目标磁盘，输入 yes 继续，输入 no 取消");
        int answer = 0;
        for (int ask = 0; ask < 3 && !answer; ++ask) {
            CHAR16 line[16];
            out(L"Proceed? [yes/no] > ");
            read_line(line, sizeof(line) / sizeof(line[0]));
            if (line[0] == 'y' || line[0] == 'Y') answer = 1;
            else if (line[0] == 'n' || line[0] == 'N') {
                ins_msg("cancelled, disk untouched", L"已取消，磁盘未改动");
                return 0;
            }
            else ins_msg("please type yes or no", L"请输入 yes 或 no");
        }
        if (!answer) { ins_msg("cancelled, disk untouched", L"已取消，磁盘未改动"); return 0; }
    }

    if (!ins_format_fat32(t->bio, t->sectors)) return 0;

    /* 固件在格式化前已缓存旧卷，重挂载不可靠；改为内核直接写 FAT32 */
    UINT32 wrote = 0;
    if (!ins_native_install(&wrote)) return 0;

    fat_sync_fsinfo();
    if (t->bio->FlushBlocks) t->bio->FlushBlocks(t->bio);

    if (ins_boot_entry(t->h))
        ins_msg("[install] boot entry written and moved to BootOrder front",
                L"[install] 引导项已写入并置于 BootOrder 首位");
    else
        ins_msg("[install] boot entry skipped, fallback path EFI/BOOT/BOOTX64.EFI",
                L"[install] 引导项未写入，改用默认路径 EFI/BOOT/BOOTX64.EFI");

    ins_msg("[install] DONE, reboot and boot from the target disk",
            L"[install] 安装完成，重启后从目标磁盘启动");
    if (reboot_after) {
        ST->ConOut->ClearScreen(ST->ConOut);
        ins_msg("[install] rebooting in 3s", L"[install] 3 秒后重启");
        ST->BootServices->Stall(3000000);
        reset_system();
    }
    return 1;
}

static void cmd_install(CHAR16 *args)
{
    int unattended = 0, reboot_after = 0;
    if (args) {
        CHAR16 *p = args;
        while (*p == ' ') ++p;
        while (*p) {
            if (*p == 'a' || *p == 'A') unattended = 1;
            if (*p == 'r' || *p == 'R') reboot_after = 1;
            ++p;
        }
    }
    ins_install(unattended, reboot_after);
}

static void cmd_instfmt(void) { ins_install(1, 0); }

/* 安装介质启动菜单：有 \wct\instmode 时先问一句，不误碰本机数据盘 */
static void installer_menu(void)
{
    ST->ConOut->ClearScreen(ST->ConOut);
    outln(L"");
    outln(L"   ================================================");
    outln(L"     wcteos Pro 1.0 installer medium");
    outln(L"   ================================================");
    outln(L"");
    outln(L"     1  Install wcteos Pro to this disk");
    outln(L"     2  Maintenance shell (do not write disks)");
    outln(L"");
    outln(L"   Press 1 or 2, then Enter");
    for (;;) {
        EFI_INPUT_KEY k;
        if (EFI_IS_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &k))) {
            ST->BootServices->Stall(20000);
            continue;
        }
        if (k.ScanCode != 0) continue;
        if (k.UnicodeChar == '1' || k.UnicodeChar == 13) {
            ins_install(0, 1);
            return;
        }
        if (k.UnicodeChar == '2') return;
    }
}

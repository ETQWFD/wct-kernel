/* WCT 模块示例：只通过内核 API 工作，不链接任何库 */
typedef struct {
    void (*puts)(const char *s);
    void (*putu)(unsigned long long v);
    void (*clear)(void);
    int version;
    const char *kernel_tag;
} WCT_API;

int wct_main(WCT_API *api)
{
    api->puts("  [module] hello from a .wct module\r\n");
    api->puts("  [module] kernel says: ");
    api->puts(api->kernel_tag);
    api->puts("  api=");
    api->putu((unsigned long long)api->version);
    api->puts("\r\n");
    unsigned long long acc = 0;
    for (unsigned long long i = 1; i <= 1000000; ++i) acc += i;
    api->puts("  [module] computed sum 1..1000000 = ");
    api->putu(acc);
    api->puts("\r\n");
    return 0;
}

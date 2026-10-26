/* tpoff (raw-syscall, no libc) - sync CMS temp overrides while charging:
 *   primary_board_temp / fixed_temp = main_batt_temp + offset  (0.1°C units)
 * offset: signed, clamped to [-150, 150]; default -50 (fake 5.0°C cooler).
 *         positive values simulate warming. "tpoff 0" exits immediately.
 * Only fakes while a charger is attached (usb or wls online); on unplug,
 * read failure, or SIGTERM/SIGINT, restores: fixed_temp <- 0 (disable fake),
 * primary_board_temp <- real board temp from battery psy.
 * Faking is also disabled (and restored) while game_bypass_charge_flag >= 1.
 */
typedef long s64;
typedef unsigned long u64;

#define SYS_openat        56
#define SYS_close         57
#define SYS_read          63
#define SYS_write         64
#define SYS_exit          93
#define SYS_nanosleep     101
#define SYS_rt_sigaction  134
#define SYS_rt_sigreturn  139

#define BOARD_NODE "/sys/class/cms_class/primary_board_temp"
#define FIXED_NODE "/sys/class/cms_class/fixed_temp"
#define BATT_NODE  "/sys/class/cms_class/main_batt_temp"
#define REAL_NODE  "/sys/class/power_supply/battery/primary_board_temp"
#define GAME_BYPASS_FLAG "/sys/class/cms_class/game_bypass_charge_flag"
#define USB_ONLINE "/sys/class/power_supply/usb/online"
#define WLS_ONLINE "/sys/class/power_supply/wls/online"
#define LOG_PATH   "/data/local/tmp/tpoff.log"

#define SA_RESTORER 0x04000000

static inline s64 sc(s64 n, s64 a, s64 b, s64 c, s64 d)
{
    register s64 x8 asm("x8") = n;
    register s64 x0 asm("x0") = a;
    register s64 x1 asm("x1") = b;
    register s64 x2 asm("x2") = c;
    register s64 x3 asm("x3") = d;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                 : "memory");
    return x0;
}

static long parse_long(const char *s)
{
    long v = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static long rd(const char *path, long fallback)
{
    char buf[32];
    long fd = sc(SYS_openat, -100, (s64)path, 0, 0);
    if (fd < 0)
        return fallback;
    long n = sc(SYS_read, fd, (s64)buf, 31, 0);
    sc(SYS_close, fd, 0, 0, 0);
    if (n <= 0)
        return fallback;
    buf[n] = 0;
    return parse_long(buf);
}

static void wr(const char *path, long v)
{
    char buf[24], tmp[20];
    int i = 0, j = 0;
    if (v < 0) { buf[i++] = '-'; v = -v; }
    do { tmp[j++] = '0' + (int)(v % 10); v /= 10; } while (v);
    while (j)
        buf[i++] = tmp[--j];
    long fd = sc(SYS_openat, -100, (s64)path, 1, 0);   /* O_WRONLY */
    if (fd >= 0) {
        sc(SYS_write, fd, (s64)buf, i, 0);
        sc(SYS_close, fd, 0, 0, 0);
    }
}

static void log_msg(const char *m)
{
    unsigned n = 0;
    while (m[n])
        n++;
    long fd = sc(SYS_openat, -100, (s64)LOG_PATH, 1 | 0100 | 02000, 0644);
    if (fd >= 0) {
        sc(SYS_write, fd, (s64)m, n, 0);
        sc(SYS_close, fd, 0, 0, 0);
    }
}

static void restore(void)
{
    long real = rd(REAL_NODE, -10000);
    if (real > -10000)
        wr(BOARD_NODE, real);
    wr(FIXED_NODE, 0);
}

static void on_exit(int sig)
{
    (void)sig;
    restore();
    log_msg("tpoff: exit, restored\n");
    sc(SYS_exit, 0, 0, 0, 0);
    __builtin_unreachable();
}

static void __attribute__((naked)) sig_restorer(void)
{
    asm volatile("mov x8, %0\nsvc #0" :: "i"(SYS_rt_sigreturn));
}

struct ksigaction { u64 handler, flags, restorer, mask; };

static void setsig(int sig, void *handler)
{
    struct ksigaction a = { (u64)handler, SA_RESTORER, (u64)sig_restorer, 0 };
    sc(SYS_rt_sigaction, sig, (s64)&a, 0, 8);
}

void __attribute__((noreturn)) run(long argc, char **argv)
{
    long off = -50;
    int faking = 0;

    if (argc > 1) {
        off = parse_long(argv[1]);
        if (off == 0)
            sc(SYS_exit, 0, 0, 0, 0);
        if (off > 150)
            off = 150;
        else if (off < -150)
            off = -150;
    }

    setsig(15, on_exit);   /* SIGTERM */
    setsig(2, on_exit);    /* SIGINT  */

    for (;;) {
        int plugged = rd(USB_ONLINE, 0) == 1 || rd(WLS_ONLINE, 0) == 1;
        long batt = rd(BATT_NODE, -10000);
        long gb = rd(GAME_BYPASS_FLAG, 0);
        if (plugged && batt > -10000 && gb < 1) {
            long fake = batt + off;
            /* hard cap: fake battery temps above ~55C make the battery
             * over-temp protection report to Android thermal service and
             * the system shuts down (observed: shutdown,thermal,battery) */
            if (fake > 550)
                fake = 550;
            if (!faking) {
                faking = 1;
                log_msg("tpoff: plug in, faking\n");
            }
            wr(BOARD_NODE, fake);
            wr(FIXED_NODE, fake);
        } else if (faking) {
            faking = 0;
            restore();
            log_msg(gb >= 1 ? "tpoff: game bypass active, faking disabled, restored\n"
                            : "tpoff: unplug or read fail, restored\n");
        }
        {
            long ts[2] = { 5, 0 };
            sc(SYS_nanosleep, (s64)ts, 0, 0, 0);
        }
    }
}

void __attribute__((naked)) _start(void)
{
    asm volatile("ldr x0, [sp]\n"      /* argc */
                 "add x1, sp, #8\n"    /* argv */
                 "b run");
}

/* cmsboost (raw-syscall, no libc) - EXPERIMENTAL, not proven to speed up
 * charging. It only raises the charger's constant-current setpoint.
 *
 * What is verified:
 *   - the FCC votable force really moves POWER_SUPPLY_CONSTANT_CHARGE_CURRENT
 *     (2700000 -> 3500000 and back), and CMS's 2 s vote does not override it;
 *   - on a 5 V/2 A adapter the actual battery current does NOT change
 *     (input-limited); on 60 W PPS and OPPO UFCS adapters the CMS parameter
 *     set is "sp" and this daemon never engages (CC is already 3.5 A).
 * So there is no measurement yet showing a real charging-speed gain.
 *
 * Background: with a charger that is NOT recognised as vivo flash, the CMS
 * intell parameter set is "chg" and its table is flat at 2700 mA below ~41 C.
 * Forcing FCC bypasses that table.
 *
 * This daemon keeps a forced ceiling only while it is safe and only when the
 * CMS table is actually holding the current below the ceiling:
 *   - charger attached (battery current < -50 mA, or usb/wls online)
 *   - primary_board_temp < 380 and main_batt_temp < 380
 *   - current CC < ceiling - 100 (otherwise nothing to win)
 * releases (force_active=0) as soon as either temp reaches 400, on unplug,
 * when another path wants more than ceiling + 300, and on SIGTERM/SIGINT.
 *
 * Usage: cmsboost [ceiling_ma] [on_temp] [off_temp]   (default 3500 380 400)
 */
typedef long s64;
typedef unsigned long u64;

#define SYS_mount         40
#define SYS_openat        56
#define SYS_close         57
#define SYS_read          63
#define SYS_write         64
#define SYS_exit          93
#define SYS_nanosleep     101
#define SYS_rt_sigaction  134
#define SYS_rt_sigreturn  139

#define DBG        "/sys/kernel/debug"
#define FCC_ACTIVE DBG "/pmic-votable/FCC/force_active"
#define FCC_VALUE  DBG "/pmic-votable/FCC/force_val"
#define BOARD_NODE "/sys/class/cms_class/primary_board_temp"
#define BATT_NODE  "/sys/class/cms_class/main_batt_temp"
#define USB_ONLINE "/sys/class/power_supply/usb/online"
#define WLS_ONLINE "/sys/class/power_supply/wls/online"
#define CHG_UEVENT "/sys/class/power_supply/primary_chg/uevent"
#define LOG_PATH   "/data/local/tmp/cmsboost.log"

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

static inline s64 sc5(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e)
{
    register s64 x8 asm("x8") = n;
    register s64 x0 asm("x0") = a;
    register s64 x1 asm("x1") = b;
    register s64 x2 asm("x2") = c;
    register s64 x3 asm("x3") = d;
    register s64 x4 asm("x4") = e;
    asm volatile("svc #0" : "+r"(x0)
                 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
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

static int starts(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}

/* current charger CC setpoint in mA, -1 if unknown */
static long read_cc_ma(void)
{
    char buf[512];
    const char *key = "CONSTANT_CHARGE_CURRENT=";
    long fd = sc(SYS_openat, -100, (s64)CHG_UEVENT, 0, 0);
    long n, i;

    if (fd < 0)
        return -1;
    n = sc(SYS_read, fd, (s64)buf, 511, 0);
    sc(SYS_close, fd, 0, 0, 0);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    for (i = 0; i + 24 < n; i++) {
        if (buf[i] == 'C' && starts(buf + i, key))
            return parse_long(buf + i + 24) / 1000;
    }
    return -1;
}

static int have_debugfs(void){
    long fd = sc(SYS_openat, -100, (s64)FCC_ACTIVE, 0, 0);
    if (fd >= 0) {
        sc(SYS_close, fd, 0, 0, 0);
        return 1;
    }
    return 0;
}

static int ensure_debugfs(void)
{
    if (have_debugfs())
        return 0;
    sc5(SYS_mount, (s64)"none", (s64)DBG, (s64)"debugfs", 0, 0);
    return have_debugfs() ? 0 : -1;
}

static void release(void)
{
    wr(FCC_ACTIVE, 0);
}

static void on_exit(int sig)
{
    (void)sig;
    release();
    log_msg("cmsboost: exit, released\n");
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
    long ceiling = 3500, t_on = 380, t_off = 400;
    int forcing = 0;

    if (argc > 1) ceiling = parse_long(argv[1]);
    if (argc > 2) t_on = parse_long(argv[2]);
    if (argc > 3) t_off = parse_long(argv[3]);
    if (ceiling < 500) ceiling = 500;
    if (ceiling > 4000) ceiling = 4000;
    if (t_off <= t_on) t_off = t_on + 10;

    setsig(15, on_exit);
    setsig(2, on_exit);

    if (ensure_debugfs()) {
        log_msg("cmsboost: no pmic-votable, abort\n");
        sc(SYS_exit, 1, 0, 0, 0);
    }
    wr(FCC_VALUE, ceiling);
    wr(FCC_ACTIVE, 0);
    log_msg("cmsboost: start, debugfs ready\n");

    for (;;) {
        int plugged = rd(USB_ONLINE, 0) == 1 || rd(WLS_ONLINE, 0) == 1;
        long board = rd(BOARD_NODE, -10000);
        long batt = rd(BATT_NODE, -10000);
        long cc = read_cc_ma();
        int hot = board >= t_off || batt >= t_off;
        int cool = board > -10000 && batt > -10000 &&
                   board < t_on && batt < t_on;

        if (!forcing) {
            /* only take over when CMS is actually holding us below the
             * ceiling (chg table = 2700); never cap a fast-charge session */
            if (plugged && cool && cc > 100 && cc < ceiling - 100) {
                wr(FCC_ACTIVE, 1);
                forcing = 1;
                log_msg("cmsboost: force on\n");
            }
        } else if (!plugged || hot) {
            release();
            forcing = 0;
            log_msg(hot ? "cmsboost: hot, released\n"
                        : "cmsboost: unplug, released\n");
        } else if (cc > ceiling + 300) {
            /* someone (sp/eq table, CP path) wants more than our ceiling */
            release();
            forcing = 0;
            log_msg("cmsboost: table wants more, released\n");
        }
        {
            long ts[2] = { 2, 0 };
            sc(SYS_nanosleep, (s64)ts, 0, 0, 0);
        }
    }
}

void __attribute__((naked)) _start(void)
{
    asm volatile("ldr x0, [sp]\n"
                 "add x1, sp, #8\n"
                 "b run");
}

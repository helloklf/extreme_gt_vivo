/* tpoff (raw-syscall, no libc) - unified CMS temperature offset for vivo.
 *
 * Two device families behave differently, detected at install time:
 *
 *   mode=board -> fixed_temp shadows the battery temperature (X300 / MTK):
 *                 only primary_board_temp is overridden.
 *   mode=both  -> fixed_temp is independent (iQOO15 / QCOM): primary_board_temp
 *                 and fixed_temp are overridden together.
 *
 * Usage:
 *   tpoff [offset]     offset in 0.1C, default -50, clamped to [-150,150];
 *                      0 exits immediately
 *   tpoff [offset] [mode] [fixed_temp_scale]
 *                      mode is board or both; scale is normally 1 or 10.
 *
 * Active only while battery status is "Charging" and while
 * game_bypass_charge_flag < 1.  On unplug / read failure the override is
 * removed.  Turbo mode disables CMS while charging below 41.0C and 30%
 * capacity, unless game bypass is active.
 *
 * Safety: every value written is clamped to [15.0C, 55.0C] and read back
 * (one retry) before it is trusted; a base value outside [-40C, 125C] is
 * treated as a read error and the override is dropped.
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
#define MAIN_NODE  "/sys/class/cms_class/main_batt_temp"
#define BTB_NODE   "/sys/class/cms_class/bat_btb_temp"
#define GAME_BYPASS_FLAG "/sys/class/cms_class/game_bypass_charge_flag"
#define CMS_ENABLE "/sys/class/cms_class/cms_enable"
#define BATT_STATUS "/sys/class/power_supply/battery/status"
#define BATT_CAPACITY "/sys/class/power_supply/battery/capacity"
#define LOG_PATH   "/data/local/tmp/tpoff.log"

#define SA_RESTORER 0x04000000

/* hard bounds for any temperature we write: never below 15.0C, never above
 * 55.0C (0.1C units).  Below 15C the CMS tables are already saturated and a
 * bad sensor read would otherwise produce a nonsense override. */
#define TEMP_MIN    150
#define TEMP_MAX    550
#define BASE_MIN    (-400)   /* -40.0C, treat anything lower as a read error */
#define BASE_MAX    1250     /* 125.0C */
#define BTB_ADJUST   60       /* bat_btb_temp is about 6.0C above main */

#define MODE_BOARD  1   /* board-only (X300) */
#define MODE_BOTH   2   /* fixed_temp + board (iQOO15) */

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

static int charging(void)
{
    char buf[16];
    long fd = sc(SYS_openat, -100, (s64)BATT_STATUS, 0, 0);
    long n;
    if (fd < 0)
        return 0;
    n = sc(SYS_read, fd, (s64)buf, 15, 0);
    sc(SYS_close, fd, 0, 0, 0);
    return n >= 8 &&
           buf[0] == 'C' && buf[1] == 'h' && buf[2] == 'a' &&
           buf[3] == 'r' && buf[4] == 'g' && buf[5] == 'i' &&
           buf[6] == 'n' && buf[7] == 'g';
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

static void out_msg(const char *m)
{
    unsigned n = 0;
    while (m[n])
        n++;
    sc(SYS_write, 1, (s64)m, n, 0);
}

/* write then read back; one retry, returns 1 only if the node reports it */
static int wr_verified(const char *path, long v)
{
    long back;
    wr(path, v);
    back = rd(path, -10000);
    if (back != v) {
        wr(path, v);
        back = rd(path, -10000);
    }
    return back == v;
}

static long clamp_temp(long v)
{
    if (v < TEMP_MIN)
        return TEMP_MIN;
    if (v > TEMP_MAX)
        return TEMP_MAX;
    return v;
}

static int base_ok(long b)
{
    return b >= BASE_MIN && b <= BASE_MAX;
}

static void restore(int mode)
{
    wr(BOARD_NODE, 0);       /* 0 = drop the override, driver reports real */
    if (mode == MODE_BOTH)
        wr(FIXED_NODE, 0);
}

static int process_mode = MODE_BOTH;

static void __attribute__((naked)) sig_restorer(void)
{
    asm volatile("mov x8, %0\nsvc #0" :: "i"(SYS_rt_sigreturn));
}

struct ksigaction { u64 handler, flags, restorer, mask; };

static void setsig(int sig, void *handler)
{
    struct ksigaction action = {
        (u64)handler, SA_RESTORER, (u64)sig_restorer, 0
    };
    sc(SYS_rt_sigaction, sig, (s64)&action, 0, 8);
}

static void exit_restore(long sig)
{
    (void)sig;
    restore(process_mode);
    wr(CMS_ENABLE, 1);
    log_msg("tpoff: exit, restored CMS and temperature overrides\n");
    sc(SYS_exit, 0, 0, 0, 0);
    __builtin_unreachable();
}

void __attribute__((noreturn)) run(long argc, char **argv)
{
    long off = -50;
    int faking = 0;
    int mode = MODE_BOTH;
    long fixed_scale = 1;
    int turbo_active;
    int turbo_warned = 0;
    int clamp_logged = 0;
    int verify_warned = 0;

    if (argc > 1) {
        off = parse_long(argv[1]);
        if (off > 150)
            off = 150;
        else if (off < -150)
            off = -150;
    }

    if (argc > 2) {
        if (argv[2][0] == 'b' && argv[2][1] == 'o' &&
            argv[2][2] == 't')
            mode = MODE_BOTH;
        else if (argv[2][0] == 'b' && argv[2][1] == 'o' &&
                 argv[2][2] == 'a')
            mode = MODE_BOARD;
    }
    process_mode = mode;
    setsig(15, exit_restore);
    setsig(2, exit_restore);

    if (argc > 3) {
        fixed_scale = parse_long(argv[3]);
        if (fixed_scale < 1)
            fixed_scale = 1;
    }
    if (off == 0)
        exit_restore(0);

    log_msg(mode == MODE_BOARD ? "tpoff: mode=board-only\n"
                               : "tpoff: mode=both\n");

    /* Preserve an existing CMS state so a restarted process does not
     * accidentally cancel Turbo mode owned by an earlier instance/tool. */
    turbo_active = rd(CMS_ENABLE, 1) == 0;
    log_msg(turbo_active ? "tpoff: turbo already enabled\n"
                         : "tpoff: turbo inactive at startup\n");

    /* clear temperature leftovers from a previous instance / other tools */
    if (mode == MODE_BOTH)
        wr(FIXED_NODE, 0);
    wr(BOARD_NODE, 0);

    for (;;) {
        int plugged = charging();
        long base, gb, capacity;

        gb = rd(GAME_BYPASS_FLAG, 0);

        base = rd(MAIN_NODE, -10000);
        if (base <= -10000) {
            base = rd(BTB_NODE, -10000);
            if (base > -10000)
                base -= BTB_ADJUST;
        }
        if (base <= -10000) {
            log_msg("tpoff: no battery temperature source, exiting\n");
            exit_restore(0);
        }

        capacity = rd(BATT_CAPACITY, -1);
        {
            int turbo = plugged && gb < 1 && base_ok(base) &&
                        base < 410 && capacity >= 0 && capacity < 30;
            if (turbo && !turbo_active) {
                if (wr_verified(CMS_ENABLE, 0)) {
                    turbo_active = 1;
                    turbo_warned = 0;
                    log_msg("tpoff: turbo enabled\n");
                } else if (!turbo_warned) {
                    turbo_warned = 1;
                    log_msg("tpoff: turbo enable verify failed\n");
                }
            } else if (!turbo && turbo_active) {
                if (wr_verified(CMS_ENABLE, 1)) {
                    turbo_active = 0;
                    turbo_warned = 0;
                    log_msg("tpoff: turbo disabled\n");
                } else if (!turbo_warned) {
                    turbo_warned = 1;
                    log_msg("tpoff: turbo restore verify failed\n");
                }
            }
        }

        if (plugged && base_ok(base) && gb < 1) {
            long raw = base + off;
            long fake = clamp_temp(raw);

            if (fake != raw && !clamp_logged) {
                clamp_logged = 1;
                log_msg(fake == TEMP_MIN
                        ? "tpoff: clamped to 15.0C floor\n"
                        : "tpoff: clamped to 55.0C ceiling\n");
            }
            if (!faking) {
                faking = 1;
                verify_warned = 0;
                log_msg("tpoff: charging, faking\n");
            }
            if (!wr_verified(BOARD_NODE, fake) && !verify_warned) {
                verify_warned = 1;
                log_msg("tpoff: board write verify failed\n");
            }
            if (mode == MODE_BOTH) {
                long fixed = (fake + fixed_scale / 2) / fixed_scale;
                if (!wr_verified(FIXED_NODE, fixed) && !verify_warned) {
                    verify_warned = 1;
                    log_msg("tpoff: fixed_temp write verify failed\n");
                }
            }
        } else if (faking) {
            faking = 0;
            restore(mode);
            log_msg(gb >= 1 ? "tpoff: game bypass active, restored\n"
                  : (plugged ? "tpoff: base temp out of range, restored\n"
                              : "tpoff: offline, restored\n"));
        }
        {
            long ts[2] = { turbo_active ? 1 : 3, 0 };
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

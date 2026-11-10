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
 * removed.  Below 30% capacity, an additional 5.0C offset is applied.
 *
 * Low-temperature guard: below 25.0C battery temperature no offset is
 * applied at all (the system's own low-temp protection would otherwise
 * throttle harder than it helps). A faked value is never written below
 * 30.0C — any lower and the fake sits inside the fast-charge admission
 * low-temp window / battery-heating ladder (fchg gates + tbat-heating
 * rungs at 20C/32.5C), which can block or delay fast-charge activation.
 * The floor only truncates the offset: if it would push the fake at or
 * above the real temperature (real temp already under the floor), no
 * override is applied at all — a fake warmer than reality would only
 * cost charging current.
 *
 * Safety: every value written is clamped to [30.0C, 55.0C] and read back
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
#define BATT_STATUS "/sys/class/power_supply/battery/status"
#define BATT_CAPACITY "/sys/class/power_supply/battery/capacity"
#define LOG_PATH   "/data/local/tmp/tpoff.log"

#define SA_RESTORER 0x04000000

/* hard bounds for any temperature we write: never below 30.0C (below that
 * the fake risks sitting inside the fast-charge admission low-temp window
 * and the battery-heating ladder, blocking fast charge), never above 55.0C
 * (0.1C units). */
#define COLD_OFF    250      /* below 25.0C real battery temp: no offset */
#define FAKE_MIN    300      /* never fake below 30.0C */
#define TEMP_MIN    150
#define TEMP_MAX    550
#define BASE_MIN    (-400)   /* -40.0C, treat anything lower as a read error */
#define BASE_MAX    1250     /* 125.0C */
#define BTB_ADJUST   60       /* bat_btb_temp is about 6.0C above main */
#define LOW_SOC_OFFSET (-50)  /* additional -5.0C below 30% */
#define LOW_SOC_MIN_OFFSET (-150) /* -15.0C minimum when offset is above it */

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

/* fixed_temp asymmetry (iQOO15): store() takes 0.1C units but show()
 * reports whole 1C, so a correct write reads back as v/10; accept both. */
static int wr_verified_fixed(long v)
{
    long back;
    wr(FIXED_NODE, v);
    back = rd(FIXED_NODE, -10000);
    if (back != v && back * 10 != v) {
        wr(FIXED_NODE, v);
        back = rd(FIXED_NODE, -10000);
    }
    return back == v || back * 10 == v;
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
    log_msg("tpoff: exit, restored temperature overrides\n");
    sc(SYS_exit, 0, 0, 0, 0);
    __builtin_unreachable();
}

void __attribute__((noreturn)) run(long argc, char **argv)
{
    long off = -50;
    int faking = 0;
    int mode = MODE_BOTH;
    long fixed_scale = 1;
    int clamp_logged = 0;
    int floor_skip_logged = 0;
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

        if (plugged && base_ok(base) && base >= COLD_OFF && gb < 1) {
            long effective_off = off;
            if (capacity >= 0 && capacity < 30) {
                effective_off += LOW_SOC_OFFSET;
                if (off > LOW_SOC_MIN_OFFSET &&
                    effective_off < LOW_SOC_MIN_OFFSET)
                    effective_off = LOW_SOC_MIN_OFFSET;
            }
            long raw = base + effective_off;
            long fake = raw < FAKE_MIN ? FAKE_MIN : raw;
            fake = clamp_temp(fake);

            if (fake >= base) {
                /* 30.0C floor sits at/above the real temperature: applying
                 * it would only fake upward and cost charging current. */
                if (faking) {
                    faking = 0;
                    restore(mode);
                    floor_skip_logged = 0;
                    log_msg("tpoff: floor at/above real temp, restored\n");
                }
                if (!floor_skip_logged) {
                    floor_skip_logged = 1;
                    log_msg("tpoff: offset below 30.0C floor, not faking\n");
                }
            } else {
                if (fake != raw && !clamp_logged) {
                    clamp_logged = 1;
                    log_msg(fake == TEMP_MAX
                            ? "tpoff: clamped to 55.0C ceiling\n"
                            : "tpoff: clamped to 30.0C floor\n");
                }
                if (!faking) {
                    faking = 1;
                    floor_skip_logged = 0;
                    verify_warned = 0;
                    log_msg("tpoff: charging, faking\n");
                }
                if (!wr_verified(BOARD_NODE, fake) && !verify_warned) {
                    verify_warned = 1;
                    log_msg("tpoff: board write verify failed\n");
                }
                if (mode == MODE_BOTH) {
                    long fixed = (fake + fixed_scale / 2) / fixed_scale;
                    if (!wr_verified_fixed(fixed) && !verify_warned) {
                        verify_warned = 1;
                        log_msg("tpoff: fixed_temp write verify failed\n");
                    }
                }
            }
        } else if (faking) {
            faking = 0;
            restore(mode);
            log_msg(gb >= 1 ? "tpoff: game bypass active, restored\n"
                  : (!plugged ? "tpoff: offline, restored\n"
                  : (base_ok(base) && base < COLD_OFF
                     ? "tpoff: battery below 25.0C, restored\n"
                     : "tpoff: base temp out of range, restored\n")));
        }
        {
            long ts[2] = { 3, 0 };
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

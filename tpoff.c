/* tpoff (raw-syscall, no libc) - unified CMS temperature offset for vivo.
 *
 * Two device families behave differently, detected at startup:
 *
 *   probe: write fixed_temp=299, read main_batt_temp back
 *     == 299  -> fixed_temp IS the battery temperature (X300 / MTK):
 *                writing it changes battery/temp and thermal zones, so we
 *                only override primary_board_temp.  Base = bat_btb_temp.
 *     != 299  -> fixed_temp is an independent knob (iQOO15 / QCOM):
 *                board and battery temperature must move together or the
 *                driver flags a "temp gap" anomaly and slows charging down.
 *                Write both, base = main_batt_temp.
 *
 * Usage:
 *   tpoff [offset]     offset in 0.1C, default -50, clamped to [-150,150];
 *                      0 exits immediately
 *   tpoff probe        print the detected mode and exit (no lasting writes)
 *
 * Active only while charging (battery current < -50mA, or usb/wls online,
 * debounced 4 samples for the brief dips the charger produces) and while
 * game_bypass_charge_flag < 1.  On unplug / read failure / SIGTERM/SIGINT
 * the override is removed (board written 0 in board-only mode, real board
 * temp restored and fixed_temp cleared in both-mode).
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
#define REAL_NODE  "/sys/class/power_supply/battery/primary_board_temp"
#define GAME_BYPASS_FLAG "/sys/class/cms_class/game_bypass_charge_flag"
#define USB_ONLINE "/sys/class/power_supply/usb/online"
#define WLS_ONLINE "/sys/class/power_supply/wls/online"
#define BATT_CURRENT "/sys/class/power_supply/battery/current_now"
#define LOG_PATH   "/data/local/tmp/tpoff.log"

#define SA_RESTORER 0x04000000
#define OFF_GRACE   4

/* hard bounds for any temperature we write: never below 15.0C, never above
 * 55.0C (0.1C units).  Below 15C the CMS tables are already saturated and a
 * bad sensor read would otherwise produce a nonsense override. */
#define TEMP_MIN    150
#define TEMP_MAX    550
#define BASE_MIN    (-400)   /* -40.0C, treat anything lower as a read error */
#define BASE_MAX    1250     /* 125.0C */

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

/* 1 = fixed_temp shadows the battery temperature (board-only device) */
static int detect_mode(void)
{
    long v;
    wr(FIXED_NODE, 299);
    v = rd(MAIN_NODE, -10000);
    wr(FIXED_NODE, 0);
    return v == 299 ? MODE_BOARD : MODE_BOTH;
}

static void restore(int mode)
{
    if (mode == MODE_BOTH) {
        long real = rd(REAL_NODE, -10000);
        wr(BOARD_NODE, real > -10000 ? real : 0);
        wr(FIXED_NODE, 0);
    } else {
        wr(BOARD_NODE, 0);   /* 0 = drop the override, driver reports real */
    }
}

void __attribute__((noreturn)) run(long argc, char **argv)
{
    long off = -50;
    int faking = 0;
    int off_cnt = OFF_GRACE;
    int mode;
    int probe_only = 0;
    int clamp_logged = 0;
    int verify_warned = 0;

    if (argc > 1 && (argv[1][0] == 'p' || argv[1][0] == 'P')) {
        probe_only = 1;
    } else if (argc > 1) {
        off = parse_long(argv[1]);
        if (off == 0)
            sc(SYS_exit, 0, 0, 0, 0);
        if (off > 150)
            off = 150;
        else if (off < -150)
            off = -150;
    }

    mode = detect_mode();

    if (probe_only) {
        out_msg(mode == MODE_BOARD
                ? "tpoff: fixed_temp shadows main_batt_temp -> board-only (X300)\n"
                : "tpoff: fixed_temp independent -> both (iQOO15)\n");
        sc(SYS_exit, 0, 0, 0, 0);
    }

    log_msg(mode == MODE_BOARD
            ? "tpoff: mode=board-only (fixed_temp is battery temp)\n"
            : "tpoff: mode=both (fixed_temp independent)\n");

    /* clear leftovers from a previous instance / other tools */
    if (mode == MODE_BOARD)
        wr(FIXED_NODE, 0);
    wr(BOARD_NODE, 0);

    for (;;) {
        /* use the battery current sign as the primary "charging" signal:
         * with the vivo charge-pump path usb/online stays 0 while charging */
        long ibat = rd(BATT_CURRENT, 0);
        int charging = ibat < -50000 ||
                       rd(USB_ONLINE, 0) == 1 || rd(WLS_ONLINE, 0) == 1;
        long base, gb;
        int plugged;

        if (charging)
            off_cnt = 0;
        else if (off_cnt < OFF_GRACE)
            off_cnt++;
        plugged = off_cnt < OFF_GRACE;

        gb = rd(GAME_BYPASS_FLAG, 0);

        if (mode == MODE_BOARD) {
            base = rd(BTB_NODE, -10000);
        } else {
            base = rd(MAIN_NODE, -10000);
            if (base <= -10000)
                base = rd(BTB_NODE, -10000);
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
            if (mode == MODE_BOTH && !wr_verified(FIXED_NODE, fake) &&
                !verify_warned) {
                verify_warned = 1;
                log_msg("tpoff: fixed_temp write verify failed\n");
            }
        } else if (faking) {
            faking = 0;
            restore(mode);
            log_msg(gb >= 1 ? "tpoff: game bypass active, restored\n"
                  : (plugged ? "tpoff: base temp out of range, restored\n"
                             : "tpoff: unplug or read fail, restored\n"));
        }
        {
            long ts[2] = { 2, 0 };
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

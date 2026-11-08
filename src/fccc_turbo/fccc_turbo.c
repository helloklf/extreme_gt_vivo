/* fccc_turbo: same temperature/voltage envelope algorithm as fccc, but the
 * backend is the Qualcomm votable force channel
 * (/sys/kernel/debug/pmic-votable/DCHG_FCC) instead of vfcs_class/fchg_fcc_ma,
 * and the whole control is POWER-based:
 *
 *  - base_ma = PWR_W * 1e9 / vbat_uV, recomputed every poll and clamped to
 *    MAX_FCC mA: the configured power ceiling expressed as battery-side
 *    current at the instantaneous voltage. Every limit below derives from
 *    this base; there is no other hardcoded current reference.
 *  - temperature curve scales base_ma down (x0.94 per 0.1C above 46.1C,
 *    extra x0.85 per 0.1C above 47.0C); below base/3 the force is released.
 *  - voltage ratchet anchors to min(base_ma, peak observed current) and
 *    decays geometrically, recovering +10 mA/poll in the safe band; floor
 *    base/4.
 *  - force pins an ABSOLUTE current (cap and floor at once): while charging
 *    the target is always forced (constant-power charging with a safety
 *    envelope), never "capped only". Above CAP_REL percent the force is
 *    released to the OEM taper (100 = never release). PWR_W=0 disables
 *    forcing entirely.
 *  - charger (usb) voltage/current are logged for reference; battery-side
 *    voltage is what sets battery-side power, so it drives the conversion.
 *  - startup writes 0 to cms_class/htccc_enable (OEM high-temperature charge
 *    control off, so it cannot fight the force); restored to 1 on exit.
 *
 * Standalone test tool: build only, adb push, run by hand. Not integrated
 * into the ExtremeGT module.
 *
 * Usage: fccc_turbo [both|-] [pwr_w] [cap_rel]
 *   both    tpoff mode=both: fixed_temp does not shadow battery temperature
 *   pwr_w   power ceiling in watts (default 60, 0 = disabled, max 120)
 *   cap_rel release to the OEM taper above this SOC (default 70, 100 = never)
 */
typedef long s64;
typedef unsigned long u64;

#define SYS_flock        32
#define SYS_mount        40
#define SYS_openat       56
#define SYS_close        57
#define SYS_read         63
#define SYS_write        64
#define SYS_exit         93
#define SYS_nanosleep    101
#define SYS_rt_sigaction 134
#define SYS_rt_sigreturn 139

#define FIXED_NODE  "/sys/class/cms_class/fixed_temp"
#define TEMP_NODE   "/sys/class/cms_class/main_batt_temp"
#define HTCCC_NODE  "/sys/class/cms_class/htccc_enable"
#define VOLT_NODE   "/sys/class/power_supply/battery/voltage_now"
#define CAP_NODE    "/sys/class/power_supply/battery/capacity"
#define CUR_NODE    "/sys/class/power_supply/battery/current_now"
#define STATUS_NODE "/sys/class/power_supply/battery/status"
#define UVOLT_NODE  "/sys/class/power_supply/usb/voltage_now"
#define UCUR_NODE   "/sys/class/power_supply/usb/current_now"
#define FVAL_NODE   "/sys/kernel/debug/pmic-votable/DCHG_FCC/force_val"
#define FACT_NODE   "/sys/kernel/debug/pmic-votable/DCHG_FCC/force_active"
#define DEBUGFS_DIR "/sys/kernel/debug"
#define LOCK_PATH   "/data/local/tmp/fccc_turbo.lock"

#define TEMP_L1      461
#define TEMP_L2      470
#define TEMP_L3      480
#define TEMP_EXIT    451
#define VOLT_CRIT   4510000
#define VOLT_TREND  4500000
#define VOLT_HIGH   4460000
#define VOLT_LOW    4410000
#define IBAT_MIN_MA    100
#define TEMP_FLOOR_DIV    3  /* temp curve below base/3 -> release (fccc: 5000/15000) */
#define VOLT_FLOOR_DIV    4  /* voltage ratchet floor base/4 (fccc: 4000/15600) */
#define DEFAULT_PWR_W    60
#define MAX_PWR_W       120
#define DEFAULT_CAP_REL  70  /* release to the OEM taper above this SOC; 100 = never */
#define MAX_FCC       17600  /* absolute battery-side current ceiling (mA) */
#define SA_RESTORER 0x04000000

#ifdef HOSTTEST
extern long sim_sc(long n, long a, long b, long c, long d);
#define sc(n, a, b, c, d) sim_sc((long)(n), (long)(a), (long)(b), (long)(c), (long)(d))
#define sc5(n, a, b, c, d, e) sim_sc((long)(n), (long)(a), (long)(b), (long)(c), (long)(d))
#else
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
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3),
                 "r"(x4) : "memory");
    return x0;
}
#endif

static int length(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void log_msg(const char *message)
{
    sc(SYS_write, 2, (s64)message, length(message), 0);
}

static char *put_number(char *p, long value)
{
    char tmp[24];
    int n = 0;
    if (value < 0) { *p++ = '-'; value = -value; }
    do { tmp[n++] = '0' + value % 10; value /= 10; } while (value);
    while (n) *p++ = tmp[--n];
    return p;
}

static char *put_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

static void log_limit(long temp, long voltage, long usb_uv, long capacity,
                      long ibat_ma, long peak_ma, long base_ma, long temp_ma,
                      long volt_ma, long current, long target, int write_needed)
{
    char buf[384], *p = buf;
    p = put_str(p, "fccc_turbo: temp_0.1C=");
    p = put_number(p, temp);
    p = put_str(p, " voltage_uV=");
    p = put_number(p, voltage);
    p = put_str(p, " usb_uV=");
    p = put_number(p, usb_uv);
    p = put_str(p, " capacity_pct=");
    p = put_number(p, capacity);
    p = put_str(p, " ibat_mA=");
    p = put_number(p, ibat_ma);
    p = put_str(p, " peak_mA=");
    p = put_number(p, peak_ma);
    p = put_str(p, " base_mA=");
    p = put_number(p, base_ma);
    p = put_str(p, " temp_mA=");
    p = put_number(p, temp_ma);
    p = put_str(p, " volt_mA=");
    p = put_number(p, volt_ma);
    p = put_str(p, " forced_mA=");
    p = put_number(p, current);
    p = put_str(p, " target_mA=");
    p = put_number(p, target);
    p = put_str(p, " write=");
    p = put_number(p, write_needed);
    *p++ = '\n';
    *p = 0;
    log_msg(buf);
}

/* Report why a node read failed: the stage (open/read/parse), the failing
 * syscall's errno, or the raw content when it does not parse as a number. */
static void log_read_error(const char *stage, const char *path, long error,
                           const char *raw)
{
    char buf[256], *p = buf;
    p = put_str(p, "fccc_turbo: ");
    p = put_str(p, stage);
    p = put_str(p, " failed for ");
    p = put_str(p, path);
    if (raw) {
        p = put_str(p, "; content=\"");
        while (*raw && p < buf + sizeof(buf) - 32) {
            *p++ = (*raw == '\n' || *raw == '\r') ? ' ' : *raw;
            raw++;
        }
        *p++ = '"';
    } else {
        p = put_str(p, "; errno=");
        p = put_number(p, -error);
    }
    *p++ = '\n';
    *p = 0;
    log_msg(buf);
}

/* Report a value that parsed fine but failed the caller's range check. */
static void log_bad_value(const char *what, long value)
{
    char buf[192], *p = buf;
    p = put_str(p, "fccc_turbo: ");
    p = put_str(p, what);
    p = put_str(p, " out of range: ");
    p = put_number(p, value);
    p = put_str(p, "; stopping, retaining limit\n");
    *p = 0;
    log_msg(buf);
}

static int read_text(const char *path, char *buf, int size)
{
    long fd = sc(SYS_openat, -100, (s64)path, 0, 0);
    long n;
    if (fd < 0) { log_read_error("open", path, fd, 0); return -1; }
    n = sc(SYS_read, fd, (s64)buf, size - 1, 0);
    sc(SYS_close, fd, 0, 0, 0);
    if (n <= 0) { log_read_error("read", path, n, 0); return -1; }
    if (n >= size - 1) { log_read_error("read (too long)", path, n, 0); return -1; }
    buf[n] = 0;
    return 0;
}

static int read_number(const char *path, long *value)
{
    char buf[64], *p = buf;
    long v = 0;
    int negative = 0, digits = 0;
    if (read_text(path, buf, sizeof(buf))) return -1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') { negative = 1; p++; }
    while (*p >= '0' && *p <= '9') {
        if (v > 1000000000L) { log_read_error("parse", path, 0, buf); return -1; }
        v = v * 10 + *p++ - '0';
        digits++;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!digits || *p) { log_read_error("parse", path, 0, buf); return -1; }
    *value = negative ? -v : v;
    return 0;
}

/* Quiet optional read (usb charger nodes may be absent on some devices). */
static int read_opt(const char *path, long *value)
{
    char buf[64], *p = buf;
    long v = 0, fd, n;
    int negative = 0, digits = 0;
    fd = sc(SYS_openat, -100, (s64)path, 0, 0);
    if (fd < 0) return -1;
    n = sc(SYS_read, fd, (s64)buf, sizeof buf - 1, 0);
    sc(SYS_close, fd, 0, 0, 0);
    if (n <= 0) return -1;
    buf[n] = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') { negative = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + *p++ - '0'; digits++; }
    if (!digits) return -1;
    *value = negative ? -v : v;
    return 0;
}

/* tpoff mode=both means fixed_temp does not shadow the battery temperature
 * (e.g. iQOO15), so a nonzero fixed_temp is then not a reason to stop. */
static int fixed_independent;

static int fixed_overridden(long *fixed)
{
    if (fixed_independent) return 0;
    return read_number(FIXED_NODE, fixed) || *fixed != 0;
}

static int write_number(const char *path, long v)
{
    char buf[24], reverse[24];
    int i = 0, n = 0;
    long fd, written, back;
    do { reverse[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n) buf[i++] = reverse[--n];
    fd = sc(SYS_openat, -100, (s64)path, 1, 0);
    if (fd < 0) return -1;
    written = sc(SYS_write, fd, (s64)buf, i, 0);
    sc(SYS_close, fd, 0, 0, 0);
    if (written != i || read_number(path, &back)) return -1;
    v = 0;
    for (n = 0; n < i; n++) v = v * 10 + buf[n] - '0';
    return back == v ? 0 : -1;
}

static int same_status(const char *buf, const char *expected)
{
    while (*expected && *buf == *expected) { buf++; expected++; }
    return !*expected && (*buf == '\n' || *buf == '\r' || !*buf);
}

static int charging(void)
{
    char buf[64];
    if (read_text(STATUS_NODE, buf, sizeof(buf))) return -1;
    if (same_status(buf, "Charging")) return 1;
    if (same_status(buf, "Discharging") || same_status(buf, "Not charging") ||
        same_status(buf, "Full")) return 0;
    return -1;
}

/* Fixed point reproduces the two separately truncated stages of the reference. */
static long scaled(long current, int ticks, int percent)
{
    long factor = 1000000000000L;
    while (ticks-- > 0) factor = factor * percent / 100;
    return current * factor / 1000000000000L;
}

/* Temperature envelope as a factor of the power-derived base current:
 * x0.94 per 0.1C above TEMP_L1 (46.1C), an extra x0.85 per 0.1C above
 * TEMP_L2 (47.0C), clamped at TEMP_L3 (48.0C), with release hysteresis at
 * TEMP_EXIT (45.1C). Below base/TEMP_FLOOR_DIV the force is released
 * entirely. Thresholds are fccc's +3C: the turbo profile trades battery
 * temperature headroom for sustained power.
 *
 * Cooling-trend exemption: inside the first band [L1, L2) a sample lower
 * than the previous one means the temperature is already recovering, so
 * the reduction is skipped entirely -- the curve would otherwise keep
 * pushing the current down after the heat source is gone (overshoot). */
static long current_for_temp(long temp, long prev_temp, long base,
                             int *limited)
{
    long current = base, ct;
    if (temp <= TEMP_L1 && (!*limited || temp <= TEMP_EXIT)) {
        *limited = 0;
        return current;
    }
    if (temp < TEMP_L2 && prev_temp >= 0 && temp < prev_temp) {
        *limited = 0;
        return current;
    }
    *limited = 1;
    ct = temp > TEMP_L3 ? TEMP_L3 : temp;
    current = scaled(current, ct > TEMP_L1 ? (int)(ct - TEMP_L1) : 0, 94);
    if (ct > TEMP_L2) current = scaled(current, (int)(ct - TEMP_L2), 85);
    return current < base / TEMP_FLOOR_DIV ? 0 : current;
}

/* Ratchet anchored to the power-derived base and the peak observed charge
 * current: on entry the cap is snapped down to the demonstrated capability,
 * then decays geometrically -- each poll multiplies the previous cap by 0.99
 * at or above VOLT_CRIT, by 0.995 above VOLT_HIGH, so the step shrinks
 * together with the cap instead of growing relative to the falling charge
 * current. Below VOLT_TREND the decay only applies while the voltage is
 * still rising. In the safe band between VOLT_LOW and VOLT_HIGH the cap
 * instead recovers by a fixed 10 mA per poll, slow enough not to reintroduce
 * oscillation, so an overshoot does not stick forever; below VOLT_LOW it
 * resets outright to the base. Floor: base/VOLT_FLOOR_DIV. */
static long cap_for_voltage(long voltage, long prev_voltage, long peak_ma,
                            long cap, long base, int *limited)
{
    if (voltage < VOLT_LOW) {
        *limited = 0;
        return base;
    }
    if (voltage <= VOLT_HIGH) {
        cap += 10;
        if (cap >= base) {
            *limited = 0;
            return base;
        }
        return cap;
    }
    if (peak_ma < IBAT_MIN_MA) return cap;
    if (voltage < VOLT_TREND && prev_voltage >= 0 && voltage <= prev_voltage)
        return cap;
    *limited = 1;
    if (cap > peak_ma) cap = peak_ma;
    cap = voltage >= VOLT_CRIT ? cap * 99 / 100 : cap * 995 / 1000;
    if (cap < base / VOLT_FLOOR_DIV) cap = base / VOLT_FLOOR_DIV;
    return cap;
}

/* ---------------- votable force backend ---------------- */

static int force_set(long v)
{
    if (write_number(FVAL_NODE, v)) return -1;
    if (write_number(FACT_NODE, 1)) return -1;
    return 0;
}

static int force_clear(void)
{
    if (write_number(FACT_NODE, 0)) return -1;
    if (write_number(FVAL_NODE, 0)) return -1;
    return 0;
}

/* Effective forced current: 0 when the force channel is not active. */
static int read_limit(long *v)
{
    long active, val;
    if (read_number(FACT_NODE, &active)) return -1;
    if (active != 1) { *v = 0; return 0; }
    if (read_number(FVAL_NODE, &val)) return -1;
    *v = val;
    return 0;
}

/* Never change a value that another writer has changed. */
static int release_limit(long *last)
{
    long current, fixed;
    if (*last < 0) return 0;
    if (fixed_overridden(&fixed)) {
        log_msg("fccc_turbo: unsafe or unknown state; retaining limit\n");
        return -1;
    }
    if (read_limit(&current)) {
        log_msg("fccc_turbo: cannot read limit for release\n");
        return -1;
    }
    if (current == 0) {
        *last = -1;
        return 0;
    }
    if (current != *last) {
        log_msg("fccc_turbo: limit changed externally; not restoring\n");
        return -1;
    }
    if (force_clear()) {
        log_msg("fccc_turbo: failed to release limit\n");
        return -1;
    }
    *last = -1;
    return 0;
}

/* Silent node probe (the debugfs dir exists even when not mounted, so
 * probing the node itself -- not the directory -- is the reliable check). */
static int node_openable(const char *path)
{
    long fd = sc(SYS_openat, -100, (s64)path, 0, 0);
    if (fd < 0) return 0;
    sc(SYS_close, fd, 0, 0, 0);
    return 1;
}

static int contains(const char *hay, const char *needle)
{
    long i, j;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j]; j++)
            if (hay[i + j] != needle[j]) break;
        if (!needle[j]) return 1;
    }
    return 0;
}

/* 1 = debugfs mounted at DEBUGFS_DIR, 0 = not mounted, -1 = cannot tell. */
static int debugfs_mounted(void)
{
    static char buf[16384];
    long fd, n, total = 0;
    fd = sc(SYS_openat, -100, (s64)"/proc/mounts", 0, 0);
    if (fd < 0) return -1;
    for (;;) {
        n = sc(SYS_read, fd, (s64)(buf + total),
               (long)sizeof buf - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        if (total >= (long)sizeof buf - 1) break;
    }
    sc(SYS_close, fd, 0, 0, 0);
    buf[total] = 0;
    /* /proc/mounts line: "debugfs /sys/kernel/debug debugfs rw,..." */
    return contains(buf, DEBUGFS_DIR " debugfs") ? 1 : 0;
}

/* debugfs is not mounted at /sys/kernel/debug by default on vivo (it lives
 * under a different mountpoint), so mount it ourselves when missing. */
static int ensure_votable(void)
{
    int m;
    if (node_openable(FACT_NODE)) return 0;
    m = debugfs_mounted();
    if (m == 1) {
        /* mounted but the votable node is absent: the driver does not
         * provide it; remounting would not help */
        log_msg("fccc_turbo: debugfs mounted but DCHG_FCC nodes missing\n");
        return -1;
    }
    if (m == 0)
        log_msg("fccc_turbo: debugfs not mounted; mounting\n");
    sc5(SYS_mount, (s64)"none", (s64)DEBUGFS_DIR, (s64)"debugfs", 0, 0);
    if (node_openable(FACT_NODE)) return 0;
    log_msg("fccc_turbo: votable force nodes unavailable\n");
    return -1;
}

static long g_pwr_w = DEFAULT_PWR_W;
static long g_cap_rel = DEFAULT_CAP_REL;
static int htccc_disabled;

static volatile int stop_requested;
static void on_signal(int sig) { (void)sig; stop_requested = 1; }

#ifdef HOSTTEST
static void sig_restorer(void) {}
void hosttest_reset(void)
{
    fixed_independent = 0;
    g_pwr_w = DEFAULT_PWR_W;
    g_cap_rel = DEFAULT_CAP_REL;
    htccc_disabled = 0;
    stop_requested = 0;
}
#else
static void __attribute__((naked)) sig_restorer(void)
{
    asm volatile("mov x8, %0\nsvc #0" :: "i"(SYS_rt_sigreturn));
}
#endif

struct ksigaction { u64 handler, flags, restorer, mask; };

static int setsig(int sig)
{
    struct ksigaction action = {
        (u64)on_signal, SA_RESTORER, (u64)sig_restorer, 0
    };
    return sc(SYS_rt_sigaction, sig, (s64)&action, 0, 8) < 0 ? -1 : 0;
}

static void sleep_second(void)
{
    long ts[2] = { 1, 0 };
    sc(SYS_nanosleep, (s64)ts, 0, 0, 0);
}

static int parse_number(const char *a, long *out)
{
    long v = 0;
    int digits = 0;
    while (*a >= '0' && *a <= '9') { v = v * 10 + *a++ - '0'; digits++; }
    if (!digits || *a) return -1;
    *out = v;
    return 0;
}

void __attribute__((noreturn)) run(long argc, char **argv)
{
    long last = -1;
    long last_target = -1;
    long dither = 0;
    long lock_fd;
    long volt_cap = 0;
    long ibat_peak = 0;
    long prev_voltage = -1;
    long prev_temp = -1;
    int temp_limited = 0, volt_limited = 0, error = 0, write_ticks = 0;
    int i, nnum = 0;
    if (argc < 1 || argc > 4 || setsig(2) || setsig(15)) {
        log_msg("fccc_turbo: unexpected arguments or signal setup failure\n");
        sc(SYS_exit, 1, 0, 0, 0);
        __builtin_unreachable();
    }
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        long v;
        if (a[0] == 'b' && a[1] == 'o' && a[2] == 't') {
            fixed_independent = 1;
            continue;
        }
        if (a[0] == '-') continue;
        if (parse_number(a, &v))
            goto usage;
        if (nnum == 0) {
            if (v > MAX_PWR_W) goto usage;
            g_pwr_w = v;
        } else if (nnum == 1) {
            if (v < 1 || v > 100) goto usage;
            g_cap_rel = v;
        } else {
            goto usage;
        }
        nnum++;
    }
    if (0) {
usage:
        log_msg("fccc_turbo: usage: fccc_turbo [both|-] [pwr_w] [cap_rel]\n");
        sc(SYS_exit, 1, 0, 0, 0);
        __builtin_unreachable();
    }
    lock_fd = sc(SYS_openat, -100, (s64)LOCK_PATH, 2 | 0100 | 0400000, 0600);
    if (lock_fd < 0 || sc(SYS_flock, lock_fd, 2 | 4, 0, 0) < 0) {
        log_msg("fccc_turbo: cannot acquire instance lock\n");
        sc(SYS_exit, 1, 0, 0, 0);
        __builtin_unreachable();
    }
    if (ensure_votable()) {
        sc(SYS_exit, 1, 0, 0, 0);
        __builtin_unreachable();
    }
    /* votable force is kernel state and survives userspace restarts:
     * clear any leftover from a previous crashed/killed run. */
    {
        long active;
        if (!read_number(FACT_NODE, &active) && active == 1) {
            log_msg("fccc_turbo: clearing stale force from previous run\n");
            force_clear();
        }
    }
    /* Take over thermal charge control: htccc_enable=0 stops the OEM's own
     * high-temperature charge-current control so it cannot fight the forced
     * current. Restored to 1 on exit (all paths). */
    if (write_number(HTCCC_NODE, 0)) {
        log_msg("fccc_turbo: htccc_enable unavailable; continuing without it\n");
    } else {
        htccc_disabled = 1;
        log_msg("fccc_turbo: htccc_enable -> 0 (OEM thermal charge control off)\n");
    }
    log_msg(fixed_independent
                ? "fccc_turbo: started (fixed_temp checks off)\n"
                : "fccc_turbo: started\n");
    {
        char buf[128], *p = buf;
        p = put_str(p, "fccc_turbo: power ceiling W=");
        p = put_number(p, g_pwr_w);
        p = put_str(p, " (0 = disabled), cap_release pct=");
        p = put_number(p, g_cap_rel);
        p = put_str(p, "\n");
        *p = 0;
        log_msg(buf);
    }
    while (!stop_requested) {
        long fixed, temp, voltage, usb_uv, usb_ua, capacity, ibat_ma;
        long base, temp_ma, volt_ma, current, target;
        int state = charging();
        if (fixed_overridden(&fixed)) {
            log_msg("fccc_turbo: fixed_temp unavailable or active; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (state < 0) {
            log_msg("fccc_turbo: charging status unavailable; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (!state) {
            if (release_limit(&last)) { error = 1; break; }
            temp_limited = 0;
            volt_limited = 0;
            volt_cap = 0;
            ibat_peak = 0;
            prev_voltage = -1;
            last_target = -1;
            write_ticks = 0;
            sleep_second();
            continue;
        }
        if (read_number(TEMP_NODE, &temp) || fixed_overridden(&fixed)) {
            log_msg("fccc_turbo: unreadable or overridden battery temperature; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (temp < -400 || temp > 1250) {
            log_bad_value("battery temperature", temp);
            error = 1;
            break;
        }
        if (read_number(VOLT_NODE, &voltage)) {
            log_msg("fccc_turbo: unavailable single-cell battery voltage; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (voltage < 2000000 || voltage > 5000000) {
            log_bad_value("single-cell battery voltage", voltage);
            error = 1;
            break;
        }
        if (read_limit(&current)) {
            log_msg("fccc_turbo: cannot read force state; stopping\n");
            error = 1;
            break;
        }
        if (read_number(CAP_NODE, &capacity)) {
            log_msg("fccc_turbo: unavailable battery capacity; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (capacity < 0 || capacity > 100) {
            log_bad_value("battery capacity", capacity);
            error = 1;
            break;
        }
        /* current_now is in uA and negative while charging on some devices. */
        if (read_number(CUR_NODE, &ibat_ma)) {
            log_msg("fccc_turbo: cannot read battery current; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (ibat_ma < 0) ibat_ma = -ibat_ma;
        ibat_ma /= 1000;
        if (ibat_ma > 100000) {
            log_bad_value("battery current", ibat_ma);
            error = 1;
            break;
        }
        if (read_opt(UVOLT_NODE, &usb_uv)) usb_uv = -1;
        if (read_opt(UCUR_NODE, &usb_ua)) usb_ua = -1;

        int write_needed;
        long write_val;
        if (ibat_ma > ibat_peak) ibat_peak = ibat_ma;
        /* Power-derived base: the configured ceiling as battery-side current
         * at the instantaneous voltage, clamped to the absolute MAX_FCC.
         * 0 = forcing disabled. */
        base = g_pwr_w > 0 ? g_pwr_w * 1000000000L / voltage : 0;
        if (base > MAX_FCC) base = MAX_FCC;
        /* The voltage ratchet never exceeds the (moving) base; re-arm it
         * after a release or a fresh charge session. */
        if (volt_cap > base || volt_cap <= 0) volt_cap = base;
        temp_ma = current_for_temp(temp, base, &temp_limited);
        volt_cap = cap_for_voltage(voltage, prev_voltage, ibat_peak, volt_cap,
                                   base, &volt_limited);
        volt_ma = volt_cap;
        prev_voltage = voltage;
        target = temp_ma < volt_ma ? temp_ma : volt_ma;
        /* Above the capacity threshold hand control back to the OEM taper. */
        if (capacity > g_cap_rel) target = 0;
        if (target <= 0) {
            log_limit(temp, voltage, usb_uv, capacity, ibat_ma, ibat_peak,
                      base, temp_ma, volt_ma, current, 0, last >= 0);
            if (release_limit(&last)) { error = 1; break; }
            last_target = 0;
            write_ticks = 0;
            sleep_second();
            continue;
        }
        /* A write issued before fast charging activates may not take effect,
         * so rewrite the target every 5 seconds even when it is unchanged. */
        write_needed = target != last_target || ++write_ticks >= 5;
        /* A write equal to the value already in the node may be dropped by the
         * driver, so nudge nonzero repeats by a cycling 1-50 mA offset. */
        write_val = target;
        if (write_needed && target > 0 && target == current) {
            dither = dither % 50 + 1;
            write_val = target + dither;
        }
        log_limit(temp, voltage, usb_uv, capacity, ibat_ma, ibat_peak,
                  base, temp_ma, volt_ma, current, target, write_needed);
        if (write_needed) {
            write_ticks = 0;
            if (fixed_overridden(&fixed)) {
                log_msg("fccc_turbo: fixed_temp changed; stopping, retaining limit\n");
                error = 1;
                break;
            }
            if (force_set(write_val)) {
                log_msg("fccc_turbo: force write or readback failed; stopping\n");
                error = 1;
                break;
            }
            last = write_val;
            last_target = target;
        }
        sleep_second();
    }
    if (!error && release_limit(&last)) error = 1;
    /* Restore the OEM thermal charge control on every exit path: on error
     * the forced limit is retained against current spikes, but htccc must
     * not be left disabled with nobody watching the temperature. */
    if (htccc_disabled && write_number(HTCCC_NODE, 1)) {
        log_msg("fccc_turbo: failed to restore htccc_enable\n");
        error = 1;
    }
    log_msg(error ? "fccc_turbo: stopped with error\n" : "fccc_turbo: stopped\n");
    sc(SYS_exit, error, 0, 0, 0);
    __builtin_unreachable();
}

#ifndef HOSTTEST
void __attribute__((naked)) _start(void)
{
    asm volatile("ldr x0, [sp]\nadd x1, sp, #8\nb run");
}
#endif

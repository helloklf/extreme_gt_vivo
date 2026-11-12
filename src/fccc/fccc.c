/* Additional CMS current limit based on temperature (0.1 C), voltage (uV)
 * and charge current (uA). */
typedef long s64;
typedef unsigned long u64;

#define SYS_flock        32
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
#define TEMP_NODE_ALT "/sys/class/power_supply/battery/temp"
#define ADPT_TEMP_NODE "/sys/class/stm32_debug/adapter_temp"
#define VOLT_NODE   "/sys/class/power_supply/battery/voltage_now"
#define CELL1_NODE  "/sys/class/cms_class/cell1_voltage"
#define CELL2_NODE  "/sys/class/cms_class/cell2_voltage"
#define CAP_NODE    "/sys/class/power_supply/battery/capacity"
#define CUR_NODE    "/sys/class/power_supply/battery/current_now"
#define LIMIT_NODE  "/sys/class/vfcs_class/fchg_fcc_ma"
#define STATUS_NODE "/sys/class/power_supply/battery/status"
#define HTCCC_NODE  "/sys/class/cms_class/htccc_enable"
#define LOCK_PATH   "/data/local/tmp/fccc.lock"

#define BASE_MA    15600
#define RELEASE_MA 0
#define TEMP_FLOOR_MA 2000  /* temp ratchet floor; hold, never release while hot */
#define TEMP_L1      425
#define TEMP_L2      435
#define TEMP_L3      445
#define TEMP_L4      450
#define TEMP_EXIT    420
/* Slope-gated ratchet tuning: the trend is the temperature change over a
 * whole window of polls (0.1C units) -- long enough to average out sensor
 * quantization, short enough to lead the thermal peak. */
#define TEMP_SLOPE_WIN   15
#define TEMP_SLOPE_DB     1   /* |slope| below this is noise: hold */
#define TEMP_SLOPE_CLAMP 30   /* bound one-poll steps (adapter reports) */
/* While htccc_enable reads 0 (someone else disabled the OEM 45C wall),
 * all temperature thresholds shift up by this much (0.1C units, = 3.0C).
 * fccc only reads that node, it never writes it. */
#define TEMP_SHIFT_HTCCC_OFF 30
/* Adapter thermal ceiling reported by the charger itself (0.1C): the vfcp
 * stack drops fast charge when adapter_temp reaches 850 (see docs/
 * iQOO15充电中后程功率闪崩分析.md). Adapter temp is normalized onto the
 * battery-temp axis with this as the full-scale point, so a hot adapter
 * drives the same temp_ma envelope as a hot battery. */
#define ADPT_TEMP_MAX 850
#define VOLT_CRIT   4510000
#define VOLT_TREND  4500000
#define VOLT_HIGH   4450000
#define VOLT_LOW    4360000
#define IBAT_MIN_MA    100
#define CAP_RELEASE      70
/* Dual-cell packs split the current across two cells and tolerate fast
 * charging much longer; release to the OEM taper later there. */
#define CAP_RELEASE_DUAL   82
/* Hard hand-back point: at or above this capacity fccc always releases,
 * even if the battery is still hot. */
#define CAP_FORCE_EXIT     90
#define VOLT_MIN_MA    4000
#define SA_RESTORER 0x04000000

#ifdef HOSTTEST
extern long sim_sc(long n, long a, long b, long c, long d);
#define sc(n, a, b, c, d) sim_sc((long)(n), (long)(a), (long)(b), (long)(c), (long)(d))
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

static void log_limit(long temp, long adpt, long voltage, long capacity, long ibat_ma,
                      long peak_ma, long temp_ma, long volt_ma,
                      int temp_limited, int volt_limited, long current,
                      long target, int write_needed)
{
    char buf[320], *p = buf;
    const char *s = "fccc: temp_0.1C=";
    while (*s) *p++ = *s++;
    p = put_number(p, temp);
    s = " adpt_0.1C=";
    while (*s) *p++ = *s++;
    p = put_number(p, adpt);
    s = " voltage_uV=";
    while (*s) *p++ = *s++;
    p = put_number(p, voltage);
    s = " capacity_pct=";
    while (*s) *p++ = *s++;
    p = put_number(p, capacity);
    s = " ibat_mA=";
    while (*s) *p++ = *s++;
    p = put_number(p, ibat_ma);
    s = " peak_mA=";
    while (*s) *p++ = *s++;
    p = put_number(p, peak_ma);
    s = " temp_mA=";
    while (*s) *p++ = *s++;
    p = put_number(p, temp_ma);
    s = " volt_mA=";
    while (*s) *p++ = *s++;
    p = put_number(p, volt_ma);
    s = " temp_limited=";
    while (*s) *p++ = *s++;
    p = put_number(p, temp_limited);
    s = " volt_limited=";
    while (*s) *p++ = *s++;
    p = put_number(p, volt_limited);
    s = " current_mA=";
    while (*s) *p++ = *s++;
    p = put_number(p, current);
    s = " requested_mA=";
    while (*s) *p++ = *s++;
    p = put_number(p, target);
    s = " write_needed=";
    while (*s) *p++ = *s++;
    p = put_number(p, write_needed);
    *p++ = '\n';
    *p = 0;
    log_msg(buf);
}

static char *put_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

/* Report why a node read failed: the stage (open/read/parse), the failing
 * syscall's errno, or the raw content when it does not parse as a number. */
static void log_read_error(const char *stage, const char *path, long error,
                           const char *raw)
{
    char buf[256], *p = buf;
    p = put_str(p, "fccc: ");
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
    p = put_str(p, "fccc: ");
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

/* tpoff mode=both means fixed_temp does not shadow the battery temperature
 * (e.g. iQOO15), so a nonzero fixed_temp is then not a reason to stop. */
static int fixed_independent;

/* 1 = stm32_debug/adapter_temp is readable, adapter temp joins the
 * temperature envelope (normalized onto the battery-temp scale). */
static int adapter_tracking;

static int fixed_overridden(long *fixed)
{
    if (fixed_independent) return 0;
    return read_number(FIXED_NODE, fixed) || *fixed != 0;
}

/* Quiet 1-byte probe: 1 when the OEM high-temperature charge control is
 * disabled (htccc_enable reads 0), 0 otherwise or when the node is missing.
 * Read-only on purpose; another program owns the decision to disable it. */
static int htccc_disabled(void)
{
    char c = 0;
    long fd = sc(SYS_openat, -100, (s64)HTCCC_NODE, 0, 0);
    if (fd < 0) return 0;
    sc(SYS_read, fd, (s64)&c, 1, 0);
    sc(SYS_close, fd, 0, 0, 0);
    return c == '0';
}

/* 1 = node can be opened for reading. */
static int node_readable(const char *path)
{
    long fd = sc(SYS_openat, -100, (s64)path, 0, 0);
    if (fd < 0) return 0;
    sc(SYS_close, fd, 0, 0, 0);
    return 1;
}

/* Quiet probe: cell2_voltage must read as a positive number for the device
 * to be dual-cell. Single-cell devices also expose the node, but report 0
 * or empty content, so node_readable() alone is not enough. */
static int cell2_present(void)
{
    char buf[16];
    long fd, n, i, v = 0;
    fd = sc(SYS_openat, -100, (s64)CELL2_NODE, 0, 0);
    if (fd < 0) return 0;
    n = sc(SYS_read, fd, (s64)buf, (long)sizeof buf - 1, 0);
    sc(SYS_close, fd, 0, 0, 0);
    if (n <= 0) return 0;
    for (i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + buf[i] - '0';
    return i > 0 && v > 0;
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

/* Temperature envelope: a slope-gated ratchet anchored to the peak charge
 * current demonstrated this session (same anchor idea as the voltage
 * ratchet below).
 *
 * The pack is a lagged plant: a current change shows up in the battery
 * temperature minutes later. Per-poll "warmer / cooler" feedback therefore
 * always overshoots -- it keeps cutting through the whole thermal rise (by
 * the temperature peaks the limit sits far below the balance current) and
 * recovers into the next overheating. The fix is to steer by the trend over
 * a whole window instead of the sign of the last sample: the slope returns
 * to zero BEFORE the temperature peak, so cutting stops near the peak
 * instead of after it. The window also averages out the 0.1C sensor
 * quantization that a 1-poll difference cannot tell from a real trend.
 *
 *   slope = temp - temp TEMP_SLOPE_WIN polls ago   (0.1C per window)
 *
 *   temp >= L4   snapped straight to TEMP_FLOOR_MA -- the decisive wall
 *   [L3, L4)     slope > 0: cut at double gain (no patience near the wall),
 *                slope <= 0: hold -- never recover this close to L4
 *   [L2, L3)     slope > db: cut; otherwise hold -- no recovery above L2
 *   (L1, L2)     slope > db: cut proportional to the slope (min 0.5%/poll);
 *                slope < -db: recover 0.5%/poll toward the anchor;
 *                deadband: hold
 *   temp <= L1   released, with lexit re-entry hysteresis
 *
 * Cut magnitude is proportional to the warming rate and stops when the rise
 * does, and recovery only runs while genuinely cooling, so a thermal event
 * costs one bounded excursion instead of a sustained over/undershoot cycle.
 * L1/L2/L3/L4/EXIT = TEMP_* + shift; shift is TEMP_SHIFT_HTCCC_OFF while
 * htccc_enable reads 0, else 0. *cap carries the ratchet state across polls;
 * the caller resets it together with the other envelope state when the
 * charger detaches (prev_temp < 0 also clears the slope window). */
static long current_for_temp(long temp, long prev_temp, long shift,
                             long peak_ma, long *cap, int *limited)
{
    static long hist[TEMP_SLOPE_WIN];
    static int hidx, hfill;
    long l1 = TEMP_L1 + shift, l2 = TEMP_L2 + shift;
    long l3 = TEMP_L3 + shift, l4 = TEMP_L4 + shift;
    long lexit = TEMP_EXIT + shift;
    long anchor = peak_ma >= IBAT_MIN_MA ? peak_ma : BASE_MA;
    long slope, cut;
    int near;
    if (anchor > 15000) anchor = 15000;
    /* Slope window; prev_temp < 0 marks a fresh session. */
    if (prev_temp < 0) { hidx = 0; hfill = 0; }
    hist[hidx] = temp;
    hidx = (hidx + 1) % TEMP_SLOPE_WIN;
    if (hfill < TEMP_SLOPE_WIN) hfill++;
    slope = hfill > 1 ? temp - (hfill >= TEMP_SLOPE_WIN ? hist[hidx] : hist[0]) : 0;
    if (slope > TEMP_SLOPE_CLAMP) slope = TEMP_SLOPE_CLAMP;
    if (slope < -TEMP_SLOPE_CLAMP) slope = -TEMP_SLOPE_CLAMP;
    if (temp <= l1 && (!*limited || temp <= lexit)) {
        *limited = 0;
        *cap = anchor;
        return BASE_MA;
    }
    if (!*limited) {
        /* Fresh entry: start from the demonstrated capability and let the
         * ratchet walk down only as the temperature demands it. */
        *cap = anchor;
    }
    *limited = 1;
    if (temp >= l4) {
        *cap = TEMP_FLOOR_MA;
    } else {
        near = temp >= l3;
        if (slope > (near ? 0 : TEMP_SLOPE_DB)) {
            /* Cutting: proportional to the warming rate, bounded. */
            cut = *cap * slope * (near ? 4 : 2) / 1000;
            if (cut < *cap / (near ? 100 : 200)) cut = *cap / (near ? 100 : 200);
            if (cut > *cap * 5 / 100) cut = *cap * 5 / 100;
            *cap -= cut;
        } else if (!near && temp < l2 && slope < -TEMP_SLOPE_DB) {
            /* Recovering: only below L2, only while clearly cooling. */
            *cap += *cap / 200;
            if (*cap > anchor) *cap = anchor;
        }
        /* Everything else (deadband, cooling near the wall) holds the cap. */
    }
    if (*cap < TEMP_FLOOR_MA) *cap = TEMP_FLOOR_MA;
    return *cap;
}

/* Ratchet anchored to the peak observed charge current: on entry the cap is
 * snapped down to the demonstrated capability, then decays geometrically --
 * each poll multiplies the previous cap by 0.99 at or above VOLT_CRIT, by
 * 0.995 above VOLT_HIGH, so the step shrinks together with the cap instead
 * of growing relative to the falling charge current. Below VOLT_TREND the
 * decay only applies while the voltage is still rising. In the safe band
 * between VOLT_LOW and VOLT_HIGH the cap instead recovers by a fixed 10 mA
 * per poll, slow enough not to reintroduce oscillation, so an overshoot
 * does not stick forever; below VOLT_LOW it resets outright. */
static long cap_for_voltage(long voltage, long prev_voltage, long peak_ma,
                            long cap, int *limited)
{
    if (voltage < VOLT_LOW) {
        *limited = 0;
        return BASE_MA;
    }
    if (voltage <= VOLT_HIGH) {
        cap += 10;
        if (cap >= BASE_MA) {
            *limited = 0;
            return BASE_MA;
        }
        return cap;
    }
    if (peak_ma < IBAT_MIN_MA) return cap;
    if (voltage < VOLT_TREND && prev_voltage >= 0 && voltage <= prev_voltage)
        return cap;
    *limited = 1;
    if (cap > peak_ma) cap = peak_ma;
    cap = voltage >= VOLT_CRIT ? cap * 99 / 100 : cap * 995 / 1000;
    if (cap < VOLT_MIN_MA) cap = VOLT_MIN_MA;
    return cap;
}

static volatile int stop_requested;
static void on_signal(int sig) { (void)sig; stop_requested = 1; }

#ifdef HOSTTEST
static void sig_restorer(void) {}
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

/* Never change a value that another writer has changed. */
static int release_limit(long *last)
{
    long current, fixed;
    if (*last < 0) return 0;
    if (fixed_overridden(&fixed)) {
        log_msg("fccc: unsafe or unknown state; retaining limit\n");
        return -1;
    }
    if (read_number(LIMIT_NODE, &current)) {
        log_msg("fccc: cannot read limit for release\n");
        return -1;
    }
    if (current == 0) {
        *last = -1;
        return 0;
    }
    if (current != *last) {
        log_msg("fccc: limit changed externally; not restoring\n");
        return -1;
    }
    if (write_number(LIMIT_NODE, RELEASE_MA)) {
        log_msg("fccc: failed to release limit\n");
        return -1;
    }
    *last = -1;
    return 0;
}

void __attribute__((noreturn)) run(long argc, char **argv)
{
    long last = -1;
    long last_target = -1;
    long dither = 0;
    long lock_fd;
    long volt_cap = BASE_MA;
    long temp_cap = BASE_MA;
    long ibat_peak = 0;
    long prev_voltage = -1;
    long prev_temp = -1;
    long temp_shift = -1;   /* -1 = not probed yet, else 0 or TEMP_SHIFT_HTCCC_OFF */
    int use_cell_nodes;     /* dual-cell: read per-cell mV from cell*_voltage */
    long cap_release = CAP_RELEASE;
    const char *temp_node;
    int temp_limited = 0, volt_limited = 0, error = 0, write_ticks = 0;
    int was_disconnected = 0;   /* set while unplugged; re-probe htccc on attach */
    if ((argc != 1 && argc != 2) || setsig(2) || setsig(15)) {
        log_msg("fccc: unexpected arguments or signal setup failure\n");
        sc(SYS_exit, 1, 0, 0, 0);
        __builtin_unreachable();
    }
    /* fccc [tpoff_mode]: mode "both" means fixed_temp is independent of the
     * battery temperature, so fixed_temp checks are skipped. */
    if (argc == 2 && argv[1][0] == 'b' && argv[1][1] == 'o' &&
        argv[1][2] == 't')
        fixed_independent = 1;
    lock_fd = sc(SYS_openat, -100, (s64)LOCK_PATH, 2 | 0100 | 0400000, 0600);
    if (lock_fd < 0 || sc(SYS_flock, lock_fd, 2 | 4, 0, 0) < 0) {
        log_msg("fccc: cannot acquire instance lock\n");
        sc(SYS_exit, 1, 0, 0, 0);
        __builtin_unreachable();
    }
    log_msg(fixed_independent ? "fccc: started (fixed_temp checks off)\n"
                              : "fccc: started\n");
    /* Probe the OEM 45C wall switch once at startup (and again on each
     * charger attach, see was_disconnected below); never written by us. */
    temp_shift = htccc_disabled() ? TEMP_SHIFT_HTCCC_OFF : 0;
    if (temp_shift)
        log_msg("fccc: htccc disabled; temperature thresholds +3C\n");
    /* Battery temperature source: cms_class/main_batt_temp preferred; some
     * devices lack it, then fall back to power_supply temp (also 0.1C). */
    temp_node = TEMP_NODE;
    if (!node_readable(TEMP_NODE) && node_readable(TEMP_NODE_ALT)) {
        temp_node = TEMP_NODE_ALT;
        log_msg("fccc: main_batt_temp unavailable; using power_supply battery temp\n");
    }
    /* Charger temperature tracking is best-effort: the adapter only reports
     * it while fast-charge protocol chatter is active, so a missing node or
     * a failed read just leaves the battery as the only source. */
    adapter_tracking = node_readable(ADPT_TEMP_NODE);
    log_msg(adapter_tracking ? "fccc: adapter temp tracking on (85.0C ceiling)\n"
                             : "fccc: adapter temp node missing; battery temp only\n");
    /* Dual-cell (2S) devices report real per-cell voltages in mV via cms;
     * prefer them over battery/voltage_now, which reads the whole pack
     * there. cell2_voltage must hold a positive value -- the node exists
     * but reads 0/empty on single-cell devices. */
    use_cell_nodes = node_readable(CELL1_NODE) && cell2_present();
    if (use_cell_nodes) {
        cap_release = CAP_RELEASE_DUAL;
        log_msg("fccc: dual-cell device; using cms cell1/cell2_voltage (mV), cap release 82%\n");
    }
    while (!stop_requested) {
        long fixed, temp, adpt, voltage, capacity, ibat_ma, temp_ma, volt_ma, current, target;
        int state = charging();
        if (fixed_overridden(&fixed)) {
            log_msg("fccc: fixed_temp unavailable or active; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (state < 0) {
            log_msg("fccc: charging status unavailable; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (!state) {
            if (release_limit(&last)) { error = 1; break; }
            temp_limited = 0;
            volt_limited = 0;
            volt_cap = BASE_MA;
            temp_cap = BASE_MA;
            ibat_peak = 0;
            prev_voltage = -1;
            prev_temp = -1;
            last_target = -1;
            write_ticks = 0;
            was_disconnected = 1;
            sleep_second();
            continue;
        }
        if (read_number(temp_node, &temp) || fixed_overridden(&fixed)) {
            log_msg("fccc: unreadable or overridden battery temperature; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (temp < -400 || temp > 1250) {
            log_bad_value("battery temperature", temp);
            error = 1;
            break;
        }
        if (use_cell_nodes) {
            long c1, c2;
            if (read_number(CELL1_NODE, &c1) ||
                read_number(CELL2_NODE, &c2)) {
                log_msg("fccc: unavailable per-cell voltage; stopping, retaining limit\n");
                error = 1;
                break;
            }
            /* mV -> uV; the higher cell drives the voltage envelope */
            voltage = (c1 > c2 ? c1 : c2) * 1000;
        } else if (read_number(VOLT_NODE, &voltage)) {
            log_msg("fccc: unavailable single-cell battery voltage; stopping, retaining limit\n");
            error = 1;
            break;
        }
        if (voltage < 2000000 || voltage > 5000000) {
            log_bad_value("single-cell battery voltage", voltage);
            error = 1;
            break;
        }
        if (read_number(LIMIT_NODE, &current)) {
            log_msg("fccc: cannot read current limit; stopping\n");
            error = 1;
            break;
        }
        if (read_number(CAP_NODE, &capacity)) {
            log_msg("fccc: unavailable battery capacity; stopping, retaining limit\n");
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
            log_msg("fccc: cannot read battery current; stopping, retaining limit\n");
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
        /*  // 检查 被其它程序修改的情况
            if ((last < 0 && current != 0 && current != RELEASE_MA) ||
                (last >= 0 && current != last)) {
                log_msg("fccc: limit already in use or changed externally; stopping\n");
                error = 1;
                break;
            }
        */
        int write_needed;
        long write_val;
        if (ibat_ma > ibat_peak) ibat_peak = ibat_ma;
        /* Re-read the OEM 45C wall switch on charger attach (never written
         * by us): while htccc is off, run the temperature envelope 3C higher. */
        if (was_disconnected) {
            long shift = htccc_disabled() ? TEMP_SHIFT_HTCCC_OFF : 0;
            was_disconnected = 0;
            if (shift != temp_shift) {
                log_msg(shift ? "fccc: htccc disabled; temperature thresholds +3C\n"
                              : "fccc: htccc enabled; temperature thresholds normal\n");
                temp_shift = shift;
            }
        }
        adpt = -1;
        if (adapter_tracking) {
            long raw;
            if (!read_number(ADPT_TEMP_NODE, &raw) && raw >= 0 && raw <= 1250) {
                /* Normalize the adapter's 85C ceiling onto the battery-temp
                 * axis (same scale the TEMP_L* thresholds live on), then let
                 * the hottest of the two sources drive the envelope:
                 * adapter 80C with L3+shift=475 reads as 80/85*475 ~= 447,
                 * i.e. just under the L3 wall. raw 0 (not reported yet)
                 * scales to 0 and never limits. */
                adpt = raw;
                raw = raw * (TEMP_L3 + temp_shift) / ADPT_TEMP_MAX;
                if (raw > temp) temp = raw;
            }
        }
        temp_ma = current_for_temp(temp, prev_temp, temp_shift, ibat_peak,
                                   &temp_cap, &temp_limited);
        prev_temp = temp;
        volt_cap = cap_for_voltage(voltage, prev_voltage, ibat_peak, volt_cap,
                                   &volt_limited);
        volt_ma = volt_cap;
        prev_voltage = voltage;
        target = temp_ma < volt_ma ? temp_ma : volt_ma;
        /* Above the capacity threshold the forced current is disabled, but
         * only once the (envelope-driving) temperature has cooled to the
         * RAW TEMP_L2 -- no htccc shift: the shifted envelope is fccc's own
         * privilege for pushing power, while handing back to the OEM stack
         * at a floated-up temperature would mean releasing into weakened
         * OEM protections. If fccc is limiting too hard the temperature
         * recovers and the release follows; if it stays hot, fccc was not
         * over-limiting and keeps control. CAP_FORCE_EXIT is the escape
         * hatch either way. */
        if (capacity > cap_release &&
            (capacity >= CAP_FORCE_EXIT || temp <= TEMP_L2))
            target = RELEASE_MA;
        /* A write issued before fast charging activates may not take effect,
         * so rewrite the target every 5 seconds even when it is unchanged. */
        write_needed = target != last_target || ++write_ticks >= 5;
        /* A write equal to the value already in the node may be dropped by the
         * driver, so nudge nonzero repeats by a cycling 1-50 mA offset. Never
         * nudge a release (0), which would impose a near-zero limit instead. */
        write_val = target;
        if (write_needed && target > 0 && target == current) {
            dither = dither % 50 + 1;
            write_val = target + dither;
        }
        log_limit(temp, adpt, voltage, capacity, ibat_ma, ibat_peak, temp_ma,
                  volt_ma, temp_limited, volt_limited, current, target,
                  write_needed);
        if (write_needed) {
            write_ticks = 0;
            if (fixed_overridden(&fixed)) {
                log_msg("fccc: fixed_temp changed; stopping, retaining limit\n");
                error = 1;
                break;
            }
            if (write_number(LIMIT_NODE, write_val)) {
                log_msg("fccc: limit write or readback failed; stopping\n");
                error = 1;
                break;
            }
            last = write_val;
            last_target = target;
        }
        sleep_second();
    }
    if (!error && release_limit(&last)) error = 1;
    log_msg(error ? "fccc: stopped with error\n" : "fccc: stopped\n");
    sc(SYS_exit, error, 0, 0, 0);
    __builtin_unreachable();
}

#ifndef HOSTTEST
void __attribute__((naked)) _start(void)
{
    asm volatile("ldr x0, [sp]\nadd x1, sp, #8\nb run");
}
#endif

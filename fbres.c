/* runtime patch: make CMS use the fboff primary-board-temp table when screen on */
typedef unsigned long u64;
typedef int s32;
typedef unsigned int u32;
struct power_supply;
extern struct power_supply *power_supply_get_by_name(const char *name);
extern void *power_supply_get_drvdata(struct power_supply *psy);
extern int _printk(const char *fmt, ...);

/* vivo,sp-primary-fbon-tc-data (identical for eq) */
static const s32 sp_fboff[30] = {
    470, 8888, 1600, 450, 469, 2600, 425, 449, 3100, 410, 424, 3600,
    395, 409, 4500, 385, 394, 6200, 370, 384, 7700, 360, 369, 10000,
    340, 359, 12000, -8888, 339, 17600
};
/* vivo,sp-primary-fboff-tc-data */
static const s32 sp_fbon[30] = {
    475, 8888, 1600, 465, 474, 2400, 453, 464, 3300, 444, 452, 5400,
    437, 443, 6550, 420, 436, 7350, 400, 419, 8400, 375, 399, 10000,
    345, 374, 12000, -8888, 344, 17600
};
/* candidate chip-struct offsets of primary-table pointer arrays */
static const u32 data_offs[4] = { 0xb750 };

static int kptr_ok(u64 p)
{
    return ((p & 0xffff000000000000UL) == 0xffff000000000000UL) && !(p & 7);
}

int init_module(void)
{
    struct power_supply *psy = power_supply_get_by_name("cms");
    char *chip;
    s32 *bufs[4];
    int n = 0, o, i, k;

    if (!psy) { _printk("fbres: no cms psy\n"); return -19; }
    chip = (char *)power_supply_get_drvdata(psy);
    if (!chip) { _printk("fbres: no drvdata\n"); return -22; }
    _printk("fbres: chip=%px\n", chip);

    for (o = 0; o < 4; o++) {
        for (i = 0; i < 6; i++) {
            u64 p = *(volatile u64 *)(chip + data_offs[o] + i * 8);
            s32 *t;
            int match;
            if (!kptr_ok(p))
                continue;
            t = (s32 *)(u64)p;
            match = 1;
            for (k = 0; k < 30; k++)
                if (t[k] != sp_fbon[k]) { match = 0; break; }
            if (match) {
                _printk("fbres: fbon table at %px (off %x idx %d)\n",
                        (void *)t, data_offs[o], i);
                if (n < 4)
                    bufs[n++] = t;
            }
        }
    }
    if (n != 2) {
        _printk("fbres: found %d fbon tables, expected 2, abort\n", n);
        return -22;
    }
    for (o = 0; o < n; o++) {
        volatile s32 *t = bufs[o];
        for (k = 0; k < 30; k++)
            t[k] = sp_fboff[k];
        _printk("fbres: patched %px\n", (void *)t);
    }
    _printk("fbres: done, fbon restored to stock fbon table\n");
    return 0;
}

void cleanup_module(void) {}

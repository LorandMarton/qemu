/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stage 1 translations tagged with ASIDs.
 *
 * Several address spaces, each with its own translation tables and ASID,
 * map the same virtual addresses to their own data page and code page,
 * with non-global entries.  Switching between them must always give the
 * translations of the current one, and a TLB invalidation must reach the
 * entries of the address spaces it names, current or not.  A TLB
 * invalidation by VA and ASID must also reach the global entries of that
 * VA, whichever address space they were cached in, and an invalidation of
 * any page of a block must reach all of the block, the code in it included.
 * The RAM map of the test itself is non-global too, so that a TLB holds
 * global entries only where a test caches them.
 *
 * Two configurations: the ASID in TTBR0_EL1 with 8-bit ASIDs (TCR_EL1.A1 0,
 * AS 0, so ASIDs that differ in bits [15:8] only are the same), and the
 * ASID in TTBR1_EL1 with 16-bit ASIDs (A1 1, AS 1), as Linux uses them.
 */

#include <stdint.h>
#include <minilib.h>

#define PAGE            4096
#define N_AS            12      /* more than QEMU keeps TLBs for */
#define N_ALT           9

/* The test addresses, at 3 GiB: level 1 index 3, outside the RAM map. */
#define TEST_VA         0xc0000000ull
#define DATA_VA         TEST_VA
#define CODE_VA         (TEST_VA + PAGE)
/* The same global pages in every address space */
#define GDATA_VA        (TEST_VA + 2 * PAGE)
#define GCODE_VA        (TEST_VA + 3 * PAGE)
/*
 * A 2 MiB block in address space 1 (level 2 index 1), with code 3 pages
 * into it, of one of two blocks of RAM after the test image; address space
 * 0 maps those at the next two indexes, to write the code.
 */
#define BLOCK           0x200000
#define BLOCK_VA        (TEST_VA + BLOCK)
#define BLOCK_CODE      (3 * PAGE)
#define BLOCK_PA(i)     ((1ull << 30) + 32 * 0x100000 + (i) * BLOCK)

#define DESC_TABLE      3ull
#define DESC_BLOCK      1ull
#define DESC_PAGE       3ull
#define DESC_AF         (1ull << 10)
#define DESC_SH_INNER   (3ull << 8)
#define DESC_NG         (1ull << 11)
#define DESC_UXN        (1ull << 54)
#define ADDR_MASK       0x0000fffffffff000ull

#define TCR_A1          (1ull << 22)
#define TCR_EPD1        (1ull << 23)
#define TCR_AS          (1ull << 36)

typedef struct {
    uint64_t l1[512];
    uint64_t l2[512];
    uint64_t l3[512];
} Tables;

static Tables tables[N_AS] __attribute__((aligned(PAGE)));
static uint64_t ram_l2[512] __attribute__((aligned(PAGE)));
static uint32_t data[N_AS + N_ALT][PAGE / 4] __attribute__((aligned(PAGE)));
static uint32_t code[N_AS + N_ALT][PAGE / 4] __attribute__((aligned(PAGE)));
static uint32_t gdata[2][PAGE / 4] __attribute__((aligned(PAGE)));
static uint32_t gcode[2][PAGE / 4] __attribute__((aligned(PAGE)));

static int errors;
static int asid_in_ttbr1;

static uint64_t page_desc(void *pa)
{
    return (uintptr_t)pa | DESC_AF | DESC_SH_INNER | DESC_NG | DESC_UXN |
           DESC_PAGE;
}

static uint64_t block_desc(uint64_t pa)
{
    return pa | DESC_AF | DESC_SH_INNER | DESC_NG | DESC_BLOCK;
}

static uint64_t global_desc(void *pa)
{
    return (uintptr_t)pa | DESC_AF | DESC_SH_INNER | DESC_UXN | DESC_PAGE;
}

/* Bit 8 set for the odd ones: no effect on the ASID with 8-bit ASIDs. */
static uint64_t asid_of(int as)
{
    return (as + 1) | (as & 1) << 8;
}

/* The ASID operand of a TLBI for the address space. */
static uint64_t tlbi_asid(int as)
{
    return (asid_in_ttbr1 ? asid_of(as) : asid_of(as) & 0xff) << 48;
}

static void switch_to(int as)
{
    uint64_t l1 = (uintptr_t)tables[as].l1;

    if (asid_in_ttbr1) {
        asm volatile("msr ttbr0_el1, %0\n\t"
                     "msr ttbr1_el1, %1\n\t"
                     "isb"
                     : : "r"(l1), "r"(asid_of(as) << 48) : "memory");
    } else {
        asm volatile("msr ttbr0_el1, %0\n\tisb"
                     : : "r"(l1 | asid_of(as) << 48) : "memory");
    }
}

static void set_pte(int as, int idx, void *pa)
{
    tables[as].l3[idx] = page_desc(pa);
    asm volatile("dsb ishst" : : : "memory");
}

static void tlbi_done(void)
{
    asm volatile("dsb ish\n\tisb" : : : "memory");
}

static uint32_t read_data(void)
{
    /* volatile: each check loads through the current translation. */
    return *(volatile uint32_t *)DATA_VA;
}

static uint32_t run_code(void)
{
    uint32_t (*fn)(void) = (uint32_t (*)(void))CODE_VA;

    return fn();
}

/* "mov w0, #value; ret" */
static void write_code(uint32_t *p, uint32_t value)
{
    p[0] = 0x52800000 | value << 5;
    p[1] = 0xd65f03c0;
}

static void check(int as, uint32_t want, const char *what)
{
    uint32_t d = read_data();
    uint32_t c = run_code();

    if (d != want || c != want) {
        ml_printf("FAIL: %s, ASID in TTBR%d: address space %d: data %d, "
                  "code %d, want %d\n", what, asid_in_ttbr1, as, d, c, want);
        errors++;
    }
}

static void setup(void)
{
    uint64_t *boot_l1;
    int ram_l1_index = -1;

    asm volatile("mrs %0, ttbr0_el1" : "=r"(boot_l1));
    boot_l1 = (uint64_t *)((uintptr_t)boot_l1 & ~(PAGE - 1ull));

    for (int i = 0; i < N_AS + N_ALT; i++) {
        data[i][0] = i + 1;
        write_code(code[i], i + 1);
    }
    for (int i = 0; i < 2; i++) {
        gdata[i][0] = 100 + i;
        write_code(gcode[i], 100 + i);
    }
    /* The RAM identity map of boot.S (blocks in one table), non-global */
    for (int i = 0; i < 512; i++) {
        if ((boot_l1[i] & 3) == DESC_TABLE) {
            uint64_t *l2 = (uint64_t *)(uintptr_t)(boot_l1[i] & ADDR_MASK);

            for (int j = 0; j < 512; j++) {
                ram_l2[j] = l2[j] & 1 ? l2[j] | DESC_NG : l2[j];
            }
            ram_l1_index = i;
        }
    }
    for (int as = 0; as < N_AS; as++) {
        Tables *t = &tables[as];

        for (int i = 0; i < 512; i++) {
            t->l1[i] = boot_l1[i];
        }
        t->l1[ram_l1_index] = (uintptr_t)ram_l2 | DESC_TABLE;
        t->l1[TEST_VA >> 30] = (uintptr_t)t->l2 | DESC_TABLE;
        t->l2[0] = (uintptr_t)t->l3 | DESC_TABLE;
        t->l3[0] = page_desc(data[as]);
        t->l3[1] = page_desc(code[as]);
        t->l3[2] = global_desc(gdata[0]);
        t->l3[3] = global_desc(gcode[0]);
    }
    tables[1].l2[1] = block_desc(BLOCK_PA(0));
    for (int i = 0; i < 2; i++) {
        tables[0].l2[2 + i] = block_desc(BLOCK_PA(i));
    }
    asm volatile("dsb ishst" : : : "memory");
    switch_to(0);
    /* No global entries of the boot map from here on */
    asm volatile("tlbi vmalle1\n\tdsb nsh\n\tisb" : : : "memory");
    for (int i = 0; i < 2; i++) {
        write_code((uint32_t *)(BLOCK_VA + (1 + i) * BLOCK + BLOCK_CODE),
                   200 + i);
    }
    asm volatile("dsb ish\n\tic iallu\n\tdsb ish\n\tisb" : : : "memory");
}

static void set_tcr(int ttbr1)
{
    uint64_t tcr;

    asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
    tcr &= ~(TCR_A1 | TCR_AS);
    /* No walks from TTBR1_EL1: only its ASID is used. */
    tcr |= TCR_EPD1 | (ttbr1 ? TCR_A1 | TCR_AS : 0);
    asm volatile("msr tcr_el1, %0\n\t"
                 "isb\n\t"
                 "tlbi vmalle1\n\t"
                 "dsb nsh\n\t"
                 "isb" : : "r"(tcr) : "memory");
    asid_in_ttbr1 = ttbr1;
}

/* Each address space sees its own pages, whatever the order of switches. */
static void test_switch(void)
{
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < N_AS; i++) {
            /* Visit a few of them often, all of them now and then. */
            int as = round & 1 ? (i * 7 + round) % N_AS : i % 3;

            switch_to(as);
            check(as, as + 1, "switch");
        }
    }
}

enum {
    TLBI_VAE1IS,
    TLBI_VALE1IS,
    TLBI_VAE1,
    TLBI_VAAE1IS,
    TLBI_ASIDE1IS,
    TLBI_ASIDE1,
    TLBI_VMALLE1IS,
    TLBI_RVAE1IS,
    TLBI_RVAAE1IS,
};

static const char *const tlbi_name[N_ALT] = {
    "vae1is", "vale1is", "vae1", "vaae1is", "aside1is", "aside1",
    "vmalle1is", "rvae1is", "rvaae1is",
};

static void tlbi(int kind, int as)
{
    uint64_t asid = tlbi_asid(as);
    /* Range: TG 4K, SCALE 0, NUM 0 -> 2 pages from BaseADDR. */
    uint64_t range = 1ull << 46 | TEST_VA >> 12;

    switch (kind) {
    case TLBI_VAE1IS:
        asm volatile("tlbi vae1is, %0" : : "r"(asid | DATA_VA >> 12));
        asm volatile("tlbi vae1is, %0" : : "r"(asid | CODE_VA >> 12));
        break;
    case TLBI_VALE1IS:
        asm volatile("tlbi vale1is, %0" : : "r"(asid | DATA_VA >> 12));
        asm volatile("tlbi vale1is, %0" : : "r"(asid | CODE_VA >> 12));
        break;
    case TLBI_VAE1:
        asm volatile("tlbi vae1, %0" : : "r"(asid | DATA_VA >> 12));
        asm volatile("tlbi vae1, %0" : : "r"(asid | CODE_VA >> 12));
        break;
    case TLBI_VAAE1IS:
        asm volatile("tlbi vaae1is, %0" : : "r"(DATA_VA >> 12));
        asm volatile("tlbi vaae1is, %0" : : "r"(CODE_VA >> 12));
        break;
    case TLBI_ASIDE1IS:
        asm volatile("tlbi aside1is, %0" : : "r"(asid));
        break;
    case TLBI_ASIDE1:
        asm volatile("tlbi aside1, %0" : : "r"(asid));
        break;
    case TLBI_VMALLE1IS:
        asm volatile("tlbi vmalle1is");
        break;
    case TLBI_RVAE1IS:
        asm volatile("sys #0, c8, c2, #1, %0" : : "r"(asid | range));
        break;
    case TLBI_RVAAE1IS:
        asm volatile("sys #0, c8, c2, #3, %0" : : "r"(range));
        break;
    }
    tlbi_done();
}

/*
 * Change the pages of address space 1, invalidate them while address space
 * @from is current, then use them from address space 1.
 */
static void test_invalidate(int kind, int from, int range_ok)
{
    const int as = 1, other = 2;
    uint32_t want = N_AS + kind + 1;

    if (!range_ok && (kind == TLBI_RVAE1IS || kind == TLBI_RVAAE1IS)) {
        return;
    }

    /* Use the pages in both, after a switch to each. */
    switch_to(other);
    switch_to(as);
    check(as, as + 1, "before invalidation");
    switch_to(other);
    check(other, other + 1, "before invalidation");

    switch_to(from);
    if (from == as) {
        /* The jump cache holds this code of the address space now. */
        check(as, as + 1, "before invalidation");
    }
    set_pte(as, 0, data[N_AS + kind]);
    set_pte(as, 1, code[N_AS + kind]);
    tlbi(kind, as);
    if (from == as) {
        check(as, want, tlbi_name[kind]);
    }
    switch_to(other);
    check(other, other + 1, tlbi_name[kind]);

    switch_to(as);
    check(as, want, tlbi_name[kind]);

    /* Back to the original pages. */
    set_pte(as, 0, data[as]);
    set_pte(as, 1, code[as]);
    tlbi(TLBI_VMALLE1IS, as);
    check(as, as + 1, "restore");
}

static void check_global(int as, uint32_t want, const char *what)
{
    /* volatile: each check loads through the current translation. */
    uint32_t d = *(volatile uint32_t *)GDATA_VA;
    uint32_t c = ((uint32_t (*)(void))GCODE_VA)();

    if (d != want || c != want) {
        ml_printf("FAIL: global pages, %s, ASID in TTBR%d: address space %d: "
                  "data %d, code %d, want %d\n", what, asid_in_ttbr1, as, d,
                  c, want);
        errors++;
    }
}

/*
 * Cache the global pages in address spaces 1 and 2, change them, invalidate
 * them by VA with the ASID of address space 3 while it is current: 1 and 2
 * must see the new pages.
 */
static void test_invalidate_global(int kind)
{
    const char *what = tlbi_name[kind];

    for (int as = 1; as <= 2; as++) {
        switch_to(as);
        check_global(as, 100, "before invalidation");
    }
    switch_to(3);
    for (int as = 0; as < N_AS; as++) {
        tables[as].l3[2] = global_desc(gdata[1]);
        tables[as].l3[3] = global_desc(gcode[1]);
    }
    asm volatile("dsb ishst" : : : "memory");
    switch (kind) {
    case TLBI_VAE1IS:
        asm volatile("tlbi vae1is, %0" : : "r"(tlbi_asid(3) | GDATA_VA >> 12));
        asm volatile("tlbi vae1is, %0" : : "r"(tlbi_asid(3) | GCODE_VA >> 12));
        break;
    case TLBI_VALE1IS:
        asm volatile("tlbi vale1is, %0" : : "r"(tlbi_asid(3) | GDATA_VA >> 12));
        asm volatile("tlbi vale1is, %0" : : "r"(tlbi_asid(3) | GCODE_VA >> 12));
        break;
    case TLBI_VAE1:
        asm volatile("tlbi vae1, %0" : : "r"(tlbi_asid(3) | GDATA_VA >> 12));
        asm volatile("tlbi vae1, %0" : : "r"(tlbi_asid(3) | GCODE_VA >> 12));
        break;
    }
    tlbi_done();
    for (int as = 1; as <= 2; as++) {
        switch_to(as);
        check_global(as, 101, what);
    }

    /* Back to the original pages. */
    for (int as = 0; as < N_AS; as++) {
        tables[as].l3[2] = global_desc(gdata[0]);
        tables[as].l3[3] = global_desc(gcode[0]);
    }
    asm volatile("dsb ishst" : : : "memory");
    tlbi(TLBI_VMALLE1IS, 1);
}

/*
 * Run the code in the block of address space 1, change the block and
 * invalidate its first page only, while address space @from is current,
 * then run the code again from address space 1.
 */
static void test_invalidate_block(int from)
{
    const int as = 1;
    uint32_t (*fn)(void) = (uint32_t (*)(void))(BLOCK_VA + BLOCK_CODE);
    uint32_t before, after;

    /* A switch to it, for its TLB to be kept when it is not current. */
    switch_to(as + 1);
    switch_to(as);
    before = fn();
    switch_to(from);
    tables[as].l2[1] = block_desc(BLOCK_PA(1));
    asm volatile("dsb ishst" : : : "memory");
    asm volatile("tlbi vae1is, %0" : : "r"(tlbi_asid(as) | BLOCK_VA >> 12));
    tlbi_done();
    switch_to(as);
    after = fn();
    if (before != 200 || after != 201) {
        ml_printf("FAIL: block, from address space %d, ASID in TTBR%d: "
                  "code %d then %d, want 200 then 201\n", from,
                  asid_in_ttbr1, before, after);
        errors++;
    }

    /* Back to the original block. */
    tables[as].l2[1] = block_desc(BLOCK_PA(0));
    asm volatile("dsb ishst" : : : "memory");
    tlbi(TLBI_VMALLE1IS, as);
}

int main(void)
{
    uint64_t isar0;
    int range_ok;

    /* ID_AA64ISAR0_EL1.TLB: 2 for TLB range maintenance. */
    asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    range_ok = ((isar0 >> 56) & 0xf) >= 2;

    setup();
    for (int ttbr1 = 0; ttbr1 <= 1; ttbr1++) {
        set_tcr(ttbr1);
        test_switch();
        for (int kind = 0; kind < N_ALT; kind++) {
            test_invalidate(kind, 2, range_ok);     /* from another */
            test_invalidate(kind, 1, range_ok);     /* from itself */
        }
        test_invalidate_global(TLBI_VAE1IS);
        test_invalidate_global(TLBI_VALE1IS);
        test_invalidate_global(TLBI_VAE1);
        test_invalidate_block(1);
        test_invalidate_block(2);
        /* And the address spaces are still distinct after all that. */
        test_switch();
    }

    if (errors) {
        ml_printf("FAIL: %d errors\n", errors);
        return 1;
    }
    ml_printf("PASS\n");
    return 0;
}

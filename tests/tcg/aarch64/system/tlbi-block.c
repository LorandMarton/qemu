/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * TLB invalidation by VA of a block mapping.
 *
 * An invalidation by VA removes the entry of the whole block that holds the
 * VA, and the code translated anywhere in the block must go with it: run
 * code three pages into a 2 MiB block, map the block to other RAM,
 * invalidate the first page of the block only, and run the code again.
 */

#include <stdint.h>
#include <minilib.h>

#define PAGE            4096
#define BLOCK           0x200000
/* At 3 GiB: level 1 index 3, outside the RAM map */
#define BLOCK_VA        0xc0000000ull
#define CODE            (3 * PAGE)
/*
 * Two blocks of RAM after the test image; level 2 indexes 1 and 2 map them
 * too, to write the code.
 */
#define BLOCK_PA(i)     ((1ull << 30) + 32 * 0x100000 + (i) * BLOCK)

#define DESC_TABLE      3ull
#define DESC_BLOCK      1ull
#define DESC_AF         (1ull << 10)
#define DESC_SH_INNER   (3ull << 8)

enum {
    TLBI_VAE1,
    TLBI_VAE1IS,
    TLBI_VALE1IS,
    TLBI_VAAE1IS,
    N_KIND
};

static const char *const tlbi_name[N_KIND] = {
    "vae1", "vae1is", "vale1is", "vaae1is",
};

static uint64_t l1[512] __attribute__((aligned(PAGE)));
static uint64_t l2[512] __attribute__((aligned(PAGE)));

static uint64_t block_desc(int i)
{
    return BLOCK_PA(i) | DESC_AF | DESC_SH_INNER | DESC_BLOCK;
}

/* "mov w0, #value; ret" in block @i */
static void write_code(int i, uint32_t value)
{
    uint32_t *p = (uint32_t *)(BLOCK_VA + (1 + i) * BLOCK + CODE);

    p[0] = 0x52800000 | value << 5;
    p[1] = 0xd65f03c0;
}

static uint32_t run_code(void)
{
    return ((uint32_t (*)(void))(BLOCK_VA + CODE))();
}

static void set_block(int i)
{
    l2[0] = block_desc(i);
    asm volatile("dsb ishst" : : : "memory");
}

/* The first page of the block, ASID 0 */
static void tlbi(int kind)
{
    uint64_t va = BLOCK_VA >> 12;

    switch (kind) {
    case TLBI_VAE1:
        asm volatile("tlbi vae1, %0" : : "r"(va));
        break;
    case TLBI_VAE1IS:
        asm volatile("tlbi vae1is, %0" : : "r"(va));
        break;
    case TLBI_VALE1IS:
        asm volatile("tlbi vale1is, %0" : : "r"(va));
        break;
    case TLBI_VAAE1IS:
        asm volatile("tlbi vaae1is, %0" : : "r"(va));
        break;
    }
    asm volatile("dsb ish\n\tisb" : : : "memory");
}

int main(void)
{
    uint64_t *boot_l1;
    int errors = 0;

    /* The RAM identity map of boot.S, and the blocks at 3 GiB */
    asm volatile("mrs %0, ttbr0_el1" : "=r"(boot_l1));
    boot_l1 = (uint64_t *)((uintptr_t)boot_l1 & ~(PAGE - 1ull));
    for (int i = 0; i < 512; i++) {
        l1[i] = boot_l1[i];
    }
    l1[BLOCK_VA >> 30] = (uintptr_t)l2 | DESC_TABLE;
    l2[0] = block_desc(0);
    l2[1] = block_desc(0);
    l2[2] = block_desc(1);
    asm volatile("dsb ish\n\t"
                 "msr ttbr0_el1, %0\n\t"
                 "isb\n\t"
                 "tlbi vmalle1\n\t"
                 "dsb nsh\n\t"
                 "isb" : : "r"(l1) : "memory");
    write_code(0, 1);
    write_code(1, 2);
    asm volatile("dsb ish\n\tic iallu\n\tdsb ish\n\tisb" : : : "memory");

    for (int kind = 0; kind < N_KIND; kind++) {
        uint32_t before, after;

        before = run_code();
        set_block(1);
        tlbi(kind);
        after = run_code();
        if (before != 1 || after != 2) {
            ml_printf("FAIL: tlbi %s: code %d then %d, want 1 then 2\n",
                      tlbi_name[kind], before, after);
            errors++;
        }

        /* Back to the first block */
        set_block(0);
        asm volatile("tlbi vmalle1\n\tdsb nsh\n\tisb" : : : "memory");
    }

    if (errors) {
        ml_printf("FAIL: %d errors\n", errors);
        return 1;
    }
    ml_printf("PASS\n");
    return 0;
}

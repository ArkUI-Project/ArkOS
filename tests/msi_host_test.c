/* Host unit test for MSI-X capability decode and table-entry encoding
 * (kernel/msi.c). Only the pure helpers are linked: the runtime half is behind
 * ARK_MSI_HOST_TEST. */
#define ARK_MSI_HOST_TEST 1
#include "msi.h"
#include "../kernel/msi.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(void) {
    MsixInfo info;

    printf("msi_decode_msix: basic\n");
    /* Message control: table size field 31 -> 32 entries, plus the enable and
     * function-mask bits, which decode must preserve untouched. */
    uint16_t control = (uint16_t)(31u | 0x8000u | 0x4000u);
    CHECK(msi_decode_msix(control, 0x00002004u, 0x00003004u, &info));
    CHECK(info.control == control);
    CHECK(info.table_size == 32);
    CHECK(info.table_bir == 4);
    CHECK(info.table_offset == 0x2000);
    CHECK(info.pba_bir == 4);
    CHECK(info.pba_offset == 0x3000);

    printf("msi_decode_msix: minimum size and BIR 0\n");
    CHECK(msi_decode_msix(0, 0, 0, &info));
    CHECK(info.table_size == 1);
    CHECK(info.table_bir == 0);
    CHECK(info.table_offset == 0);
    CHECK(info.pba_offset == 0);

    printf("msi_decode_msix: maximum size 2048\n");
    CHECK(msi_decode_msix(0x7ff, 0x8, 0x8, &info));
    CHECK(info.table_size == 2048);
    CHECK(info.table_offset == 8);

    printf("msi_decode_msix: rejects reserved BIR and null out\n");
    CHECK(!msi_decode_msix(0, 6, 0, &info)); /* table BIR 6 reserved */
    CHECK(!msi_decode_msix(0, 0, 7, &info)); /* PBA BIR 7 reserved */
    CHECK(!msi_decode_msix(0, 0, 0, 0));     /* null out pointer */
    /* BIR 5 is the last valid BAR and the offset keeps its high bits. */
    CHECK(msi_decode_msix(0, 0x00004005u, 0, &info));
    CHECK(info.table_bir == 5);
    CHECK(info.table_offset == 0x4000);

    printf("msi_encode_entry\n");
    uint32_t entry[4] = {0xdead, 0xdead, 0xdead, 0xdead};
    msi_encode_entry(0x43, 0x2, entry);
    CHECK(entry[0] == (0xFEE00000u | (2u << 12)));
    CHECK(entry[1] == 0);
    CHECK(entry[2] == 0x43);
    CHECK(entry[3] == 0); /* unmasked: the kernel unmasks after programming */

    printf("msi_encode_entry: high apic id and vector\n");
    msi_encode_entry(0xff, 0xff, entry);
    CHECK(entry[0] == (0xFEE00000u | (0xffu << 12)));
    CHECK(entry[2] == 0xff);

    printf("vector window\n");
    CHECK(MSI_VECTOR_FIRST == 0x40);
    CHECK(MSI_VECTOR_LAST == 0xEF);
    /* The INT80 syscall gate sits inside the window and must stay reserved. */
    CHECK(MSI_VECTOR_RESERVED == 0x80);
    CHECK(MSI_VECTOR_RESERVED > MSI_VECTOR_FIRST && MSI_VECTOR_RESERVED < MSI_VECTOR_LAST);

    printf("PASS\n");
    return 0;
}

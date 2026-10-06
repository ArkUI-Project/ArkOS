/* MSI-X vector allocation and PCI capability programming (see include/msi.h).
 * The pure decode/encode helpers are shared with the host test; everything
 * below the ARK_MSI_HOST_TEST guard touches real PCI configuration space. */
#include "msi.h"

bool msi_decode_msix(uint16_t message_control, uint32_t table_off_bir,
                     uint32_t pba_off_bir, MsixInfo *out) {
    if (!out)
        return false;
    /* Low three bits name the BAR; the rest is a byte offset from that BAR's
     * base. Only BARs 0..5 can host the table/PBA (6 and 7 are reserved). */
    unsigned table_bir = table_off_bir & 7u;
    unsigned pba_bir = pba_off_bir & 7u;
    if (table_bir > 5 || pba_bir > 5)
        return false;
    out->control = message_control;
    out->table_size = (unsigned)(message_control & 0x7ffu) + 1u;
    out->table_bir = table_bir;
    out->table_offset = table_off_bir & ~7u;
    out->pba_bir = pba_bir;
    out->pba_offset = pba_off_bir & ~7u;
    return true;
}

void msi_encode_entry(uint32_t vector, uint32_t apic_id, uint32_t entry[4]) {
    /* x86 MSI(-X) message: fixed 0xFEE00xxx LAPIC address, destination in bits
     * 12..19, then the 8-bit vector as message data, entry unmasked. */
    entry[0] = 0xFEE00000u | ((apic_id & 0xffu) << 12);
    entry[1] = 0;
    entry[2] = vector & 0xffu;
    entry[3] = 0;
}

#ifndef ARK_MSI_HOST_TEST

#include "pci.h"
#include "ioapic.h"
#include "mmio.h"

#define MSIX_CAP_ID 0x11u
#define MSI_SLOTS (MSI_VECTOR_LAST - MSI_VECTOR_FIRST + 1u)
#define MSI_BDF_FREE 0xffffffffu

typedef struct {
    uint32_t bdf;    /* MSI_BDF_FREE = slot unused */
    unsigned entry;  /* MSI-X table entry index programmed */
    int owner;       /* module slot, or -1 for kernel-owned */
    void (*isr)(void);
    uint64_t count;
} MsiSlot;

static MsiSlot msi_slots[MSI_SLOTS];
static bool msi_ready;

static void msi_ensure(void) {
    if (msi_ready)
        return;
    for (unsigned i = 0; i < MSI_SLOTS; i++)
        msi_slots[i].bdf = MSI_BDF_FREE;
    msi_ready = true;
}

static unsigned msi_index(unsigned vector) {
    return vector - MSI_VECTOR_FIRST;
}

bool msi_is_vector(unsigned vector) {
    if (vector < MSI_VECTOR_FIRST || vector > MSI_VECTOR_LAST)
        return false;
    /* The reserved slot must fall through to the INT80 syscall path. */
    return vector != MSI_VECTOR_RESERVED;
}

int msi_vector_owner(unsigned vector) {
    if (!msi_ready || !msi_is_vector(vector))
        return -1;
    MsiSlot *s = &msi_slots[msi_index(vector)];
    return s->bdf == MSI_BDF_FREE ? -1 : s->owner;
}

void (*msi_vector_isr(unsigned vector))(void) {
    if (!msi_ready || !msi_is_vector(vector))
        return 0;
    return msi_slots[msi_index(vector)].isr;
}

void msi_vector_note(unsigned vector) {
    if (msi_ready && msi_is_vector(vector))
        msi_slots[msi_index(vector)].count++;
}

unsigned msi_owner_vectors(int owner) {
    unsigned n = 0;
    for (unsigned i = 0; i < MSI_SLOTS; i++)
        if (msi_slots[i].bdf != MSI_BDF_FREE && msi_slots[i].owner == owner)
            n++;
    return n;
}

bool msi_owner_vector_at(int owner, unsigned index, unsigned *vector, uint64_t *count) {
    for (unsigned i = 0; i < MSI_SLOTS; i++) {
        if (msi_slots[i].bdf == MSI_BDF_FREE || msi_slots[i].owner != owner)
            continue;
        if (index--)
            continue;
        if (vector)
            *vector = MSI_VECTOR_FIRST + i;
        if (count)
            *count = msi_slots[i].count;
        return true;
    }
    return false;
}

/* Resolve a function's MSI-X table to a mapped kernel alias. Fills cap_out
 * (capability offset) and info; returns NULL when the function has no usable
 * MSI-X capability, BAR or table mapping. */
static volatile uint32_t *msix_resolve(uint32_t bdf, unsigned *cap_out, MsixInfo *info) {
    unsigned cap;
    if (!pci_capability(bdf, MSIX_CAP_ID, &cap))
        return 0;
    uint16_t control = (uint16_t)pci_read(bdf, cap + 2);
    if (!msi_decode_msix(control, pci_read(bdf, cap + 4), pci_read(bdf, cap + 8), info))
        return 0;
    uint64_t base;
    if (!pci_bar_base(bdf, info->table_bir, &base))
        return 0;
    if (info->table_offset > UINT64_MAX - base)
        return 0;
    uint64_t physical = base + info->table_offset;
    if (!physical ||
        !platform_map_mmio(physical, (uint64_t)info->table_size * MSI_ENTRY_BYTES))
        return 0;
    if (cap_out)
        *cap_out = cap;
    return (volatile uint32_t *)(uintptr_t)physical;
}

static void msix_mask_entry(uint32_t bdf, unsigned entry) {
    MsixInfo info;
    volatile uint32_t *table = msix_resolve(bdf, 0, &info);
    if (!table || entry >= info.table_size)
        return;
    table[entry * 4 + 3] = 1; /* vector control: mask */
}

int msi_attach(uint32_t bdf, void (*isr)(void), int owner, uint32_t *vector_out) {
    if (!isr || !vector_out)
        return -22;
    msi_ensure();
    uint32_t apic_id;
    if (!ioapic_lapic_destination(&apic_id))
        return -19; /* no LAPIC: MSI cannot be delivered */
    unsigned cap;
    MsixInfo info;
    volatile uint32_t *table = msix_resolve(bdf, &cap, &info);
    if (!table)
        return -19;
    /* Lowest table entry not already in use by this function. */
    int entry = -1;
    for (unsigned e = 0; e < info.table_size && entry < 0; e++) {
        bool taken = false;
        for (unsigned i = 0; i < MSI_SLOTS; i++)
            if (msi_slots[i].bdf == bdf && msi_slots[i].entry == e) {
                taken = true;
                break;
            }
        if (!taken)
            entry = (int)e;
    }
    if (entry < 0)
        return -16;
    int slot = -1;
    for (unsigned i = 0; i < MSI_SLOTS; i++)
        if (msi_slots[i].bdf == MSI_BDF_FREE) {
            slot = (int)i;
            break;
        }
    if (slot < 0)
        return -16;
    uint32_t vector = MSI_VECTOR_FIRST + (uint32_t)slot;
    uint32_t words[4];
    msi_encode_entry(vector, apic_id, words);
    volatile uint32_t *e = &table[(unsigned)entry * 4];
    e[3] = 1; /* mask while reprogramming so no half-written entry fires */
    __asm__ volatile("" ::: "memory");
    e[0] = words[0];
    e[1] = words[1];
    e[2] = words[2];
    __asm__ volatile("" ::: "memory");
    e[3] = words[3]; /* unmask */
    /* Enable MSI-X and clear the function mask; disable INTx so the same
     * function cannot also assert its legacy line. */
    pci_write16(bdf, cap + 2, (uint16_t)((info.control & ~0x4000u) | 0x8000u));
    pci_command(bdf, 0x0400u);
    msi_slots[slot].bdf = bdf;
    msi_slots[slot].entry = (unsigned)entry;
    msi_slots[slot].owner = owner;
    msi_slots[slot].isr = isr;
    msi_slots[slot].count = 0;
    *vector_out = vector;
    return 0;
}

void msi_release(int owner) {
    msi_ensure();
    for (unsigned i = 0; i < MSI_SLOTS; i++) {
        if (msi_slots[i].bdf == MSI_BDF_FREE || msi_slots[i].owner != owner)
            continue;
        msix_mask_entry(msi_slots[i].bdf, msi_slots[i].entry);
        msi_slots[i].bdf = MSI_BDF_FREE;
        msi_slots[i].isr = 0;
        msi_slots[i].owner = -1;
        msi_slots[i].count = 0;
    }
}

#endif

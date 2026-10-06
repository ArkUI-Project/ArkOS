#ifndef ARK_MSI_H
#define ARK_MSI_H
/* Dynamic MSI/MSI-X interrupt vectors.
 *
 * PCI MSI-X (capability id 0x11) is the only interrupt path QEMU's nvme device
 * offers, and the 13700H track needs it for storage. The kernel owns the
 * capability: a driver asks msi_attach for an interrupt and gets back the IDT
 * vector the kernel programmed into the device's MSI-X table. Drivers never
 * touch the table themselves, so a module cannot redirect another function's
 * interrupts.
 *
 * Vectors are allocated from 0x40..0xEF, clear of the legacy ISA window
 * (32..47), the SMP timer (48), the INT80 syscall gate (128), the IPI (240)
 * and the spurious vector (255). One owner per vector; releasing an owner
 * masks the device entry before the vector is reused. Delivery always goes
 * through the BSP LAPIC, so MSI is only available when the MADT brought the
 * LAPIC up (see ioapic_lapic_destination).
 *
 * Only the pure decode/encode helpers are exercised by the host test; the
 * runtime half is compiled out under ARK_MSI_HOST_TEST. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MSI_VECTOR_FIRST 0x40u
#define MSI_VECTOR_LAST 0xEFu
#define MSI_VECTOR_RESERVED 0x80u /* INT80 syscall gate: never an MSI target */
#define MSI_ENTRY_BYTES 16u
#define MSI_TABLE_MAX 2048u

/* Decoded MSI-X capability. table_size is the number of 16-byte table entries
 * (1..2048); table_offset/pba_offset are byte offsets from the base of the BAR
 * named by table_bir/pba_bir. */
typedef struct {
    uint16_t control;      /* raw Message Control word (cap + 2) */
    unsigned table_size;   /* (control & 0x7ff) + 1 */
    unsigned table_bir;    /* BAR holding the MSI-X table (0..5, or 0..5 for ROM) */
    uint32_t table_offset; /* 8-byte aligned byte offset inside that BAR */
    unsigned pba_bir;
    uint32_t pba_offset;
} MsixInfo;

/* Decodes the three capability words. Returns false on a null out pointer or a
 * table/PBA BIR outside BARs 0..5 (6 and 7 are reserved). */
bool msi_decode_msix(uint16_t message_control, uint32_t table_off_bir,
                     uint32_t pba_off_bir, MsixInfo *out);
/* Encodes one MSI-X table entry as four little-endian dwords: LAPIC address,
 * address high, message data (the vector) and an unmasked vector control. */
void msi_encode_entry(uint32_t vector, uint32_t apic_id, uint32_t entry[4]);

#ifndef ARK_MSI_HOST_TEST
/* Allocate one vector and program the device's MSI-X table entry. owner is the
 * module slot (or -1 for kernel use). Returns 0 and the vector, or:
 * -12 MMIO map failed, -16 no free vector / table entry, -19 no usable MSI-X
 * capability, BAR or LAPIC, -22 malformed input. */
int msi_attach(uint32_t bdf, void (*isr)(void), int owner, uint32_t *vector_out);
/* Mask and free every vector owned by owner. Safe on an unknown owner. */
void msi_release(int owner);
/* True for the whole reserved MSI window except the INT80 gate, so dispatch
 * can complete the LAPIC EOI even for an unallocated (stray) vector. */
bool msi_is_vector(unsigned vector);
int msi_vector_owner(unsigned vector);        /* -1 when the vector is free */
void (*msi_vector_isr(unsigned vector))(void);
void msi_vector_note(unsigned vector);        /* bump the ISR counter */
/* Iterate one owner's live vectors in allocation order: index 0.. returns true
 * and fills vector/count; false past the last. */
bool msi_owner_vector_at(int owner, unsigned index, unsigned *vector, uint64_t *count);
unsigned msi_owner_vectors(int owner);
#endif
#endif

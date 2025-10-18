// SPDX-License-Identifier: GPL-2.0

/*
 * GIC-400 support library for Amiga OS running on Emu68/Raspberry Pi 4B
 * This is intended to handle SPIs only and is supposed to coexist with Emu68.
 * For start, well assume that:
 * - all SPIs are level-triggered, active high
 * - all SPIs are in Group 0
 * - all SPIs are routed to CPU0 (where 68k emulation runs)
 * - secure/non-secure access is of no concern
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/devicetree_protos.h>
#else
#include <proto/exec.h>
#include <proto/devicetree.h>
#endif

#include <exec/types.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>

#include <devtree.h>
#include <debug.h>
#include <compat.h>
#include <gic400.h>

#define GIC_ERROR 1

#define GIC_MAX_REGISTERED_IRQS 16U

extern struct ExecBase *SysBase;

static const char gic_dispatcher_name[] = "gic400.dispatcher";

struct gic_irq_handler
{
    ULONG irq;
    struct Interrupt *interrupt;
};

struct GICD_IIDR_bits
{
    ULONG product_id : 4;   // [31:24] GIC product identifier (0x02)
    ULONG reserved : 4;     // [23:20] Reserved, RAZ
    ULONG variant : 4;      // [19:16] GIC variant number
    ULONG revision : 4;     // [15:12] GIC revision number
    ULONG implementer : 12; // [11:0] Implementer code, 0x43B for ARM
};

struct GICD_TYPER_bits
{
    ULONG it_lines_number : 5; // [4:0] ITLinesNumber, number of interrupt lines / 32 - 1
    ULONG cpus_number : 3;     // [7:5] CPUNumber, number of CPU interfaces / 2 - 1
    ULONG reserved1 : 2;       // [9:8] Reserved, RAZ
    ULONG security_extn : 1;   // [10] Security Extensions, 1 if supported
    ULONG lspi : 5;            // [15:11] number of LSPIs
    ULONG reserved2 : 16;      // [31:16] Reserved, RAZ
};

struct GICC_IIDR_bits
{
    ULONG product_id : 12;  // [31:20] GIC product identifier (0x02)
    ULONG architecture : 4; // [19:16] Architecture version (0x2 for GICv2)
    ULONG revision : 4;     // [15:12] GIC revision number
    ULONG implementer : 12; // [11:0] Implementer code, 0x43B for ARM
};

struct GICC_CTLR_bits
{
    ULONG enable_grp0 : 1;         // [0] Enable Group 0
    ULONG enable_grp1 : 1;         // [1] Enable Group 1
    ULONG ack_ctl : 1;             // [2] FIQ or IRQ interrupt acknowledge (recommended 0)
    ULONG fiq_en : 1;              // [3] FIQ enable for group 0
    ULONG cbpr : 1;                // [4] Complete before priority change (0 GICC_BPR for group0 GICC_ABPR for group1, 1 only GICC_BPR for both groups)
    ULONG fiq_bypass_dis_grp0 : 1; // [5] Disable bypassing of FIQ for Group 0
    ULONG irq_bypass_dis_grp0 : 1; // [6] Disable bypassing of IRQ for Group 0
    ULONG fiq_bypass_dis_grp1 : 1; // [7] Disable bypassing of FIQ for Group 1
    ULONG irq_bypass_dis_grp1 : 1; // [8] Disable bypassing of IRQ for Group 1
    ULONG eoi_mode_s : 1;          // [9] EOI mode for secure interrupts
    ULONG eoi_mode_ns : 1;         // [10] EOI mode for non-secure interrupts (0 GICC_EOIR does both priority drop and deactivate, 1 only priority drop)
    ULONG reserved : 21;           // [31:11] Reserved, RAZ
};

static struct
{
    struct GICD_IIDR_bits gicd_iidr;
    struct GICD_TYPER_bits gicd_typer;
    struct GICC_IIDR_bits gicc_iidr;

    APTR gic_base;
    ULONG max_irqs;

    struct gic_irq_handler handlers[GIC_MAX_REGISTERED_IRQS];
    ULONG handler_count;

    struct Interrupt dispatcher_interrupt;
} gic_data;

static ULONG gic400_exec_dispatcher(void);

static inline BOOL gicd_irq_valid(ULONG irq)
{
    return gic_data.max_irqs > 0 && irq < gic_data.max_irqs;
}

/* Distributor */
#define GICD_BASE 0x1000
#define GICD_CTLR (gic_data.gic_base + GICD_BASE + 0x000)                    // Distributor Control Register
#define GICD_TYPER (gic_data.gic_base + GICD_BASE + 0x004)                   // Interrupt Controller Type Register
#define GICD_IIDR (gic_data.gic_base + GICD_BASE + 0x008)                    // Distributor Implementer ID Register
#define GICD_IGROUPR(n) (gic_data.gic_base + GICD_BASE + 0x080 + (n) * 4)    // Interrupt Group Registers
#define GICD_ISENABLER(n) (gic_data.gic_base + GICD_BASE + 0x100 + (n) * 4)  // Interrupt Set-Enable Registers
#define GICD_ICENABLER(n) (gic_data.gic_base + GICD_BASE + 0x180 + (n) * 4)  // Interrupt Clear-Enable Registers
#define GICD_ISPENDR(n) (gic_data.gic_base + GICD_BASE + 0x200 + (n) * 4)    // Interrupt Set-Pending Registers
#define GICD_ICPENDR(n) (gic_data.gic_base + GICD_BASE + 0x280 + (n) * 4)    // Interrupt Clear-Pending Registers
#define GICD_ISACTIVER(n) (gic_data.gic_base + GICD_BASE + 0x300 + (n) * 4)  // Interrupt Set-Active Registers
#define GICD_ICACTIVER(n) (gic_data.gic_base + GICD_BASE + 0x380 + (n) * 4)  // Interrupt Clear-Active Registers
#define GICD_IPRIORITYR(n) (gic_data.gic_base + GICD_BASE + 0x400 + (n) * 4) // Interrupt Priority Registers
#define GICD_ITARGETSR(n) (gic_data.gic_base + GICD_BASE + 0x800 + (n) * 4)  // Interrupt Processor Targets Registers
#define GICD_ICFGR(n) (gic_data.gic_base + GICD_BASE + 0xC00 + (n) * 4)      // Interrupt Configuration Registers
#define GICD_SPISR(n) (gic_data.gic_base + GICD_BASE + 0xD04 + (n) * 4)      // Shared Peripheral Interrupt Status Registers
#define GICD_COMPONENT_ID (gic_data.gic_base + GICD_BASE + 0xFF0)            // Component ID Register
#define GICD_PERIPHERAL_ID (gic_data.gic_base + GICD_BASE + 0xFE0)           // Peripheral ID Register

/* gicd_print_info: Log distributor ID and capability registers.
 * Args: none.
 * Returns: void.
 */
static void gicd_print_info(void)
{
    struct GICD_TYPER_bits typer;
    *(ULONG *)&typer = readl(GICD_TYPER);
    struct GICD_IIDR_bits iidr;
    *(ULONG *)&iidr = readl(GICD_IIDR);

    Kprintf("[gic] GICD_IIDR=0x%08lx, Implementer=0x%03lx, Revision=%ld, Variant=%ld, ProductID=0x%02lx\n",
            *(ULONG *)&iidr, iidr.implementer, iidr.revision, iidr.variant, iidr.product_id);
    Kprintf("[gic] GICD_TYPER=0x%08lx, ITLinesNumber=%ld, CPUNumber=%ld, SecurityExtensions=%ld, LSPIs=%ld\n",
            *(ULONG *)&typer, (typer.it_lines_number + 1) * 32, typer.cpus_number + 1, typer.security_extn, typer.lspi);
}

/* gicd_is_gic_v2: Verify component and peripheral IDs match GICv2.
 * Args: none.
 * Returns: TRUE when IDs match GICv2, otherwise FALSE.
 */
static BOOL gicd_is_gic_v2(void)
{
    ULONG component_id = readl(GICD_COMPONENT_ID);
    ULONG peripheral_id = readl(GICD_PERIPHERAL_ID);

    if (component_id != 0x0df005b1)
    {
        Kprintf("[gic] Unknown GIC component ID: 0x%04lx\n", component_id);
        return FALSE;
    }

    if (peripheral_id != 0x90b42b00)
    {
        Kprintf("[gic] Unknown GIC peripheral ID: 0x%04lx\n", peripheral_id);
        return FALSE;
    }
    return TRUE;
}

/* gicd_enable_group: Enable a distributor interrupt group bit.
 * Args: group - group index (0 or 1).
 * Returns: void.
 */
static inline void gicd_enable_group(ULONG group)
{
    if (group > 1)
        return;

    // set enable bit in GICD_CTLR
    ULONG reg = readl(GICD_CTLR);
    reg |= (1 << group);
    writel(reg, GICD_CTLR);
}

/* gicd_disable_group: Disable a distributor interrupt group bit.
 * Args: group - group index (0 or 1).
 * Returns: void.
 */
static inline void gicd_disable_group(ULONG group)
{
    if (group > 1)
        return;

    // clear enable bit in GICD_CTLR
    ULONG reg = readl(GICD_CTLR);
    reg &= ~(1 << group);
    writel(reg, GICD_CTLR);
}

/* gicd_get_irq_status: Report SPI active status from SPISR.
 * Args: irq - interrupt number.
 * Returns: TRUE when pending in SPISR, otherwise FALSE.
 */
static inline BOOL gicd_get_irq_status(ULONG irq)
{
    if (!gicd_irq_valid(irq))
        return FALSE;

    // read SPI status from GICD_SPISR
    if (irq < 32)
        return FALSE; // SGI and PPI are not handled here

    ULONG spi_irq = irq - 32;
    ULONG reg_index = spi_irq >> 5;
    ULONG bit_offset = spi_irq & 0x1F;
    ULONG reg = readl(GICD_SPISR(reg_index));
    return (reg & (1 << bit_offset)) != 0;
}

/* gicd_is_pending: Check pending bit for an IRQ in ISPENDR.
 * Args: irq - interrupt number.
 * Returns: TRUE when the pending bit is set, otherwise FALSE.
 */
static inline BOOL gicd_is_pending(ULONG irq)
{
    if (!gicd_irq_valid(irq))
        return FALSE;

    // read pending status from GICD_ISPENDR
    ULONG reg_index = irq >> 5;
    ULONG bit_offset = irq & 0x1F;
    ULONG reg = readl(GICD_ISPENDR(reg_index));
    return (reg & (1UL << bit_offset)) != 0;
}

/* gicd_is_active: Check active bit for an IRQ in ISACTIVER.
 * Args: irq - interrupt number.
 * Returns: TRUE when the active bit is set, otherwise FALSE.
 */
static inline BOOL gicd_is_active(ULONG irq)
{
    if (!gicd_irq_valid(irq))
        return FALSE;

    // read active status from GICD_ISACTIVER
    ULONG reg_index = irq >> 5;
    ULONG bit_offset = irq & 0x1F;
    ULONG reg = readl(GICD_ISACTIVER(reg_index));
    return (reg & (1UL << bit_offset)) != 0;
}

/* gicd_is_enabled: Check enable bit for an IRQ in ISENABLER.
 * Args: irq - interrupt number.
 * Returns: TRUE when enabled, otherwise FALSE.
 */
static inline BOOL gicd_is_enabled(ULONG irq)
{
    if (!gicd_irq_valid(irq))
        return FALSE;

    // read enabled status from GICD_ISENABLER
    ULONG reg_index = irq >> 5;
    ULONG bit_offset = irq & 0x1F;
    ULONG reg = readl(GICD_ISENABLER(reg_index));
    return (reg & (1UL << bit_offset)) != 0;
}

/* gicd_enable_irq: Set enable bit for an IRQ in ISENABLER.
 * Args: irq - interrupt number.
 * Returns: void.
 */
static inline void gicd_enable_irq(ULONG irq)
{
    if (!gicd_irq_valid(irq))
        return;

    // set enable bit in GICD_ISENABLER
    ULONG reg_index = irq >> 5;
    ULONG bit_offset = irq & 0x1F;
    writel(1UL << bit_offset, GICD_ISENABLER(reg_index));
}

/* gicd_disable_irq: Clear enable bit for an IRQ via ICENABLER.
 * Args: irq - interrupt number.
 * Returns: void.
 */
static inline void gicd_disable_irq(ULONG irq)
{
    if (!gicd_irq_valid(irq))
        return;

    // set disable bit in GICD_ICENABLER
    ULONG reg_index = irq >> 5;
    ULONG bit_offset = irq & 0x1F;
    writel(1UL << bit_offset, GICD_ICENABLER(reg_index));
}

/* gicd_get_priority: Fetch per-IRQ priority value.
 * Args: irq - interrupt number.
 * Returns: priority byte read from IPRIORITYR.
 */
static inline UBYTE gicd_get_priority(ULONG irq)
{
    if (!gicd_irq_valid(irq))
        return 0;

    // read priority from GICD_IPRIORITYR
    ULONG reg_index = irq >> 2;
    ULONG byte_offset = irq & 0x03;
    ULONG reg = readl(GICD_IPRIORITYR(reg_index));
    return (reg >> (byte_offset * 8)) & 0xFF;
}

/* gicd_set_priority: Program per-IRQ priority value.
 * Args: irq - interrupt number; priority - byte to store.
 * Returns: void.
 */
static inline void gicd_set_priority(ULONG irq, UBYTE priority)
{
    if (!gicd_irq_valid(irq))
        return;

    // write priority to GICD_IPRIORITYR
    ULONG reg_index = irq >> 2;
    ULONG byte_offset = irq & 0x03;
    ULONG reg = readl(GICD_IPRIORITYR(reg_index));
    reg &= ~(0xFF << (byte_offset * 8));
    reg |= (priority & 0xFF) << (byte_offset * 8);
    writel(reg, GICD_IPRIORITYR(reg_index));
}

/* gicd_is_cpu_enabled: Check CPU target bit for an IRQ.
 * Args: irq - interrupt number; cpu - CPU index.
 * Returns: TRUE when the CPU is targeted, otherwise FALSE.
 */
static inline BOOL gicd_is_cpu_enabled(ULONG irq, UBYTE cpu)
{
    if (!gicd_irq_valid(irq) || cpu >= 8)
        return FALSE;

    // read target from GICD_ITARGETSR and check if cpu bit is set
    if (irq < 32)
        return FALSE; // SGI and PPI are not handled here

    ULONG reg_index = irq >> 2;
    ULONG byte_offset = irq & 0x03;
    ULONG reg = readl(GICD_ITARGETSR(reg_index));
    UBYTE target = (reg >> (byte_offset * 8)) & 0xFF;
    return (target & (1 << cpu)) != 0;
}

/* gicd_set_cpu: Set or clear CPU target bit for an IRQ.
 * Args: irq - interrupt number; cpu - CPU index; enable - TRUE to set.
 * Returns: void.
 */
static inline void gicd_set_cpu(ULONG irq, UBYTE cpu, BOOL enable)
{
    if (!gicd_irq_valid(irq) || cpu >= 8)
        return;

    // write target to GICD_ITARGETSR
    if (irq < 32)
        return; // SGI and PPI are not handled here

    ULONG reg_index = irq >> 2;
    ULONG byte_offset = irq & 0x03;
    ULONG reg = readl(GICD_ITARGETSR(reg_index));
    UBYTE target = (reg >> (byte_offset * 8)) & 0xFF;
    if (enable)
        target |= (1 << cpu);
    else
        target &= ~(1 << cpu);
    reg &= ~(0xFF << (byte_offset * 8));
    reg |= (target & 0xFF) << (byte_offset * 8);
    writel(reg, GICD_ITARGETSR(reg_index));
}

/* gicd_get_group: Read group assignment for an IRQ.
 * Args: irq - interrupt number.
 * Returns: 0 for group 0, 1 for group 1.
 */
static inline UBYTE gicd_get_group(ULONG irq)
{
    if (!gicd_irq_valid(irq))
        return 0;

    // read group from GICD_IGROUPR
    ULONG reg_index = irq >> 5;
    ULONG bit_offset = irq & 0x1F;
    ULONG reg = readl(GICD_IGROUPR(reg_index));
    return (reg >> bit_offset) & 0x01;
}

/* gicd_set_group: Assign an IRQ to group 0 or 1.
 * Args: irq - interrupt number; group - desired group value.
 * Returns: void.
 */
static inline void gicd_set_group(ULONG irq, UBYTE group)
{
    if (!gicd_irq_valid(irq) || group > 1)
        return;

    // write group to GICD_IGROUPR
    ULONG reg_index = irq >> 5;
    ULONG bit_offset = irq & 0x1F;
    ULONG reg = readl(GICD_IGROUPR(reg_index));
    if (group)
        reg |= (1UL << bit_offset);
    else
        reg &= ~(1UL << bit_offset);
    writel(reg, GICD_IGROUPR(reg_index));
}

/* gicd_set_trigger: Configure trigger mode for an IRQ.
 * Args: irq - interrupt number; edge - TRUE for edge triggered.
 * Returns: void.
 */
static inline void gicd_set_trigger(ULONG irq, BOOL edge)
{
    if (!gicd_irq_valid(irq))
        return;

    // write trigger mode to GICD_ICFGR
    if (irq < 16)
        return; // SGIs have fixed configuration

    ULONG reg_index = irq >> 4;
    ULONG bit_offset = (irq & 0x0F) * 2;
    ULONG reg = readl(GICD_ICFGR(reg_index));
    if (edge)
        reg |= (2UL << bit_offset); // 10b for edge-triggered
    else
        reg &= ~(2UL << bit_offset); // 00b for level-triggered
    writel(reg, GICD_ICFGR(reg_index));
}

/* CPU Interface */
#define GICC_BASE 0x2000
#define GICC_CTLR (gic_data.gic_base + GICC_BASE + 0x000) // CPU Interface Control Register
#define GICC_PMR (gic_data.gic_base + GICC_BASE + 0x004)  // Interrupt Priority Mask Register

#define GICC_BPR (gic_data.gic_base + GICC_BASE + 0x008)   // Binary Point Register
#define GICC_IAR (gic_data.gic_base + GICC_BASE + 0x00C)   // Interrupt Acknowledge Register
#define GICC_EOIR (gic_data.gic_base + GICC_BASE + 0x010)  // End of Interrupt Register
#define GICC_RPR (gic_data.gic_base + GICC_BASE + 0x014)   // Running Priority Register
#define GICC_HPPIR (gic_data.gic_base + GICC_BASE + 0x018) // Highest Priority Pending Interrupt Register

#define GICC_ABPR (gic_data.gic_base + GICC_BASE + 0x01C)   // Aliased Binary Point Register
#define GICC_AIAR (gic_data.gic_base + GICC_BASE + 0x020)   // Aliased Interrupt Acknowledge Register
#define GICC_AEOIR (gic_data.gic_base + GICC_BASE + 0x024)  // Aliased End of Interrupt Register
#define GICC_AHPPIR (gic_data.gic_base + GICC_BASE + 0x028) // Aliased Highest Priority Pending Interrupt Register

#define GICC_APR(n) (gic_data.gic_base + GICC_BASE + 0x0D0 + (n) * 4)   // Active Priority Register
#define GICC_NSAPR(n) (gic_data.gic_base + GICC_BASE + 0x0E0 + (n) * 4) // Non-secure Active Priority Register
#define GICC_IIDR (gic_data.gic_base + GICC_BASE + 0x0FC)               // CPU Interface Implementer Identification Register
#define GICC_DIR (gic_data.gic_base + GICC_BASE + 0x1000)               // Deactivate Interrupt Register

/* gicc_print_info: Log CPU interface identification details.
 * Args: none.
 * Returns: void.
 */
static void gicc_print_info(void)
{
    struct GICC_IIDR_bits iidr;
    *(ULONG *)&iidr = readl(GICC_IIDR);

    Kprintf("[gic] GICC_IIDR=0x%08lx, Implementer=0x%03lx, Revision=%ld, Architecture=%ld, ProductID=0x%03lx\n",
            *(ULONG *)&iidr, iidr.implementer, iidr.revision, iidr.architecture, iidr.product_id);
}

/* gicc_set_ctlr: Write CPU interface control register.
 * Args: ctlr - bitfield structure with desired settings.
 * Returns: void.
 */
static inline void gicc_set_ctlr(struct GICC_CTLR_bits ctlr)
{
    writel(*(ULONG *)&ctlr, GICC_CTLR);
}

/* gicc_get_ctlr: Read CPU interface control register.
 * Args: ctlr - destination pointer for control bits.
 * Returns: void.
 */
static inline void gicc_get_ctlr(struct GICC_CTLR_bits *ctlr)
{
    if (!ctlr)
        return;

    *(ULONG *)ctlr = readl(GICC_CTLR);
}

/* gicc_set_priority_mask: Write priority mask threshold.
 * Args: priority - mask value to store.
 * Returns: void.
 */
static inline void gicc_set_priority_mask(UBYTE priority)
{
    writel(priority, GICC_PMR);
}

/* gicc_get_priority_mask: Read priority mask threshold.
 * Args: priority - destination pointer for mask value.
 * Returns: void.
 */
static inline void gicc_get_priority_mask(UBYTE *priority)
{
    if (!priority)
        return;

    *priority = readl(GICC_PMR) & 0xFF;
}

/* gicc_acknowledge_interrupt_group: Read pending IRQ ID from IAR.
 * Reads the interrupt ID of the highest priority pending interrupt from GICC_IAR (or GICC_AIAR for group 1).
 * The ID is in bits [9:0], bits [31:10] are reserved and RAZ.
 * If no interrupt is pending, the ID is 1023 (0x3FF).
 * If the ID is 1022 (0x3FE), it indicates a spurious interrupt.
 * The ID must be written to GICC_EOIR when the interrupt is handled.
 * Args: group - 0 for secure group, 1 for non-secure group.
 * Returns: raw value from the acknowledge register.
 */
static inline ULONG gicc_acknowledge_interrupt_group(UBYTE group)
{
    if (group > 1)
        return 0x3FF;

    if (group == 0)
    {
        return readl(GICC_IAR);
    }
    else
    {
        return readl(GICC_AIAR);
    }
}

/* gicc_end_interrupt_group: Signal completion of an IRQ.
 * Ends the interrupt by writing the interrupt ID to GICC_EOIR (or GICC_AEOIR for group 1).
 * if EIOmode is 0 - this drops the priority and deactivates the interrupt
 * if EIOmode is 1 - this only drops the priority
 * Args: irq - raw value previously read from IAR; group - 0 or 1.
 * Returns: void.
 */
static inline void gicc_end_interrupt_group(ULONG irq, UBYTE group)
{
    if (group > 1)
        return;

    if (group == 0)
    {
        writel(irq, GICC_EOIR);
    }
    else
    {
        writel(irq, GICC_AEOIR);
    }
}

/* gicc_deactivate_interrupt: Explicitly deactivate an IRQ via DIR.
 * Args: irq - raw value for the IRQ to deactivate.
 * Returns: void.
 */
static inline void gicc_deactivate_interrupt(ULONG irq)
{
    writel(irq, GICC_DIR);
}

/* gicc_get_running_priority: Obtain current running priority.
 * Args: none.
 * Returns: 8-bit priority value.
 */
static inline ULONG gicc_get_running_priority(void)
{
    return readl(GICC_RPR) & 0xFF;
}

/* gicc_get_highest_pending_group: Fetch highest pending IRQ number.
 * Args: group - 0 for secure, 1 for non-secure group view.
 * Returns: pending IRQ ID masked to 10 bits.
 */
static inline ULONG gicc_get_highest_pending_group(UBYTE group)
{
    if (group > 1)
        return 0x3FF;

    if (group == 0)
    {
        return readl(GICC_HPPIR) & 0x3FF;
    }
    else
    {
        return readl(GICC_AHPPIR) & 0x3FF;
    }
}

APTR DeviceTreeBase;

static int gic400_parse_devicetree()
{
	DT_Init();

    APTR root_key = DT_OpenKey((CONST_STRPTR) "/");
    if (root_key == NULL)
    {
        Kprintf("[gic] %s: Failed to open root key\n", __func__);
        return -GIC_ERROR;
    }

    const ULONG gic_phandle = DT_GetPropertyValueULONG(root_key, "interrupt-controller", 1, FALSE);

    APTR gic_key = DT_FindByPHandle(root_key, gic_phandle);
    if (gic_key == NULL)
    {
        Kprintf("[gic] %s: Failed to find GIC key for handle %08lx\n", __func__, gic_phandle);
        DT_CloseKey(root_key);
        return -GIC_ERROR;
    }

    CONST_STRPTR gic_compatible = DT_GetPropValue(DT_FindProperty(gic_key, (CONST_STRPTR) "compatible"));
    
    const APTR parent_key = DT_GetParent(gic_key);
	const ULONG address_cells_parent = DT_GetPropertyValueULONG(parent_key, "#address-cells", 1, FALSE);
	gic_data.gic_base = (APTR)(ULONG)DT_GetNumber(DT_GetPropValue(DT_FindProperty(gic_key, (CONST_STRPTR) "reg")), address_cells_parent);
	DT_TranslateAddress(&gic_data.gic_base, parent_key);
    if (gic_data.gic_base == NULL)
    {
        Kprintf("[gic] %s: Failed to get base address for GIC\n", __func__);
        DT_CloseKey(gic_key);
        DT_CloseKey(root_key);
        return -GIC_ERROR;
    }

	Kprintf("[genet] %s: compatible: %s\n", __func__, gic_compatible);
	Kprintf("[genet] %s: register base: %08lx\n", __func__, gic_data.gic_base);

	// We're done with the device tree
	DT_CloseKey(gic_key);
	DT_CloseKey(root_key);
	return 0;
}

/* gic400_init: Initialize GIC state and install dispatcher.
 * Args: base - physical base address shared with Emu68.
 * Returns: 0 on success, -GIC_ERROR on failure.
 */
int gic400_init(ULONG base)
{
    int ret = gic400_parse_devicetree();
    if (ret < 0)
        return ret;

    // Initialize GICD
    ULONG temp_iidr = readl(GICD_IIDR);
    gic_data.gicd_iidr = *(struct GICD_IIDR_bits *)&temp_iidr;
    ULONG temp_typer = readl(GICD_TYPER);
    gic_data.gicd_typer = *(struct GICD_TYPER_bits *)&temp_typer;
    ULONG temp_gicc_iidr = readl(GICC_IIDR);
    gic_data.gicc_iidr = *(struct GICC_IIDR_bits *)&temp_gicc_iidr;

    gic_data.max_irqs = (gic_data.gicd_typer.it_lines_number + 1) * 32;

    gic_data.handler_count = 0;
    for (ULONG i = 0; i < GIC_MAX_REGISTERED_IRQS; ++i)
    {
        gic_data.handlers[i].irq = (ULONG)-1;
        gic_data.handlers[i].interrupt = NULL;
    }

    if (!gicd_is_gic_v2())
    {
        Kprintf("[gic] GIC is not GICv2, aborting initialization\n");
        return -GIC_ERROR;
    }

    gicc_print_info();
    gicd_print_info();

    gicd_enable_group(0);         // enable group 0
    gicc_set_priority_mask(0xFF); // allow all priorities

    struct GICC_CTLR_bits ctlr;
    gicc_get_ctlr(&ctlr);
    ctlr.ack_ctl = 0;             // FIQ or IRQ interrupt acknowledge (recommended 0)
    ctlr.cbpr = 1;                // only GICC_BPR for both groups
    ctlr.eoi_mode_s = 0;          // GICC_EOIR does both priority drop and deactivate
    ctlr.eoi_mode_ns = 0;         // GICC_EOIR does both priority drop and deactivate
    ctlr.enable_grp0 = 1;         // enable group 0
    ctlr.enable_grp1 = 0;         // disable group 1
    ctlr.fiq_en = 0;              // disable FIQ for group 0
    ctlr.fiq_bypass_dis_grp0 = 1; // disable bypassing of FIQ for Group 0
    ctlr.irq_bypass_dis_grp0 = 1; // disable bypassing of IRQ for Group 0
    ctlr.fiq_bypass_dis_grp1 = 1; // disable bypassing of FIQ for Group 1
    ctlr.irq_bypass_dis_grp1 = 1; // disable bypassing of IRQ for Group 1
    gicc_set_ctlr(ctlr);

    gic_data.dispatcher_interrupt.is_Node.ln_Type = NT_INTERRUPT;
    gic_data.dispatcher_interrupt.is_Node.ln_Pri = 100;
    gic_data.dispatcher_interrupt.is_Node.ln_Name = (STRPTR)gic_dispatcher_name;
    gic_data.dispatcher_interrupt.is_Data = NULL;
    gic_data.dispatcher_interrupt.is_Code = gic400_exec_dispatcher;
    AddIntServer(INTB_EXTER, &gic_data.dispatcher_interrupt);
    Kprintf("[gic] dispatcher installed on INTB_EXTER\n");

    return 0;
}

/* find_handler: Locate existing handler entry by IRQ.
 * Args: irq - interrupt number to search for.
 * Returns: pointer to handler slot or NULL when not found.
 */
static struct gic_irq_handler *find_handler(ULONG irq)
{
    for (ULONG i = 0; i < gic_data.handler_count; ++i)
    {
        if (gic_data.handlers[i].irq == irq)
            return &gic_data.handlers[i];
    }
    return NULL;
}

/* find_handler_index: Return handler array index for an IRQ.
 * Args: irq - interrupt number to search for.
 * Returns: non-negative index on success, -1 on failure.
 */
static LONG find_handler_index(ULONG irq)
{
    for (ULONG i = 0; i < gic_data.handler_count; ++i)
    {
        if (gic_data.handlers[i].irq == irq)
            return (LONG)i;
    }
    return -1;
}

/* gic400_enable_irq: Configure group 0 SPI and enable it.
 * Args: irq - interrupt number; priority - priority byte to assign.
 * Returns: 0 on success, -GIC_ERROR on invalid IRQ.
 */
static int gic400_enable_irq(ULONG irq, UBYTE priority)
{
    if (irq >= gic_data.max_irqs)
    {
        Kprintf("[gic] IRQ %ld is out of range, max is %ld\n", irq, gic_data.max_irqs - 1);
        return -GIC_ERROR;
    }

    gicd_disable_irq(irq); // disable IRQ before configuration

    gicd_set_group(irq, 0);           // assign to group 0
    gicd_set_priority(irq, priority); // set priority
    gicd_set_cpu(irq, 0, TRUE);       // route to CPU0
    gicd_set_trigger(irq, FALSE);     // set level-triggered

    gicd_enable_irq(irq); // enable IRQ

    return 0;
}

/* gic400_disable_irq: Disable a configurable SPI.
 * Args: irq - interrupt number to disable.
 * Returns: 0 on success, -GIC_ERROR on invalid IRQ.
 */
static int gic400_disable_irq(ULONG irq)
{
    if (irq >= gic_data.max_irqs)
    {
        Kprintf("[gic] IRQ %ld is out of range, max is %ld\n", irq, gic_data.max_irqs - 1);
        return -GIC_ERROR;
    }

    gicd_disable_irq(irq); // disable IRQ

    return 0;
}

/* gic400_get_irq_status: Read pending/active/enabled flags for SPI.
 * Args: irq - interrupt number; pending/active/enabled - optional outputs.
 * Returns: 0 on success, -GIC_ERROR on invalid IRQ.
 */
int gic400_get_irq_status(ULONG irq, BOOL *pending, BOOL *active, BOOL *enabled)
{
    if (irq >= gic_data.max_irqs)
    {
        Kprintf("[gic] IRQ %ld is out of range, max is %ld\n", irq, gic_data.max_irqs - 1);
        return -GIC_ERROR;
    }

    if (pending)
        *pending = gicd_is_pending(irq);
    if (active)
        *active = gicd_is_active(irq);
    if (enabled)
        *enabled = gicd_is_enabled(irq);

    return 0;
}

/* gic400_call_interrupt: Invoke interrupt server with Exec ABI.
 * Args: interrupt - Exec interrupt entry; irq - source IRQ number.
 * Returns: void.
 */
static inline void gic400_call_interrupt(struct Interrupt *interrupt, ULONG irq)
{
    if (interrupt == NULL || interrupt->is_Code == NULL)
        return;

    __asm__ __volatile__(
        "move.l %[sysbase],%%a6\n\t"
        "move.l %[irq],%%d0\n\t"
        "move.l %[data],%%a1\n\t"
        "jsr (%[code])\n\t"
        :
        : [code] "a"(interrupt->is_Code),
          [data] "r"(interrupt->is_Data),
          [irq] "r"(irq),
          [sysbase] "r"(SysBase)
        : "d0", "d1", "a0", "a1", "a6");
}

/* gic400_exec_dispatcher: Exec interrupt server for INTB_EXTER hook.
 * Args: none.
 * Returns: void.
 */
static ULONG gic400_exec_dispatcher(void)
{
    ULONG iar = gicc_acknowledge_interrupt_group(0);
    ULONG irq = iar & 0x3FF;

    if (irq == 0x3FF || irq == 0x3FE)
    {
        return 0; // No pending interrupts
    }

    if (irq >= gic_data.max_irqs)
    {
        gicc_end_interrupt_group(iar, 0);
        return 0;
    }

    struct gic_irq_handler *handler = find_handler(irq);

    if (handler && handler->interrupt)
    {
        gic400_call_interrupt(handler->interrupt, irq);
    }

    gicc_end_interrupt_group(iar, 0);
    return 1;
}

/* gic400_add_int_server: Register interrupt server for an SPI.
 * Args: irq - interrupt number; interrupt - Exec interrupt descriptor.
 * Returns: 0 on success, -GIC_ERROR when registration fails.
 */
int gic400_add_int_server(ULONG irq, struct Interrupt *interrupt)
{
    if (!interrupt || !interrupt->is_Code)
        return -GIC_ERROR;
    if (!irq_in_range(irq))
        return -GIC_ERROR;

    Disable();

    struct gic_irq_handler *existing = find_handler(irq);
    if (existing)
    {
        if (existing->interrupt == interrupt)
        {
            Enable();
            return 0;
        }

        Enable();
        Kprintf("[gic] IRQ %ld already has a different server registered\n", irq);
        return -GIC_ERROR;
    }

    if (gic_data.handler_count >= GIC_MAX_REGISTERED_IRQS)
    {
        Enable();
        Kprintf("[gic] IRQ table full (%ld entries)\n", (ULONG)GIC_MAX_REGISTERED_IRQS);
        return -GIC_ERROR;
    }

    gic_data.handlers[gic_data.handler_count].irq = irq;
    gic_data.handlers[gic_data.handler_count].interrupt = interrupt;
    gic_data.handler_count++;

    Enable();
    return 0;
}

/* gic400_rem_int_server: Remove registered interrupt server.
 * Args: irq - interrupt number; interrupt - handler to remove.
 * Returns: 0 on success, -GIC_ERROR if not found or mismatched.
 */
int gic400_rem_int_server(ULONG irq, struct Interrupt *interrupt)
{
    if (!interrupt)
        return -GIC_ERROR;
    if (!irq_in_range(irq))
        return -GIC_ERROR;

    Disable();

    LONG index = find_handler_index(irq);
    if (index < 0)
    {
        Enable();
        return -GIC_ERROR;
    }

    for (ULONG i = (ULONG)index; i + 1 < gic_data.handler_count; ++i)
    {
        gic_data.handlers[i] = gic_data.handlers[i + 1];
    }

    if (gic_data.handler_count > 0)
    {
        gic_data.handler_count--;
        gic_data.handlers[gic_data.handler_count].irq = (ULONG)-1;
        gic_data.handlers[gic_data.handler_count].interrupt = NULL;
    }

    Enable();
    return 0;
}

// SPDX-License-Identifier: GPL-2.0

/*
 * GIC-400 support library for Amiga OS running on Emu68/Raspberry Pi 4B
 * This is intended to handle primarily SPIs and is supposed to coexist with Emu68.
 * As far as I know Emu68 is running non-secure, so this code assumes non-secure mode only.
 * For start, well assume that:
 * - all SPIs are level-triggered, active high
 * - all SPIs are routed to CPU0 (where 68k emulation runs)
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

#define GIC_MAX_REGISTERED_IRQS 16U /* ...should be enough for everyone ;) */

extern struct ExecBase *SysBase;

static char gic_dispatcher_name[] = "ARM GIC-400 dispatcher";

struct gic_irq_handler
{
    ULONG irq;
    struct Interrupt *interrupt;
};

/* GIC Distributor and CPU interface identification helpers. */
#define GICD_IIDR_PRODUCT_ID(value) (((value) >> 24) & 0xFF)
#define GICD_IIDR_VARIANT(value) (((value) >> 16) & 0x0F)
#define GICD_IIDR_REVISION(value) (((value) >> 12) & 0x0F)
#define GICD_IIDR_IMPLEMENTER(value) ((value) & 0x0FFF)

#define GICD_TYPER_IT_LINES_NUMBER(value) ((value) & 0x1F)
#define GICD_TYPER_CPUS_NUMBER(value) (((value) >> 5) & 0x07)
#define GICD_TYPER_SECURITY_EXTN(value) (((value) >> 10) & 0x01)
#define GICD_TYPER_LSPI(value) (((value) >> 11) & 0x1F)

#define GICC_IIDR_PRODUCT_ID(value) (((value) >> 20) & 0x0FFF)
#define GICC_IIDR_ARCHITECTURE(value) (((value) >> 16) & 0x0F)
#define GICC_IIDR_REVISION(value) (((value) >> 12) & 0x0F)
#define GICC_IIDR_IMPLEMENTER(value) ((value) & 0x0FFF)

/* Bit definitions for GICC_CTLR */
#define GICC_CTLR_ENABLE_GRP1 (1UL << 0)
#define GICC_CTLR_FIQ_BYPASS_DIS_GRP1 (1UL << 5)
#define GICC_CTLR_IRQ_BYPASS_DIS_GRP1 (1UL << 6)
#define GICC_CTLR_EOI_MODE_NS (1UL << 9)

#define GICC_CTLR_FLAG(value, mask) ((ULONG)(((value) & (mask)) != 0))

static struct
{
    ULONG gicd_iidr;
    ULONG gicd_typer;
    ULONG gicc_iidr;

    APTR gic_base_distributor;
    APTR gic_base_cpuif;
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
#define GICD_CTLR (gic_data.gic_base_distributor + 0x000)                    // Distributor Control Register
#define GICD_TYPER (gic_data.gic_base_distributor + 0x004)                   // Interrupt Controller Type Register
#define GICD_IIDR (gic_data.gic_base_distributor + 0x008)                    // Distributor Implementer ID Register
#define GICD_IGROUPR(n) (gic_data.gic_base_distributor + 0x080 + (n) * 4)    // Interrupt Group Registers
#define GICD_ISENABLER(n) (gic_data.gic_base_distributor + 0x100 + (n) * 4)  // Interrupt Set-Enable Registers
#define GICD_ICENABLER(n) (gic_data.gic_base_distributor + 0x180 + (n) * 4)  // Interrupt Clear-Enable Registers
#define GICD_ISPENDR(n) (gic_data.gic_base_distributor + 0x200 + (n) * 4)    // Interrupt Set-Pending Registers
#define GICD_ICPENDR(n) (gic_data.gic_base_distributor + 0x280 + (n) * 4)    // Interrupt Clear-Pending Registers
#define GICD_ISACTIVER(n) (gic_data.gic_base_distributor + 0x300 + (n) * 4)  // Interrupt Set-Active Registers
#define GICD_ICACTIVER(n) (gic_data.gic_base_distributor + 0x380 + (n) * 4)  // Interrupt Clear-Active Registers
#define GICD_IPRIORITYR(n) (gic_data.gic_base_distributor + 0x400 + (n) * 4) // Interrupt Priority Registers
#define GICD_ITARGETSR(n) (gic_data.gic_base_distributor + 0x800 + (n) * 4)  // Interrupt Processor Targets Registers
#define GICD_ICFGR(n) (gic_data.gic_base_distributor + 0xC00 + (n) * 4)      // Interrupt Configuration Registers
#define GICD_SPISR(n) (gic_data.gic_base_distributor + 0xD04 + (n) * 4)      // Shared Peripheral Interrupt Status Registers
#define GICD_COMPONENT_ID (gic_data.gic_base_distributor + 0xFF0)            // Component ID Register
#define GICD_PERIPHERAL_ID (gic_data.gic_base_distributor + 0xFE0)           // Peripheral ID Register
// TODO SGIR(n)
// TODO CPENDSGIR(n)
// TODO SPENDSGIR(n)

/* gicd_print_info: Log distributor ID and capability registers.
 * Args: none.
 * Returns: void.
 */
static void gicd_print_info(void)
{
    ULONG iidr = gic_data.gicd_iidr;
    Kprintf("[gic] Distributor: Implementer=0x%03lx, Revision=%ld, Variant=%ld, ProductID=0x%02lx\n",
            GICD_IIDR_IMPLEMENTER(iidr), GICD_IIDR_REVISION(iidr), GICD_IIDR_VARIANT(iidr), GICD_IIDR_PRODUCT_ID(iidr));

    ULONG typer = gic_data.gicd_typer;
    Kprintf("[gic] Distributor: ITLinesNumber=%ld, CPUNumber=%ld, SecurityExtensions=%ld, LSPIs=%ld\n",
            (GICD_TYPER_IT_LINES_NUMBER(typer) + 1) * 32,
            GICD_TYPER_CPUS_NUMBER(typer) + 1,
            GICD_TYPER_SECURITY_EXTN(typer),
            GICD_TYPER_LSPI(typer));
}

/* gicd_enable_group: Enable forwarding of pending interrupts from the Distributor to the CPU interface
 */
static inline void gicd_enable()
{

    // set enable bit in GICD_CTLR
    ULONG reg = readl(GICD_CTLR);
    reg |= 1;
    writel(reg, GICD_CTLR);
}

/* gicd_disable_group: Disable forwarding of pending interrupts from the Distributor to the CPU interface
 */
static inline void gicd_disable()
{
    // clear enable bit in GICD_CTLR
    ULONG reg = readl(GICD_CTLR);
    reg &= ~1;
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

/* gicd_set_pending: Set pending bit for an IRQ in ISPENDR.
 * Args: irq - interrupt number.
 * Returns: void.
 */
static inline void gicd_set_pending(ULONG irq)
{
    if (!gicd_irq_valid(irq))
        return;

    // set pending bit in GICD_ISPENDR
    ULONG reg_index = irq >> 5;
    ULONG bit_offset = irq & 0x1F;
    writel(1UL << bit_offset, GICD_ISPENDR(reg_index));
}

/* gicd_clear_pending: Clear pending bit for an IRQ in ISPENDR.
 * Args: irq - interrupt number.
 * Returns: void.
 */
static inline void gicd_clear_pending(ULONG irq)
{
    if (!gicd_irq_valid(irq))
        return;

    // clear pending bit in GICD_ICPENDR
    ULONG reg_index = irq >> 5;
    ULONG bit_offset = irq & 0x1F;
    writel(1UL << bit_offset, GICD_ICPENDR(reg_index));
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
 * Note that in non-secure mode the LSB is always 0.
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

    reg = readl(GICD_ICFGR(reg_index));
    BOOL is_edge = ((reg >> bit_offset) & 0x02) != 0;
    if (is_edge != edge)
    {
        Kprintf("[gic] Failed to set GICD IRQ %lu trigger to %s\n", irq, edge ? "edge" : "level");
    }
}

/* CPU Interface */
#define GICC_CTLR (gic_data.gic_base_cpuif + 0x000) // CPU Interface Control Register
#define GICC_PMR (gic_data.gic_base_cpuif + 0x004)  // Interrupt Priority Mask Register

#define GICC_BPR (gic_data.gic_base_cpuif + 0x008)   // Binary Point Register
#define GICC_IAR (gic_data.gic_base_cpuif + 0x00C)   // Interrupt Acknowledge Register
#define GICC_EOIR (gic_data.gic_base_cpuif + 0x010)  // End of Interrupt Register
#define GICC_RPR (gic_data.gic_base_cpuif + 0x014)   // Running Priority Register
#define GICC_HPPIR (gic_data.gic_base_cpuif + 0x018) // Highest Priority Pending Interrupt Register

#define GICC_ABPR (gic_data.gic_base_cpuif + 0x01C)   // Aliased Binary Point Register
#define GICC_AIAR (gic_data.gic_base_cpuif + 0x020)   // Aliased Interrupt Acknowledge Register
#define GICC_AEOIR (gic_data.gic_base_cpuif + 0x024)  // Aliased End of Interrupt Register
#define GICC_AHPPIR (gic_data.gic_base_cpuif + 0x028) // Aliased Highest Priority Pending Interrupt Register

#define GICC_APR(n) (gic_data.gic_base_cpuif + 0x0D0 + (n) * 4)   // Active Priority Register
#define GICC_NSAPR(n) (gic_data.gic_base_cpuif + 0x0E0 + (n) * 4) // Non-secure Active Priority Register
#define GICC_IIDR (gic_data.gic_base_cpuif + 0x0FC)               // CPU Interface Implementer Identification Register
#define GICC_DIR (gic_data.gic_base_cpuif + 0x1000)               // Deactivate Interrupt Register

/* gicc_print_info: Log CPU interface identification details.
 * Args: none.
 * Returns: void.
 */
static void gicc_print_info(void)
{
    ULONG iidr = gic_data.gicc_iidr;
    Kprintf("[gic] Controller: Implementer=0x%03lx, Revision=%ld, Architecture=%ld, ProductID=0x%03lx\n",
            GICC_IIDR_IMPLEMENTER(iidr), GICC_IIDR_REVISION(iidr), GICC_IIDR_ARCHITECTURE(iidr), GICC_IIDR_PRODUCT_ID(iidr));
}

/* gicc_set_ctlr: Write CPU interface control register.
 * Args: ctlr - new register value.
 * Returns: void.
 */
static inline void gicc_set_ctlr(ULONG ctlr)
{
    writel(ctlr, GICC_CTLR);
}

/* gicc_get_ctlr: Read CPU interface control register.
 * Args: none.
 * Returns: current register value.
 */
static inline ULONG gicc_get_ctlr(void)
{
    return readl(GICC_CTLR);
}

static void gicc_log_ctlr(CONST_STRPTR label, ULONG ctlr)
{
    Kprintf("[gic] %s GICC_CTLR=0x%08lx: enable_grp1=%ld, fiq_bypass_dis_grp1=%ld, irq_bypass_dis_grp1=%ld, eoi_mode_ns=%ld\n",
            label, ctlr,
            GICC_CTLR_FLAG(ctlr, GICC_CTLR_ENABLE_GRP1),
            GICC_CTLR_FLAG(ctlr, GICC_CTLR_FIQ_BYPASS_DIS_GRP1),
            GICC_CTLR_FLAG(ctlr, GICC_CTLR_IRQ_BYPASS_DIS_GRP1),
            GICC_CTLR_FLAG(ctlr, GICC_CTLR_EOI_MODE_NS));
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

/* gicc_acknowledge_interrupt: Read pending IRQ ID from IAR.
 * Reads the interrupt ID of the highest priority pending interrupt from GICC_IAR.
 * The ID is in bits [9:0], bits [12:10] is CPUID, rest are reserved and RAZ.
 * If no interrupt is pending, the ID is 1023 (0x3FF).
 * If the ID is 1022 (0x3FE), it indicates a spurious interrupt.
 * The ID must be written to GICC_EOIR when the interrupt is handled.
 * Returns: raw value from the acknowledge register.
 */
static inline ULONG gicc_acknowledge_interrupt(void)
{
    return readl(GICC_IAR);
}

/* gicc_end_interrupt: Signal completion of an IRQ.
 * Ends the interrupt by writing the interrupt ID to GICC_EOIR.
 * if EIOmode is 0 - this drops the priority and deactivates the interrupt
 * if EIOmode is 1 - this only drops the priority
 * Args: irq - raw value previously read from IAR
 * Returns: void
 */
static inline void gicc_end_interrupt(ULONG irq)
{
    writel(irq, GICC_EOIR);
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

/* gicc_get_highest_pending: Fetch highest pending IRQ number.
 * Returns: pending IRQ ID masked to 10 bits.
 */
static inline ULONG gicc_get_highest_pending(void)
{
    return readl(GICC_HPPIR) & 0x3FF;
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
    // TODO this is awful. rework DT_TranslateAddress

    const APTR parent_key = DT_GetParent(gic_key);
    const ULONG address_cells_parent = DT_GetPropertyValueULONG(parent_key, "#address-cells", 1, FALSE);
    const ULONG size_cells_parent = DT_GetPropertyValueULONG(parent_key, "#size-cells", 1, FALSE);
    const ULONG cells_per_record = address_cells_parent + size_cells_parent;

    const ULONG *value = DT_GetPropValue(DT_FindProperty(gic_key, (CONST_STRPTR) "reg"));

    gic_data.gic_base_distributor = (APTR)(ULONG)DT_GetNumber(value, address_cells_parent);
    DT_TranslateAddress(&gic_data.gic_base_distributor, parent_key);
    if (gic_data.gic_base_distributor == NULL)
    {
        Kprintf("[gic] %s: Failed to get Distributor base address for GIC\n", __func__);
        DT_CloseKey(gic_key);
        DT_CloseKey(root_key);
        return -GIC_ERROR;
    }

    gic_data.gic_base_cpuif = (APTR)(ULONG)DT_GetNumber(value + cells_per_record, address_cells_parent);
    DT_TranslateAddress(&gic_data.gic_base_cpuif, parent_key);
    if (gic_data.gic_base_cpuif == NULL)
    {
        Kprintf("[gic] %s: Failed to get CPU Interface base address for GIC\n", __func__);
        DT_CloseKey(gic_key);
        DT_CloseKey(root_key);
        return -GIC_ERROR;
    }

    Kprintf("[genet] %s: compatible: %s\n", __func__, gic_compatible);
    Kprintf("[genet] %s: Distributor register base: %08lx\n", __func__, gic_data.gic_base_distributor);
    Kprintf("[genet] %s: CPU Interface register base: %08lx\n", __func__, gic_data.gic_base_cpuif);

    // We're done with the device tree
    DT_CloseKey(gic_key);
    DT_CloseKey(root_key);
    return 0;
}

/* gic400_init: Initialize GIC state and install dispatcher.
 * Args: base - physical base address shared with Emu68.
 * Returns: 0 on success, -GIC_ERROR on failure.
 */
int gic400_init()
{
    int ret = gic400_parse_devicetree();
    if (ret < 0)
        return ret;

    // Initialize GICD
    gic_data.gicd_iidr = readl(GICD_IIDR);
    gic_data.gicd_typer = readl(GICD_TYPER);
    gic_data.gicc_iidr = readl(GICC_IIDR);

    gic_data.max_irqs = (GICD_TYPER_IT_LINES_NUMBER(gic_data.gicd_typer) + 1) * 32;

    gic_data.handler_count = 0;
    for (ULONG i = 0; i < GIC_MAX_REGISTERED_IRQS; ++i)
    {
        gic_data.handlers[i].irq = (ULONG)-1;
        gic_data.handlers[i].interrupt = NULL;
    }

    gicc_print_info();
    gicd_print_info();

    Disable();

    gicc_set_priority_mask(0x7F); // allow all priorities

    ULONG ctlr = gicc_get_ctlr();
    gicc_log_ctlr((CONST_STRPTR) "Initial", ctlr);

    ctlr &= ~GICC_CTLR_EOI_MODE_NS;        // GICC_EOIR does both priority drop and deactivate
    ctlr |= GICC_CTLR_ENABLE_GRP1;         // enable CPU interface
    ctlr |= GICC_CTLR_FIQ_BYPASS_DIS_GRP1; // disable bypassing of FIQ for Group 1
    ctlr |= GICC_CTLR_IRQ_BYPASS_DIS_GRP1; // disable bypassing of IRQ for Group 1

    gicc_log_ctlr((CONST_STRPTR) "Configured", ctlr);
    gicc_set_ctlr(ctlr);

    ctlr = gicc_get_ctlr();
    gicc_log_ctlr((CONST_STRPTR) "Final", ctlr);

    gicd_enable(); // enable Distributor

    gic_data.dispatcher_interrupt.is_Node.ln_Type = NT_INTERRUPT;
    gic_data.dispatcher_interrupt.is_Node.ln_Pri = 100;
    gic_data.dispatcher_interrupt.is_Node.ln_Name = gic_dispatcher_name;
    gic_data.dispatcher_interrupt.is_Data = NULL;
    gic_data.dispatcher_interrupt.is_Code = (APTR)gic400_exec_dispatcher;
    AddIntServer(INTB_EXTER, &gic_data.dispatcher_interrupt);
    Kprintf("[gic] dispatcher installed on INTB_EXTER\n");
    Enable();

    return 0;
}

/* gic400_shutdown: Remove all handlers and dispatcher.
 * Args: none.
 * Returns: void.
 */
void gic400_shutdown()
{
    Disable();
    while (gic_data.handler_count > 0)
    {
        gic400_rem_int_server(gic_data.handlers[0].irq, gic_data.handlers[0].interrupt);
        Kprintf("[gic] warning: removed handler for IRQ %ld during shutdown\n", gic_data.handlers[0].irq);
    }

    RemIntServer(INTB_EXTER, &gic_data.dispatcher_interrupt);
    gicd_disable();

    Enable();
    Kprintf("[gic] dispatcher removed from INTB_EXTER\n");
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
 * Args: 
 *  irq - interrupt number
 *  priority - priority byte to assign
 *  edge - TRUE for edge-triggered, FALSE for level-triggered
 * Returns: 0 on success, -GIC_ERROR on invalid IRQ.
 */
static int gic400_enable_irq(ULONG irq, UBYTE priority, BOOL edge)
{
    if (irq >= gic_data.max_irqs)
    {
        Kprintf("[gic] IRQ %ld is out of range, max is %ld\n", irq, gic_data.max_irqs - 1);
        return -GIC_ERROR;
    }
    Kprintf("[gic] Enabling IRQ %ld with priority %lu\n", irq, priority);

    gicd_disable_irq(irq); // disable IRQ before configuration

    gicd_set_priority(irq, priority); // set priority
    gicd_set_cpu(irq, 0, TRUE);       // route to CPU0
    gicd_set_trigger(irq, edge);     // set level-triggered

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
    Kprintf("[gic] Disabling IRQ %ld\n", irq);

    gicd_disable_irq(irq); // disable IRQ
    gicd_set_cpu(irq, 0, FALSE); // unroute from CPU0

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
    ULONG iar = gicc_acknowledge_interrupt();
    ULONG irq = iar & 0x3FF;

    if (irq == 0x3FF || irq == 0x3FE)
    {
        return 0; // No pending interrupts
    }

    if (irq >= gic_data.max_irqs)
    {
        gicc_end_interrupt(iar);
        return 0;
    }

    struct gic_irq_handler *handler = find_handler(irq);

    if (handler && handler->interrupt)
    {
        KprintfH("[gic] Invoking handler for IRQ %ld\n", irq);
        gic400_call_interrupt(handler->interrupt, irq);
    }

    gicc_end_interrupt(iar);
    return 1;
}

/* gic400_add_int_server: Register interrupt server for an SPI.
 * Args: 
 *  irq - interrupt number
 *  priority - priority byte to assign (0-0x7f)
 *  edge - TRUE for edge-triggered, FALSE for level-triggered
 *  interrupt - Exec interrupt descriptor
 * Returns: 0 on success, -GIC_ERROR when registration fails.
 */
int gic400_add_int_server(ULONG irq, UBYTE priority, BOOL edge, struct Interrupt *interrupt)
{
    if (!interrupt || !interrupt->is_Code)
    {
        Kprintf("[gic] Invalid interrupt server for IRQ %ld\n", irq);
        return -GIC_ERROR;
    }
    if (!gicd_irq_valid(irq))
    {
        Kprintf("[gic] IRQ %ld is not a valid SPI\n", irq);
        return -GIC_ERROR;
    }

    Disable();

    struct gic_irq_handler *existing = find_handler(irq);
    if (existing)
    {
        if (existing->interrupt == interrupt)
        {
            Kprintf("[gic] IRQ %ld is already registered\n", irq);
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
    gic400_enable_irq(irq, priority, edge);

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
    {
        Kprintf("[gic] Invalid interrupt server for IRQ %ld\n", irq);
        return -GIC_ERROR;
    }
    if (!gicd_irq_valid(irq))
    {
        Kprintf("[gic] IRQ %ld is not a valid SPI\n", irq);
        return -GIC_ERROR;
    }

    Disable();

    LONG index = find_handler_index(irq);
    if (index < 0)
    {
        Kprintf("[gic] No handler registered for IRQ %ld\n", irq);
        Enable();
        return -GIC_ERROR;
    }

    gic400_disable_irq(irq);

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

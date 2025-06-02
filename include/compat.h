// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
#ifndef _COMPAT_H
#define _COMPAT_H

#include <exec/types.h>

inline ULONG LE32(ULONG x) { return __builtin_bswap32(x); }

inline void delay_us(ULONG us)
{
    ULONG timer = LE32(*(volatile ULONG *)0xf2003004); // TODO get from device tree
    ULONG end = timer + us;

    if (end < timer)
    {
        while (end < LE32(*(volatile ULONG *)0xf2003004))
            asm volatile("nop");
    }
    while (end > LE32(*(volatile ULONG *)0xf2003004))
        asm volatile("nop");
}

inline void _memset(APTR dst, UBYTE val, ULONG len)
{
    UBYTE *d = (UBYTE *)dst;
    for (ULONG i = 0; i < len; i++)
        d[i] = val;
}

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

inline static APTR roundup(APTR x, ULONG y)
{
    return (APTR)((((ULONG)x + y - 1) / y) * y);
}

inline static APTR rounddown(APTR x, ULONG y)
{
    return (APTR)((ULONG)x - ((ULONG)x % y));
}

#define lower_32_bits(n) ((ULONG)(n))

#define BIT(nr) (1UL << (nr))

#define readl(addr) in_le32((volatile ULONG *)(addr))
#define writel(b, addr) out_le32((volatile ULONG *)(addr), (b))

static inline void writel_relaxed(ULONG val, APTR addr)
{
    *(volatile ULONG *)addr = LE32(val);
    // asm volatile("nop");
}

static inline ULONG readl_relaxed(APTR addr)
{
    ULONG val = LE32(*(volatile ULONG *)addr);
    // asm volatile("nop");
    return val;
}

static inline unsigned in_le32(volatile ULONG *addr)
{
    ULONG val = LE32(*(volatile ULONG *)addr);
    // asm volatile("nop");
    return val;
}

static inline void out_le32(volatile ULONG *addr, int val)
{
    *(volatile ULONG *)addr = LE32(val);
    // asm volatile("nop");
}

#define clrbits_32(addr, clear) clrbits(le32, addr, clear)
#define setbits_32(addr, set) setbits(le32, addr, set)
#define clrsetbits_32(addr, clear, set) clrsetbits(le32, addr, clear, set)

#define clrbits(type, addr, clear) \
    out_##type((addr), in_##type(addr) & ~(clear))

#define setbits(type, addr, set) \
    out_##type((addr), in_##type(addr) | (set))

#define clrsetbits(type, addr, clear, set) \
    out_##type((addr), (in_##type(addr) & ~(clear)) | (set))

#endif
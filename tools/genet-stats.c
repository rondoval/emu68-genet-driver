// SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
// genet-stats — genet.device SANA-II statistics viewer

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/listbrowser.h>

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <clib/alib_protos.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <gadgets/listbrowser.h>
#include <devices/sana2.h>

#include <stdio.h>
#include <string.h>

struct ExecBase      *SysBase;
struct IntuitionBase *IntuitionBase;
struct DosLibrary    *DOSBase;
struct Library       *ListBrowserBase;

#define GENET_DEVICE  "genet.device"
#define GENET_UNIT    0
#define REFRESH_SECS  1

#define WIN_LEFT    50
#define WIN_TOP     50
#define WIN_WIDTH   400
#define WIN_HEIGHT  440

#define LBL_SZ 32
#define VAL_SZ 32

#define MAX_SS_RECORDS 64

/* Combine S2QUAD high/low halves into a 64-bit value. */
#define S2Q(q)  (((unsigned long long)(q).s2q_High << 32) | (unsigned long long)(q).s2q_Low)

static const struct TagItem s_bufmgr[] = { {TAG_DONE, 0} };

struct dyn_row {
    char label[LBL_SZ];
    char value[VAL_SZ];
};

#define MAX_ROWS 96

static struct List         s_list;
static struct dyn_row      s_rows[MAX_ROWS];
static int                 s_row_count;
static unsigned long long  s_rx_bytes;
static unsigned long long  s_tx_bytes;
static ULONG               s_prev_irq0_count;
static BOOL                s_have_irq0_sample;

static struct ColumnInfo s_ci[] = {
    { 200, (STRPTR)"Statistic", 0 },
    { 160, (STRPTR)"Value",     0 },
    { -1,  (STRPTR)~0UL, ~0UL }
};

/* ------------------------------------------------------------------ */

static void fmt_dec1(char *out, unsigned long long val,
                     unsigned long long div, const char *unit)
{
    snprintf(out, VAL_SZ, "%llu.%llu %s",
             val / div, (val % div) * 10ULL / div, unit);
}

static void fmt_rate(char *out, unsigned long long bps)
{
    if      (bps >= 1048576ULL) fmt_dec1(out, bps, 1048576ULL, "MiB/s");
    else if (bps >= 1024ULL)    fmt_dec1(out, bps, 1024ULL,    "KiB/s");
    else                        snprintf(out, VAL_SZ, "%llu B/s", bps);
}

static void fmt_count_rate(char *out, unsigned long long rate)
{
    snprintf(out, VAL_SZ, "%llu/s", rate);
}

static void fmt_bytes(char *out, unsigned long long bytes)
{
    if      (bytes >= 1073741824ULL) fmt_dec1(out, bytes, 1073741824ULL, "GiB");
    else if (bytes >= 1048576ULL)    fmt_dec1(out, bytes, 1048576ULL,    "MiB");
    else if (bytes >= 1024ULL)       fmt_dec1(out, bytes, 1024ULL,       "KiB");
    else                             snprintf(out, VAL_SZ, "%llu B", bytes);
}

/* ------------------------------------------------------------------ */

static void clear_list(struct Gadget *lb, struct Window *win)
{
    SetGadgetAttrs(lb, win, NULL, LISTBROWSER_Labels, ~0UL, TAG_DONE);
    struct Node *n;
    while ((n = RemHead(&s_list)) != NULL)
        FreeListBrowserNode(n);
    s_row_count = 0;
}

static void attach_list(struct Gadget *lb, struct Window *win)
{
    SetGadgetAttrs(lb, win, NULL, LISTBROWSER_Labels, (ULONG)&s_list, TAG_DONE);
}

static void add_row(const char *label, const char *value)
{
    if (s_row_count >= MAX_ROWS) return;
    int i = s_row_count++;
    strncpy(s_rows[i].label, label, LBL_SZ - 1);
    s_rows[i].label[LBL_SZ - 1] = 0;
    if (value)
    {
        strncpy(s_rows[i].value, value, VAL_SZ - 1);
        s_rows[i].value[VAL_SZ - 1] = 0;
    }
    else
    {
        s_rows[i].value[0] = 0;
    }
    struct Node *node = AllocListBrowserNode(2,
        LBNA_Column, 0, LBNCA_Text, (ULONG)(STRPTR)s_rows[i].label,
        LBNA_Column, 1, LBNCA_Text, (ULONG)(STRPTR)s_rows[i].value,
        TAG_DONE);
    if (node) AddTail(&s_list, node);
}

static void add_section(const char *title)
{
    char buf[LBL_SZ];
    snprintf(buf, sizeof(buf), "--- %s ---", title);
    add_row(buf, "");
}

/* ------------------------------------------------------------------ */

static void resize_lb(struct Gadget *lb, struct Window *win)
{
    WORD bw = (WORD)(win->BorderLeft + win->BorderRight);
    WORD bh = (WORD)(win->BorderTop  + win->BorderBottom);
    RemoveGList(win, lb, -1);
    SetGadgetAttrs(lb, win, NULL,
        GA_Top,       (ULONG)(LONG)win->BorderTop,
        GA_Left,      (ULONG)(LONG)win->BorderLeft,
        GA_RelWidth,  (ULONG)(LONG)(-bw),
        GA_RelHeight, (ULONG)(LONG)(-bh),
        TAG_DONE);
    AddGList(win, lb, (UWORD)-1, -1, NULL);
    RefreshGList(lb, win, NULL, -1);
}

/* ------------------------------------------------------------------ */

static const char *section_for_type(ULONG type)
{
    /* type = (S2WireType_Ethernet << 16) | sub */
    ULONG sub = type & 0xFFFFul;
    if (sub <  0x1100ul) return "Driver software";
    if (sub <  0x1140ul) return "Hardware RX";
    if (sub <  0x1180ul) return "Hardware TX";
    return "MAC misc";
}

static void fetch_stats(struct Gadget *lb, struct Window *win,
                        struct IOSana2Req *io,
                        struct Sana2ExtDeviceStats *xs,
                        struct Sana2ThroughputStats *ts,
                        UBYTE *ssbuf)
{
    /* Extended global stats */
    xs->s2xds_Length        = (ULONG)sizeof(*xs);
    io->ios2_Req.io_Command = S2_GETEXTENDEDGLOBALSTATS;
    io->ios2_StatData       = (APTR)xs;
    DoIO((struct IORequest *)io);
    BYTE xs_err = io->ios2_Req.io_Error;

    /* Throughput sample */
    ts->s2ts_Length         = (ULONG)sizeof(*ts);
    ts->s2ts_NotifyTask     = NULL;
    ts->s2ts_NotifyMask     = 0;
    io->ios2_Req.io_Command = S2_SAMPLE_THROUGHPUT;
    io->ios2_StatData       = (APTR)ts;
    DoIO((struct IORequest *)io);

    char rate_rx[VAL_SZ], rate_tx[VAL_SZ];
    unsigned long long elapsed_us = 0;
    if (ts->s2ts_StartTime.tv_secs != 0) {
        elapsed_us =
            (unsigned long long)(ts->s2ts_EndTime.tv_secs - ts->s2ts_StartTime.tv_secs) * 1000000ULL
            + (unsigned long long)ts->s2ts_EndTime.tv_micro
            - (unsigned long long)ts->s2ts_StartTime.tv_micro;
        if (!elapsed_us) elapsed_us = 1;
        unsigned long long rx_delta = S2Q(ts->s2ts_BytesReceived);
        unsigned long long tx_delta = S2Q(ts->s2ts_BytesSent);
        s_rx_bytes += rx_delta;
        s_tx_bytes += tx_delta;
        fmt_rate(rate_rx, rx_delta * 1000000ULL / elapsed_us);
        fmt_rate(rate_tx, tx_delta * 1000000ULL / elapsed_us);
    } else {
        strcpy(rate_rx, "---");
        strcpy(rate_tx, "---");
    }

    /* Special stats */
    struct Sana2SpecialStatHeader *hdr = (struct Sana2SpecialStatHeader *)ssbuf;
    hdr->RecordCountMax      = MAX_SS_RECORDS;
    hdr->RecordCountSupplied = 0;
    io->ios2_Req.io_Command  = S2_GETSPECIALSTATS;
    io->ios2_StatData        = (APTR)hdr;
    DoIO((struct IORequest *)io);
    BYTE ss_err = io->ios2_Req.io_Error;
    struct Sana2SpecialStatRecord *recs = (struct Sana2SpecialStatRecord *)(hdr + 1);

    /* Rebuild list */
    clear_list(lb, win);

    char tmp[VAL_SZ];

    if (xs_err == 0) {
        snprintf(tmp, VAL_SZ, "%llu", S2Q(xs->s2xds_PacketsReceived));
        add_row("Packets RX", tmp);
        snprintf(tmp, VAL_SZ, "%llu", S2Q(xs->s2xds_PacketsSent));
        add_row("Packets TX", tmp);
        fmt_bytes(tmp, s_rx_bytes); add_row("Bytes RX (since open)", tmp);
        fmt_bytes(tmp, s_tx_bytes); add_row("Bytes TX (since open)", tmp);
        add_row("Throughput RX", rate_rx);
        add_row("Throughput TX", rate_tx);
        snprintf(tmp, VAL_SZ, "%llu", S2Q(xs->s2xds_UnknownTypesReceived));
        add_row("Unknown Protocol", tmp);
        snprintf(tmp, VAL_SZ, "%llu", S2Q(xs->s2xds_Reconfigurations));
        add_row("Reconfigurations", tmp);
    } else {
        snprintf(tmp, VAL_SZ, "error %d", (int)xs_err);
        add_row("Packets RX", tmp);
    }

    if (ss_err == 0 && hdr->RecordCountSupplied > 0) {
        const char *cur_section = NULL;
        for (ULONG i = 0; i < hdr->RecordCountSupplied; i++) {
            const char *sect = section_for_type(recs[i].Type);
            if (cur_section == NULL || strcmp(sect, cur_section) != 0) {
                add_section(sect);
                cur_section = sect;
            }
            const char *label = recs[i].String ? (const char *)recs[i].String : "?";
            if (recs[i].Type == ((((ULONG)S2WireType_Ethernet) & 0xFFFFul) << 16 | 0x1014ul)) {
                ULONG irq0_count = recs[i].Count;
                if (elapsed_us != 0 && s_have_irq0_sample)
                {
                    ULONG irq0_delta = irq0_count - s_prev_irq0_count;
                    fmt_count_rate(tmp, ((unsigned long long)irq0_delta * 1000000ULL) / elapsed_us);
                }
                else
                {
                    strcpy(tmp, "---");
                }
                s_prev_irq0_count = irq0_count;
                s_have_irq0_sample = TRUE;
                label = "irq0_rate";
            }
            else
            {
                snprintf(tmp, VAL_SZ, "%lu", (unsigned long)recs[i].Count);
            }
            add_row(label, tmp);
        }
    } else if (ss_err != 0) {
        snprintf(tmp, VAL_SZ, "error %d", (int)ss_err);
        add_row("Special stats", tmp);
    }

    attach_list(lb, win);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    /* Declared up front: all touched by cleanup. */
    struct MsgPort      *devPort   = NULL;
    struct MsgPort      *timerPort = NULL;
    struct IOSana2Req   *devIO     = NULL;
    struct timerequest  *timerIO   = NULL;
    struct Window       *win       = NULL;
    struct Gadget       *lb_gad    = NULL;
    UBYTE               *ssbuf     = NULL;
    int dev_open   = 0;
    int timer_open = 0;
    int timer_sent = 0;
    int rc         = RETURN_FAIL;

    (void)argc; (void)argv;

    SysBase         = *(struct ExecBase **)4UL;
    DOSBase         = (struct DosLibrary    *)OpenLibrary((CONST_STRPTR)"dos.library",       37);
    IntuitionBase   = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library",  37);
    ListBrowserBase =                         OpenLibrary((CONST_STRPTR)"gadgets/listbrowser.gadget", 0);
    if (!IntuitionBase || !ListBrowserBase) goto cleanup;

    devPort   = CreateMsgPort();
    timerPort = CreateMsgPort();
    if (!devPort || !timerPort) goto cleanup;

    devIO   = (struct IOSana2Req  *)CreateIORequest(devPort,   sizeof(struct IOSana2Req));
    timerIO = (struct timerequest *)CreateIORequest(timerPort, sizeof(struct timerequest));
    if (!devIO || !timerIO) goto cleanup;

    devIO->ios2_BufferManagement = (APTR)s_bufmgr;
    dev_open   = (OpenDevice((CONST_STRPTR)GENET_DEVICE, GENET_UNIT,
                             (struct IORequest *)devIO, 0) == 0);
    timer_open = (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                             (struct IORequest *)timerIO, 0) == 0);

    /* Buffer for S2_GETSPECIALSTATS: header + N records */
    ULONG ssbuf_size = (ULONG)sizeof(struct Sana2SpecialStatHeader)
                     + (ULONG)MAX_SS_RECORDS * (ULONG)sizeof(struct Sana2SpecialStatRecord);
    ssbuf = AllocVec(ssbuf_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!ssbuf) goto cleanup;

    NewList(&s_list);

    win = (struct Window *)OpenWindowTags(NULL,
        WA_Left,      WIN_LEFT,    WA_Top,       WIN_TOP,
        WA_Width,     WIN_WIDTH,   WA_Height,    WIN_HEIGHT,
        WA_MinWidth,  280,         WA_MinHeight, 130,
        WA_MaxWidth,  ~0UL,        WA_MaxHeight, ~0UL,
        WA_IDCMP,     (ULONG)(IDCMP_CLOSEWINDOW | IDCMP_NEWSIZE),
        WA_Flags,     (ULONG)(WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                              WFLG_SIZEGADGET | WFLG_ACTIVATE | WFLG_SMART_REFRESH),
        WA_Title,     (ULONG)"genet.device Statistics",
        WA_PubScreen, 0,
        TAG_DONE);
    if (!win) goto cleanup;

    WORD bw = (WORD)(win->BorderLeft + win->BorderRight);
    WORD bh = (WORD)(win->BorderTop  + win->BorderBottom);
    lb_gad = (struct Gadget *)NewObject(LISTBROWSER_GetClass(), NULL,
        GA_ID,                    1,
        GA_Top,                   (ULONG)(LONG)win->BorderTop,
        GA_Left,                  (ULONG)(LONG)win->BorderLeft,
        GA_RelWidth,              (ULONG)(LONG)(-bw),
        GA_RelHeight,             (ULONG)(LONG)(-bh),
        LISTBROWSER_Labels,       (ULONG)&s_list,
        LISTBROWSER_ColumnInfo,   (ULONG)s_ci,
        LISTBROWSER_ColumnTitles, (LONG)TRUE,
        LISTBROWSER_ShowSelected, (LONG)FALSE,
        LISTBROWSER_Separators,   (LONG)FALSE,
        LISTBROWSER_AutoFit,      (LONG)TRUE,
        GA_ReadOnly,              (LONG)TRUE,
        TAG_DONE);
    if (!lb_gad) goto cleanup;

    AddGList(win, lb_gad, (UWORD)-1, -1, NULL);
    RefreshGList(lb_gad, win, NULL, -1);

    struct Sana2ExtDeviceStats  xs;
    struct Sana2ThroughputStats ts;
    memset(&xs, 0, sizeof(xs));
    memset(&ts, 0, sizeof(ts));

    if (dev_open) {
        fetch_stats(lb_gad, win, devIO, &xs, &ts, ssbuf);
    } else {
        add_row("Status", "No genet.device");
        attach_list(lb_gad, win);
    }

    if (timer_open) {
        timerIO->tr_node.io_Command = TR_ADDREQUEST;
        timerIO->tr_time.tv_secs    = REFRESH_SECS;
        timerIO->tr_time.tv_micro   = 0;
        SendIO((struct IORequest *)timerIO);
        timer_sent = 1;
    }

    ULONG win_sig   = 1UL << win->UserPort->mp_SigBit;
    ULONG timer_sig = timer_open ? (1UL << timerPort->mp_SigBit) : 0UL;
    int   done      = 0;

    while (!done) {
        ULONG sig = Wait(win_sig | timer_sig | SIGBREAKF_CTRL_C);

        if (sig & SIGBREAKF_CTRL_C) done = 1;

        if (sig & win_sig) {
            struct IntuiMessage *msg;
            while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL) {
                ULONG cls = msg->Class;
                ReplyMsg((struct Message *)msg);
                if      (cls == IDCMP_CLOSEWINDOW) done = 1;
                else if (cls == IDCMP_NEWSIZE)     resize_lb(lb_gad, win);
            }
        }

        if (!done && (sig & timer_sig)) {
            WaitIO((struct IORequest *)timerIO);
            if (dev_open)
                fetch_stats(lb_gad, win, devIO, &xs, &ts, ssbuf);
            timerIO->tr_node.io_Command = TR_ADDREQUEST;
            timerIO->tr_time.tv_secs    = REFRESH_SECS;
            timerIO->tr_time.tv_micro   = 0;
            SendIO((struct IORequest *)timerIO);
        }
    }

    rc = RETURN_OK;

cleanup:
    if (timer_sent) {
        if (!CheckIO((struct IORequest *)timerIO))
            AbortIO((struct IORequest *)timerIO);
        WaitIO((struct IORequest *)timerIO);
    }
    if (lb_gad && win) {
        SetGadgetAttrs(lb_gad, win, NULL, LISTBROWSER_Labels, ~0UL, TAG_DONE);
        RemoveGList(win, lb_gad, -1);
        DisposeObject(lb_gad);
    }
    {
        struct Node *n;
        while ((n = RemHead(&s_list)) != NULL)
            FreeListBrowserNode(n);
    }
    if (win)            CloseWindow(win);
    if (ssbuf)          FreeVec(ssbuf);
    if (timer_open)     CloseDevice((struct IORequest *)timerIO);
    if (dev_open)       CloseDevice((struct IORequest *)devIO);
    if (timerIO)        DeleteIORequest((struct IORequest *)timerIO);
    if (devIO)          DeleteIORequest((struct IORequest *)devIO);
    if (timerPort)      DeleteMsgPort(timerPort);
    if (devPort)        DeleteMsgPort(devPort);
    if (ListBrowserBase) CloseLibrary(ListBrowserBase);
    if (IntuitionBase)  CloseLibrary((struct Library *)IntuitionBase);
    if (DOSBase)        CloseLibrary((struct Library *)DOSBase);
    return rc;
}

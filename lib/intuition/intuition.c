/*
 * lib/intuition/intuition.c -- intuition.library: the client's side
 * (v7-plan steps 3 and 4)
 *
 * Over the messages of planar/intuition.h, on a connection the caller's
 * Intuition holds. OPEN, CLOSE, INFO and EVENTS wait for their answers, a second
 * at most. A present doesn't: it goes on one of INTUI_INFLIGHT messages
 * of its own, and its answer is taken later, when a message is wanted
 * again. A present intuition refused is said by the next PresentWindow
 * or WaitSafe. PresentWindow checks what intuition would refuse first,
 * so a refusal there is a client's bug or a window intuition closed.
 *
 * WaitSafe makes no kernel call when the back buffer is free already,
 * nor GetIntuiEvent, unless it reads the ring down from full or from
 * INTUI_EVENTS_MOVES: intuition may have held events back for room
 * there, and is told with SignalConn there is some.
 * The connection's IntuiAck is a page of its own, shared with intuition
 * read-only. Nothing here keeps state of its own: it's all in the
 * caller's Intuition and Windows, and that page.
 */
#include <planar/intuition.h>
#include <planar/ipc.h>
#include <planar/syscalls.h>

constexpr int64_t SIG_INTUITION = 22;
constexpr uint64_t MS = 1000000;
constexpr uint64_t CALL_DEADLINE = 1000 * MS;

static void clear(void *p, uint64_t n)
{
    for (uint8_t *b = p; n--;)
        *b++ = 0;
}

static int64_t call(struct Intuition *in, uint32_t op)
{
    int64_t e;

    in->in_Call->im_Msg.mn_Length = sizeof *in->in_Call;
    in->in_Call->im_Op = op;
    in->in_Call->im_Task = FindTask(nullptr);
    e = DoMsgUntil(&in->in_Port, &in->in_Call->im_Msg, CurrentTime() + CALL_DEADLINE);
    return e ? e : in->in_Call->im_Msg.mn_Error;
}

/* The page saying how far the caller has read, shared with intuition,
 * and the page with the ring. */
static int64_t events_up(struct Intuition *in)
{
    void *at;
    int64_t h, e;

    if ((h = AllocArea(4096, AREAF_CLEAR, &at)) < 0)
        return h;
    in->in_AckArea = (Handle)h;
    in->in_Ack = at;
    if ((e = ShareArea(in->in_AckArea, in->in_Server, AREAR_READ)) != 0)
        return e;
    clear(in->in_Call, sizeof *in->in_Call);
    in->in_Call->im_Ack = (uint64_t)in->in_Ack;
    if ((e = call(in, INTUI_EVENTS)) != 0)
        return e;
    in->in_Page = (const struct IntuiPage *)in->in_Call->im_Page;
    return 0;
}

int64_t OpenIntuition(struct Intuition *in, uint64_t deadline)
{
    int64_t e;

    clear(in, sizeof *in);
    /* A slot each way for the call and each present: a power of two. */
    if ((e = ConnectPortUntil(&in->in_Port, "intuition", SIG_INTUITION, 2 * INTUI_INFLIGHT,
                              (1 + INTUI_INFLIGHT) * sizeof(struct IntuiMsg), deadline)) != 0)
        return e;
    in->in_Call = AllocMsg(&in->in_Port, sizeof *in->in_Call);
    for (unsigned i = 0; i < INTUI_INFLIGHT; i++)
        in->in_Present[i] = AllocMsg(&in->in_Port, sizeof *in->in_Call);
    if (!in->in_Call || !in->in_Present[INTUI_INFLIGHT - 1]) {
        ClosePort(&in->in_Port);
        return ERR_NOMEM;
    }
    clear(in->in_Call, sizeof *in->in_Call);
    if ((e = call(in, INTUI_INFO)) == 0) {
        in->in_Server = (Handle)in->in_Call->im_Server;
        in->in_Width = in->in_Call->im_Width;
        in->in_Height = in->in_Call->im_Height;
        e = events_up(in);
    }
    if (e != 0)
        CloseIntuition(in);
    return e;
}

/* The windows go with the connection: intuition closes them, and lets
 * go of the IntuiAck's page, which lasts until it has. */
void CloseIntuition(struct Intuition *in)
{
    ClosePort(&in->in_Port);
    if (in->in_AckArea)
        FreeArea(in->in_AckArea);
    in->in_AckArea = 0;
    in->in_Ack = nullptr;
    in->in_Page = nullptr;
}

static void describe(struct IntuiBuffer *ib, const struct BitMap *bm)
{
    *ib = (struct IntuiBuffer){
        .ib_Address = (uint64_t)bm->bm_BaseAddr, .ib_BytesPerRow = bm->bm_BytesPerRow,
        .ib_Format = bm->bm_PixelFormat, .ib_Width = bm->bm_Width, .ib_Height = bm->bm_Height,
    };
}

/* gfx.library's InitRastPort, as the library can't call it. */
static void rastport(struct Window *w)
{
    struct BitMap *bm = w->w_BitMap[w->w_Back];

    w->w_RPort = (struct RastPort){
        .rp_BitMap = bm, .rp_Clip = { 0, 0, (int32_t)bm->bm_Width - 1, (int32_t)bm->bm_Height - 1 },
        .rp_FgPen = 0xffffffff, .rp_BgPen = 0xff000000, .rp_DrawMode = JAM2,
    };
}

int64_t OpenWindowTagList(struct Intuition *in, struct Window *w, const struct TagItem *tags)
{
    struct BitMap *bm = (struct BitMap *)GetTagData(WA_BitMap, 0, tags);
    struct BitMap *back = (struct BitMap *)GetTagData(WA_BackBitMap, 0, tags);
    const char *title = (const char *)GetTagData(WA_Title, (uint64_t)"", tags);
    struct IntuiMsg *m = in->in_Call;
    int64_t e;

    clear(w, sizeof *w);
    if (!bm || !bm->bm_Area || (back && !back->bm_Area))
        return ERR_BADARG;              /* AllocBitMap's, to be shared */
    clear(m, sizeof *m);
    m->im_Left = (int32_t)GetTagData(WA_Left, 0, tags);
    m->im_Top = (int32_t)GetTagData(WA_Top, 0, tags);
    m->im_Width = (uint32_t)GetTagData(WA_Width, bm->bm_Width, tags);
    m->im_Height = (uint32_t)GetTagData(WA_Height, bm->bm_Height, tags);
    m->im_Flags = (uint32_t)GetTagData(WA_Flags, 0, tags);
    m->im_IDCMP = (uint32_t)GetTagData(WA_IDCMP, 0, tags);
    m->im_Buffers = back ? 2 : 1;
    describe(&m->im_Buffer[0], bm);
    if (back)
        describe(&m->im_Buffer[1], back);
    for (unsigned i = 0; i < INTUI_TITLE - 1 && title[i]; i++)
        m->im_Title[i] = title[i];
    for (unsigned b = 0; b < m->im_Buffers; b++) {
        e = ShareArea((b ? back : bm)->bm_Area, in->in_Server, AREAR_READ);
        if (e != 0 && e != ERR_EXISTS)
            return e;
    }
    if ((e = call(in, INTUI_OPEN)) != 0)
        return e;
    *w = (struct Window){
        .w_Intuition = in, .w_Id = m->im_Window, .w_Buffers = m->im_Buffers,
        .w_LeftEdge = m->im_Left, .w_TopEdge = m->im_Top, .w_Width = m->im_Width,
        .w_Height = m->im_Height, .w_BitMap = { bm, back }, .w_Flags = m->im_Flags,
        .w_Sync = (const struct IntuiSync *)m->im_Sync,
        .w_BorderLeft = m->im_Flags & WFLG_BORDERLESS ? 0 : (int32_t)INTUI_BORDER,
        .w_BorderTop = m->im_Flags & WFLG_BORDERLESS ? 0 : (int32_t)INTUI_TITLEBAR,
    };
    rastport(w);
    return 0;
}

int64_t CloseWindow(struct Window *w)
{
    struct Intuition *in = w->w_Intuition;
    int64_t e;

    if (!in || !w->w_Id)
        return ERR_BADARG;
    clear(in->in_Call, sizeof *in->in_Call);
    in->in_Call->im_Window = w->w_Id;
    e = call(in, INTUI_CLOSE);
    w->w_Id = 0;
    return e;
}

/* The answers come back, and any refusal is kept. */
static void reap(struct Intuition *in)
{
    struct Message *r;

    while ((r = GetMsg(&in->in_Port)))
        for (unsigned i = 0; i < INTUI_INFLIGHT; i++)
            if (r == &in->in_Present[i]->im_Msg && in->in_Busy & 1u << i) {
                in->in_Busy &= ~(1u << i);
                if (r->mn_Error && !in->in_Error)
                    in->in_Error = r->mn_Error;
            }
}

/* A present message free, waiting a second at most if none is: the
 * clock read only then, as it's a kernel call. */
static int64_t free_present(struct Intuition *in, unsigned *k)
{
    for (uint64_t deadline = 0;;) {
        reap(in);
        for (unsigned i = 0; i < INTUI_INFLIGHT; i++)
            if (!(in->in_Busy & 1u << i)) {
                *k = i;
                return 0;
            }
        if (PortGone(&in->in_Port))
            return ERR_PORT_DIED;
        if (!deadline)
            deadline = CurrentTime() + CALL_DEADLINE;
        if (ArmPort(&in->in_Port) && !WaitUntil(in->in_Port.mp_SigMask, deadline))
            return ERR_TIMEOUT;
    }
}

static int64_t refused(struct Intuition *in)
{
    int64_t e = in->in_Error;

    in->in_Error = 0;
    return e;
}

int64_t PresentWindow(struct Window *w, const struct GfxRect *damage, uint32_t count)
{
    struct Intuition *in = w->w_Intuition;
    struct IntuiMsg *m;
    unsigned k;
    int64_t e;

    if (!in || !w->w_Id || count > INTUI_RECTS || (count && !damage))
        return ERR_BADARG;
    for (uint32_t i = 0; i < count; i++)
        if (damage[i].MinX < 0 || damage[i].MinY < 0 || damage[i].MinX > damage[i].MaxX ||
            damage[i].MinY > damage[i].MaxY || damage[i].MaxX >= (int64_t)w->w_Width ||
            damage[i].MaxY >= (int64_t)w->w_Height)
            return ERR_BADARG;
    if ((e = free_present(in, &k)) != 0)
        return e;
    m = in->in_Present[k];
    clear(m, sizeof *m);
    m->im_Msg.mn_Length = sizeof *m;
    m->im_Op = INTUI_PRESENT;
    m->im_Window = w->w_Id;
    m->im_Index = w->w_Back;
    m->im_Frame = w->w_Frame + 1;
    m->im_Count = count;
    for (uint32_t i = 0; i < count; i++)
        m->im_Rect[i] = damage[i];
    if ((e = PutMsg(&in->in_Port, &m->im_Msg)) != 0)
        return e;
    in->in_Busy |= 1u << k;
    w->w_Frame++;
    w->w_Back = (w->w_Back + 1) % w->w_Buffers;
    rastport(w);
    return refused(in);
}

int64_t WaitSafe(struct Window *w, uint64_t deadline)
{
    const struct IntuiSync *s = w->w_Sync;
    uint64_t v;

    if (!w->w_Intuition || !w->w_Id)
        return ERR_BADARG;
    while ((v = __atomic_load_n(&s->is_Safe, __ATOMIC_ACQUIRE)) < w->w_Frame) {
        if (PortGone(&w->w_Intuition->in_Port))
            return ERR_PORT_DIED;
        if (WaitAddr(&s->is_Safe, v, deadline) == ERR_TIMEOUT)
            return ERR_TIMEOUT;
    }
    reap(w->w_Intuition);
    return refused(w->w_Intuition);
}

bool GetIntuiEvent(struct Intuition *in, struct IntuiEvent *ev)
{
    const struct IntuiPage *p = in->in_Page;
    uint64_t tail, head;

    if (!p)
        return false;
    tail = in->in_Ack->ia_Tail;
    head = __atomic_load_n(&p->ip_Head, __ATOMIC_ACQUIRE);
    if (tail == head)
        return false;
    *ev = p->ip_Event[tail % INTUI_EVENTS_RING];
    __atomic_store_n(&in->in_Ack->ia_Tail, tail + 1, __ATOMIC_RELEASE);
    if (head - tail == INTUI_EVENTS_RING || head - tail == INTUI_EVENTS_MOVES)
        SignalConn(in->in_Port.mp_Conn);    /* room again for what intuition held back */
    return true;
}

int64_t WaitIntuiEvent(struct Intuition *in, uint64_t deadline)
{
    const struct IntuiPage *p = in->in_Page;
    uint64_t v;

    if (!p)
        return ERR_BADARG;
    while ((v = __atomic_load_n(&p->ip_Head, __ATOMIC_ACQUIRE)) == in->in_Ack->ia_Tail) {
        if (PortGone(&in->in_Port))
            return ERR_PORT_DIED;
        if (WaitAddr(&p->ip_Head, v, deadline) == ERR_TIMEOUT)
            return ERR_TIMEOUT;
    }
    return 0;
}

/*
 * lib/dos/con.c -- dos.library's console: SetConName, OpenCon, WriteCon,
 * PutStr, Printf and ReadCon (planar/dos.h)
 */
#include <stddef.h>

#include <planar/dos.h>
#include <planar/drv.h>
#include <planar/exec.h>
#include <planar/syscalls.h>

/* The process's console. */
static char con_name[CON_NAME] = "con.0";

void SetConName(const char *name)
{
    unsigned i = 0;

    if (!name || !name[0])
        name = "con.0";
    for (; name[i] && i < CON_NAME - 1; i++)
        con_name[i] = name[i];
    con_name[i] = '\0';
}

const char *ConName(void)
{
    return con_name;
}

void OpenCon(struct Con *c, int64_t sigbit)
{
    memset(c, 0, sizeof *c);
    c->cn_SigBit = sigbit;
    if (ConnectPort(&c->cn_Port, con_name, sigbit, 4, 2 * sizeof(struct DrvRequest)) != 0)
        return;
    if (!(c->cn_Msg = AllocMsg(&c->cn_Port, sizeof(struct DrvRequest)))) {
        ClosePort(&c->cn_Port);
        return;
    }
    c->cn_Open = true;
}

void CloseCon(struct Con *c)
{
    if (c->cn_Count) {
        c->cn_Line[c->cn_Count] = '\0';
        KLog(c->cn_Line);
        c->cn_Count = 0;
    }
    if (c->cn_Open)
        ClosePort(&c->cn_Port);
    c->cn_Open = false;
}

void WriteCon(struct Con *c, const char *text, uint32_t len)
{
    while (len) {
        struct DrvRequest *r = c->cn_Msg;
        uint32_t n = len < sizeof r->dr_Data ? len : sizeof r->dr_Data;

        if (c->cn_Open) {
            memset(r, 0, sizeof *r);
            r->dr_Msg.mn_Length = sizeof *r;
            r->dr_Op = DRV_WRITE;
            r->dr_Len = n;
            memcpy(r->dr_Data, text, n);
            int64_t e = DoMsgUntil(&c->cn_Port, &r->dr_Msg, CurrentTime() + CON_DEADLINE_MS * 1000000);

            if (e == 0 && r->dr_Msg.mn_Error == 0) {
                text += n;
                len -= n;
                continue;
            }
            if (e == ERR_TIMEOUT)
                ReportUnresponsive(con_name);
            /* The console went (display restarting): once more on a new
             * connection, else to the log from here. */
            ClosePort(&c->cn_Port);
            c->cn_Open = false;
            if (!c->cn_Retried) {
                c->cn_Retried = true;
                OpenCon(c, c->cn_SigBit);
                c->cn_Retried = true;
                if (c->cn_Open)
                    continue;
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            if (text[i] == '\n' || c->cn_Count == sizeof c->cn_Line - 1) {
                c->cn_Line[c->cn_Count] = '\0';
                KLog(c->cn_Line);
                c->cn_Count = 0;
                if (text[i] == '\n')
                    continue;
            }
            c->cn_Line[c->cn_Count++] = text[i];
        }
        text += n;
        len -= n;
    }
}

void PutStr(struct Con *c, const char *text)
{
    uint32_t n = 0;

    while (text[n])
        n++;
    WriteCon(c, text, n);
}

/* Printf's bytes, gathered into what one message carries. */
struct out {
    struct Con *con;
    uint32_t n;
    char text[sizeof ((struct DrvRequest *)nullptr)->dr_Data];
};

static void put(char ch, void *data)
{
    struct out *o = data;

    if (!ch)
        return;                     /* RawDoFmt's NUL at the end */
    o->text[o->n++] = ch;
    if (o->n == sizeof o->text) {
        WriteCon(o->con, o->text, o->n);
        o->n = 0;
    }
}

uint64_t VPrintf(struct Con *c, const char *format, va_list args)
{
    struct out o = { .con = c };
    uint64_t n = RawDoFmt(format, args, put, &o);

    if (o.n)
        WriteCon(c, o.text, o.n);
    return n;
}

uint64_t Printf(struct Con *c, const char *format, ...)
{
    va_list args;
    uint64_t n;

    va_start(args, format);
    n = VPrintf(c, format, args);
    va_end(args);
    return n;
}

int64_t ReadCon(struct Con *c, char *buffer, uint32_t length)
{
    struct DrvRequest *r = c->cn_Msg;
    int64_t e;

    if (!c->cn_Open)
        return ERR_PORT_DIED;
    memset(r, 0, sizeof *r);
    r->dr_Msg.mn_Length = sizeof *r;
    r->dr_Op = DRV_READ;
    r->dr_Len = length < sizeof r->dr_Data ? length : sizeof r->dr_Data;
    if ((e = DoMsg(&c->cn_Port, &r->dr_Msg)) != 0)
        return e;
    if (r->dr_Msg.mn_Error != 0)
        return r->dr_Msg.mn_Error;
    if (r->dr_Len > length || r->dr_Len > sizeof r->dr_Data)
        return ERR_BADARG;
    memcpy(buffer, r->dr_Data, r->dr_Len);
    return r->dr_Len;
}

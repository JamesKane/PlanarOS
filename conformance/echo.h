/*
 * conformance/echo.h -- the message the echo server and its clients share
 */
#ifndef CONFORMANCE_ECHO_H
#define CONFORMANCE_ECHO_H

#include <planar/msg.h>

struct EchoMsg {
    struct Message em_Msg;
    uint64_t em_Value;          /* the server adds one */
    uint64_t em_Seq;            /* the client's; the server leaves it alone */
};

#endif /* CONFORMANCE_ECHO_H */

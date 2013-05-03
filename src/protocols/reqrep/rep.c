/*
    Copyright (c) 2012-2013 250bpm s.r.o.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "rep.h"
#include "xrep.h"

#include "../../nn.h"
#include "../../reqrep.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/chunkref.h"
#include "../../utils/wire.h"
#include "../../utils/list.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NN_REP_INPROGRESS 1

struct nn_rep {
    struct nn_xrep xrep;
    uint32_t flags;
    struct nn_chunkref backtrace;
};

/*  Private functions. */
static int nn_rep_init (struct nn_rep *self,
    const struct nn_sockbase_vfptr *vfptr, void *hint);
static void nn_rep_term (struct nn_rep *self);

/*  Implementation of nn_sockbase's virtual functions. */
static void nn_rep_close (struct nn_sockbase *self);
static void nn_rep_destroy (struct nn_sockbase *self);
static int nn_rep_events (struct nn_sockbase *self);
static int nn_rep_send (struct nn_sockbase *self, struct nn_msg *msg);
static int nn_rep_recv (struct nn_sockbase *self, struct nn_msg *msg);

static const struct nn_sockbase_vfptr nn_rep_sockbase_vfptr = {
    nn_rep_close,
    nn_rep_destroy,
    nn_xrep_add,
    nn_xrep_rm,
    nn_xrep_in,
    nn_xrep_out,
    nn_rep_events,
    nn_rep_send,
    nn_rep_recv,
    nn_xrep_setopt,
    nn_xrep_getopt
};

static int nn_rep_init (struct nn_rep *self,
    const struct nn_sockbase_vfptr *vfptr, void *hint)
{
    int rc;

    rc = nn_xrep_init (&self->xrep, vfptr, hint);
    if (rc < 0)
        return rc;
    self->flags = 0;

    return 0;
}

static void nn_rep_term (struct nn_rep *self)
{
    if (self->flags & NN_REP_INPROGRESS)
        nn_chunkref_term (&self->backtrace);
    nn_xrep_term (&self->xrep);
}

static void nn_rep_close (struct nn_sockbase *self)
{
    /*  Nothing special to do done. The object is closed straight away. */
    nn_sockbase_closed (self);
}

static void nn_rep_destroy (struct nn_sockbase *self)
{
    struct nn_rep *rep;

    rep = nn_cont (self, struct nn_rep, xrep.sockbase);

    nn_rep_term (rep);
    nn_free (rep);
}

static int nn_rep_events (struct nn_sockbase *self)
{
    struct nn_rep *rep;
    int events;

    rep = nn_cont (self, struct nn_rep, xrep.sockbase);
    events = nn_xrep_events (&rep->xrep.sockbase);
    if (!(rep->flags & NN_REP_INPROGRESS))
        events &= ~NN_SOCKBASE_EVENT_OUT;
    return events;
}

static int nn_rep_send (struct nn_sockbase *self, struct nn_msg *msg)
{
    int rc;
    struct nn_rep *rep;

    rep = nn_cont (self, struct nn_rep, xrep.sockbase);

    /*  If no request was received, there's nowhere to send the reply to. */
    if (nn_slow (!(rep->flags & NN_REP_INPROGRESS)))
        return -EFSM;

    /*  Move the stored backtrace into the message header. */
    nn_assert (nn_chunkref_size (&msg->hdr) == 0);
    nn_chunkref_term (&msg->hdr);
    nn_chunkref_mv (&msg->hdr, &rep->backtrace);
    rep->flags &= ~NN_REP_INPROGRESS;

    /*  Send the reply. If it cannot be sent because of pushback,
        drop it silently. */
    rc = nn_xrep_send (&rep->xrep.sockbase, msg);
    errnum_assert (rc == 0 || rc == -EAGAIN, -rc);

    return 0;
}

static int nn_rep_recv (struct nn_sockbase *self, struct nn_msg *msg)
{
    int rc;
    struct nn_rep *rep;

    rep = nn_cont (self, struct nn_rep, xrep.sockbase);

    /*  If a request is already being processed, cancel it. */
    if (nn_slow (rep->flags & NN_REP_INPROGRESS)) {
        nn_chunkref_term (&rep->backtrace);
        rep->flags &= ~NN_REP_INPROGRESS;
    }

    /*  Receive the request. */
    rc = nn_xrep_recv (&rep->xrep.sockbase, msg);
    if (nn_slow (rc == -EAGAIN))
        return -EAGAIN;
    errnum_assert (rc == 0, -rc);

    /*  Store the backtrace. */
    nn_chunkref_mv (&rep->backtrace, &msg->hdr);
    nn_chunkref_init (&msg->hdr, 0);
    rep->flags |= NN_REP_INPROGRESS;

    return 0;
}

static int nn_rep_create (void *hint, struct nn_sockbase **sockbase)
{
    int rc;
    struct nn_rep *self;

    self = nn_alloc (sizeof (struct nn_rep), "socket (rep)");
    alloc_assert (self);
    rc = nn_rep_init (self, &nn_rep_sockbase_vfptr, hint);
    if (rc < 0) {
        nn_free (self);
        return rc;
    }
    *sockbase = &self->xrep.sockbase;

    return 0;
}

static struct nn_socktype nn_rep_socktype_struct = {
    AF_SP,
    NN_REP,
    0,
    nn_rep_create,
    nn_xrep_ispeer,
    NN_LIST_ITEM_INITIALIZER
};

struct nn_socktype *nn_rep_socktype = &nn_rep_socktype_struct;


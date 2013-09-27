/*
    Copyright (c) 2012-2013 250bpm s.r.o.  All rights reserved.
    Copyright (c) 2013 GoPivotal, Inc.  All rights reserved.

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

#include "tcpmux.h"
//#include "btcpmux.h"
#include "ctcpmux.h"

#include "../../tcpmux.h"

/*  nn_transport interface. */
static int nn_tcpmux_bind (void *hint, struct nn_epbase **epbase);
static int nn_tcpmux_connect (void *hint, struct nn_epbase **epbase);

static struct nn_transport nn_tcpmux_vfptr = {
    "tcpmux",
    NN_TCPMUX,
    NULL,
    NULL,
    nn_tcpmux_bind,
    nn_tcpmux_connect,
    NULL,
    NN_LIST_ITEM_INITIALIZER
};

struct nn_transport *nn_tcpmux = &nn_tcpmux_vfptr;

static int nn_tcpmux_bind (void *hint, struct nn_epbase **epbase)
{
//    return nn_btcpmux_create (hint, epbase);
return -1;
}

static int nn_tcpmux_connect (void *hint, struct nn_epbase **epbase)
{
    return nn_ctcpmux_create (hint, epbase);
}


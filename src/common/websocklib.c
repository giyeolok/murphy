#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <murphy/common/macros.h>
#include <murphy/common/mm.h>
#include <murphy/common/log.h>
#include <murphy/common/refcnt.h>
#include <murphy/common/mainloop.h>
#include <murphy/common/fragbuf.h>

#include "websocklib.h"

#define LWS_EVENT_OK    0                /* event handler result: ok */
#define LWS_EVENT_DENY  1                /* event handler result: deny */
#define LWS_EVENT_ERROR 1                /* event handler result: error */

/* libwebsocket status used to close sockets upon error */
#define LWS_INTERNAL_ERROR LWS_CLOSE_STATUS_UNEXPECTED_CONDITION

/* SSL modes */
#define LWS_NO_SSL         0             /* no SSL at all */
#define LWS_SSL            1             /* SSL, deny self-signed certs */
#define LWS_SSL_SELFSIGNED 2             /* SSL, allow self-signed certs */

/*
 * define shorter aliasen for libwebsocket types
 */

typedef struct libwebsocket                  lws_t;
typedef struct libwebsocket_context          lws_ctx_t;
typedef struct libwebsocket_extension        lws_ext_t;
typedef struct libwebsocket_protocols        lws_proto_t;
typedef enum   libwebsocket_callback_reasons lws_event_t;


/*
 * a libwebsocket fd we (e)poll
 *
 * Unfortunately the mechanism offered by libwebsockets for external
 * mainloop integration uses event mask diffs when asking the mainloop
 * to modify what an fd is polled for. This forces us to do double
 * bookkeeping: we need to to keep track of the current event mask for
 * all descriptors just to figure out the new mask when libwebsockets
 * hands us a diff.
 */

typedef struct {
    int      fd;                         /* libwebsocket file descriptor */
    uint32_t events;                     /* monitored (epoll) events */
} pollfd_t;


/*
 * a websocket context
 */

struct wsl_ctx_s {
    lws_ctx_t       *ctx;                 /* libwebsocket context */
    wsl_proto_t     *protos;              /* protocols */
    int              nproto;              /* number of protocols */
    lws_proto_t     *lws_protos;          /* libwebsocket protocols */
    mrp_refcnt_t     refcnt;              /* reference count */
    int              epollfd;             /* epoll descriptor */
    mrp_io_watch_t  *w;                   /* I/O watch for epollfd */
    mrp_mainloop_t  *ml;                  /* pumping mainloop */
    pollfd_t        *fds;                 /* polled descriptors */
    int              nfd;                 /* number descriptors */
    void            *user_data;           /* opaque user data */
    lws_t           *pending;             /* pending connection */
    void            *pending_user;        /* user_data of pending */
    wsl_proto_t     *pending_proto;       /* protocol of pending */
    int              has_http;            /* has HTTP as upper layer protocol */
    mrp_list_hook_t  pure_http;           /* pure HTTP sockets */
};

/*
 * a websocket instance
 */

struct wsl_sck_s {
    wsl_ctx_t       *ctx;                /* associated context */
    lws_t           *sck;                /* libwebsocket instance */
    wsl_proto_t     *proto;              /* protocol data */
    wsl_sendmode_t   send_mode;          /* libwebsocket write mode */
    mrp_fragbuf_t   *buf;                /* fragment collection buffer */
    void            *user_data;          /* opaque user data */
    wsl_sck_t      **sckptr;             /* back pointer from sck to us */
    int              closing : 1;        /* close in progress */
    int              pure_http : 1;      /* pure HTTP socket */
    int              busy;               /* upper-layer callback(s) active */
    mrp_list_hook_t  hook;               /* to pure HTTP list, if such */
};


/*
 * mark a socket busy while executing a piece of code
 */

#define SOCKET_BUSY_REGION(sck, ...) do {       \
        (sck)->busy++;                          \
        __VA_ARGS__;                            \
        (sck)->busy--;                          \
    } while (0)



static int http_event(lws_ctx_t *ws_ctx, lws_t *ws, lws_event_t event,
                      void *user, void *in, size_t len);
static int wsl_event(lws_ctx_t *ws_ctx, lws_t *ws, lws_event_t event,
                     void *user, void *in, size_t len);
static void destroy_context(wsl_ctx_t *ctx);



static inline uint32_t map_poll_to_event(int in)
{
    uint32_t mask = 0;

    if (in & POLLIN)  mask |= MRP_IO_EVENT_IN;
    if (in & POLLOUT) mask |= MRP_IO_EVENT_OUT;
    if (in & POLLHUP) mask |= MRP_IO_EVENT_HUP;
    if (in & POLLERR) mask |= MRP_IO_EVENT_ERR;

    return mask;

}


static inline short map_event_to_poll(uint32_t in)
{
    short mask = 0;

    if (in & MRP_IO_EVENT_IN)  mask |= POLLIN;
    if (in & MRP_IO_EVENT_OUT) mask |= POLLOUT;
    if (in & MRP_IO_EVENT_HUP) mask |= POLLHUP;
    if (in & MRP_IO_EVENT_ERR) mask |= POLLERR;

    return mask;
}


static int add_fd(wsl_ctx_t *wsc, int fd, int events)
{
    struct epoll_event e;

    if (wsc != NULL) {
        e.data.u64 = 0;
        e.data.fd  = fd;
        e.events   = map_poll_to_event(events);

        if (epoll_ctl(wsc->epollfd, EPOLL_CTL_ADD, fd, &e) == 0) {
            if (mrp_reallocz(wsc->fds, wsc->nfd, wsc->nfd + 1) != NULL) {
                wsc->fds[wsc->nfd].fd     = fd;
                wsc->fds[wsc->nfd].events = e.events;
                wsc->nfd++;

                return TRUE;
            }
            else
                epoll_ctl(wsc->epollfd, EPOLL_CTL_DEL, fd, &e);
        }
    }

    return FALSE;
}


static int del_fd(wsl_ctx_t *wsc, int fd)
{
    struct epoll_event e;
    int                i;

    if (wsc != NULL) {
        e.data.u64 = 0;
        e.data.fd  = fd;
        e.events   = 0;
        epoll_ctl(wsc->epollfd, EPOLL_CTL_DEL, fd, &e);

        for (i = 0; i < wsc->nfd; i++) {
            if (wsc->fds[i].fd == fd) {
                if (i < wsc->nfd - 1)
                    memmove(wsc->fds + i, wsc->fds + i + 1,
                            (wsc->nfd - i - 1) * sizeof(*wsc->fds));

                mrp_reallocz(wsc->fds, wsc->nfd, wsc->nfd - 1);
                wsc->nfd--;

                return TRUE;
            }
        }
    }

    return FALSE;
}


static pollfd_t *find_fd(wsl_ctx_t *wsc, int fd)
{
    int i;

    if (wsc != NULL) {
        for (i = 0; i < wsc->nfd; i++)
            if (wsc->fds[i].fd == fd)
                return wsc->fds + i;
    }

    return NULL;
}


static int mod_fd(wsl_ctx_t *wsc, int fd, int events, int clear)
{
    struct epoll_event  e;
    pollfd_t           *wfd;

    if (wsc != NULL) {
        wfd = find_fd(wsc, fd);

        if (wfd != NULL) {
            e.data.u64 = 0;
            e.data.fd  = fd;

            if (clear)
                e.events = wfd->events & ~map_poll_to_event(events);
            else
                e.events = wfd->events |  map_poll_to_event(events);

            if (epoll_ctl(wsc->epollfd, EPOLL_CTL_MOD, fd, &e) == 0)
                return TRUE;
        }
    }

    return FALSE;
}


static void purge_fds(wsl_ctx_t *wsc)
{
    if (wsc != NULL) {
        mrp_free(wsc->fds);
        wsc->fds = NULL;
        wsc->nfd = 0;
    }
}


static void epoll_event(mrp_mainloop_t *ml, mrp_io_watch_t *w, int fd,
                        mrp_io_event_t mask, void *user_data)
{
    wsl_ctx_t          *wsc = (wsl_ctx_t *)user_data;
    pollfd_t           *wfd;
    struct epoll_event *events, *e;
    int                 nevent, n, i;
    struct pollfd       pollfd;

    MRP_UNUSED(ml);
    MRP_UNUSED(w);
    MRP_UNUSED(fd);

    if (wsc->nfd <= 0 || !(mask & MRP_IO_EVENT_IN))
        return;

    nevent = wsc->nfd;
    events = alloca(nevent * sizeof(*events));

    while ((n = epoll_wait(wsc->epollfd, events, nevent, 0)) > 0) {
        mrp_debug("got %d epoll events for websocket context %p", n, wsc);

        for (i = 0, e = events; i < n; i++, e++) {
            wfd = find_fd(wsc, e->data.fd);

            if (wfd != NULL) {
                pollfd.fd      = wfd->fd;
                pollfd.events  = map_event_to_poll(wfd->events);
                pollfd.revents = map_event_to_poll(e->events);

                mrp_debug("delivering events 0x%x to websocket fd %d",
                          pollfd.revents, pollfd.fd);

                libwebsocket_service_fd(wsc->ctx, &pollfd);
            }
        }
    }
}


/*
 * context handling
 */

wsl_ctx_t *wsl_create_context(mrp_mainloop_t *ml, struct sockaddr *addr,
                              wsl_proto_t *protos, int nproto, void *user_data)
{
    lws_ext_t      *builtin = libwebsocket_internal_extensions;
    wsl_ctx_t      *ctx;
    wsl_proto_t    *up;
    lws_proto_t    *lws_protos, *lp;
    int             lws_nproto, has_http;
    mrp_io_event_t  events;
    const char     *dev;
    int             port, i;


    if (addr == NULL) {
        dev  = NULL;
        port = 0;
    }
    else {
        switch (addr->sa_family) {
        case AF_INET:
            dev  = NULL;
            port = (int)ntohs(((struct sockaddr_in *)addr)->sin_port);
            break;

        case AF_INET6:
            dev  = NULL;
            port = (int)ntohs(((struct sockaddr_in6 *)addr)->sin6_port);
            break;

        default:
            errno = EINVAL;
            return NULL;
        }
    }

    ctx = mrp_allocz(sizeof(*ctx));

    if (ctx == NULL)
        goto fail;

    mrp_refcnt_init(&ctx->refcnt);
    mrp_list_init(&ctx->pure_http);

    ctx->protos = protos;
    ctx->nproto = nproto;

    has_http   = !strncmp(protos[0].name, "http", 4) ? 1 : 0;
    lws_nproto = (has_http ? nproto : nproto + 1) + 1;
    lws_protos = mrp_allocz_array(lws_proto_t, lws_nproto);

    if (lws_protos == NULL)
        goto fail;

    lws_protos[0].name     = "http";
    lws_protos[0].callback = http_event;
    if (!has_http)
        lws_protos[0].per_session_data_size = sizeof(void *);
    else
        lws_protos[0].per_session_data_size = sizeof(void *);

    lp = lws_protos + 1;
    up = protos + (has_http ? 1 : 0);

    for (i = has_http; i < nproto; i++) {
        lp->name                  = up->name;
        lp->callback              = wsl_event;
        lp->per_session_data_size = sizeof(void *);

        lp++;
        up++;
    }

    ctx->lws_protos = lws_protos;
    ctx->has_http   = has_http;

    ctx->epollfd = epoll_create1(EPOLL_CLOEXEC);

    if (ctx->epollfd < 0)
        goto fail;

    events  = MRP_IO_EVENT_IN;
    ctx->ml = ml;
    ctx->w  = mrp_add_io_watch(ml, ctx->epollfd, events, epoll_event, ctx);

    if (ctx->w == NULL)
        goto fail;

    ctx->ctx = libwebsocket_create_context(port, dev, lws_protos, builtin,
                                           NULL, NULL, NULL, -1, -1, 0,
                                           ctx);

    if (ctx->ctx != NULL) {
        ctx->user_data = user_data;

        return ctx;
    }

 fail:
    if (ctx != NULL) {
        if (ctx->epollfd >= 0) {
            mrp_del_io_watch(ctx->w);
            close(ctx->epollfd);
        }

        mrp_free(ctx);
    }

    return NULL;
}


wsl_ctx_t *wsl_ref_context(wsl_ctx_t *ctx)
{
    return mrp_ref_obj(ctx, refcnt);
}


int wsl_unref_context(wsl_ctx_t *ctx)
{
    if (mrp_unref_obj(ctx, refcnt)) {
        destroy_context(ctx);

        return TRUE;
    }
    else
        return FALSE;
}


static void destroy_context(wsl_ctx_t *ctx)
{
    if (ctx != NULL) {
        mrp_del_io_watch(ctx->w);
        ctx->w = NULL;

        close(ctx->epollfd);
        ctx->epollfd = -1;

        purge_fds(ctx);

        if (ctx->ctx != NULL)
            libwebsocket_context_destroy(ctx->ctx);

        mrp_free(ctx->lws_protos);
        mrp_free(ctx);
    }
}


static wsl_proto_t *find_context_protocol(wsl_ctx_t *ctx, const char *protocol)
{
    wsl_proto_t *up;
    int          i;

    if (protocol != NULL) {
        for (i = 0, up = ctx->protos; i < ctx->nproto; i++, up++)
            if (!strcmp(up->name, protocol))
                return up;
    }

    return NULL;
}


static wsl_sck_t *find_pure_http(wsl_ctx_t *ctx, lws_t *ws)
{
    mrp_list_hook_t *p, *n;
    wsl_sck_t       *sck;

    /*
     * Notes:
     *     We expect an extremely low number of concurrent pure
     *     HTTP connections so we do asimple linear search here.
     *     We can change this if this turns out to be a false
     *     assumption.
     */

    mrp_list_foreach(&ctx->pure_http, p, n) {
        sck = mrp_list_entry(p, typeof(*sck), hook);

        if (sck->sck == ws)
            return sck;
    }

    return NULL;
}


wsl_sck_t *wsl_connect(wsl_ctx_t *ctx, struct sockaddr *sa,
                       const char *protocol, void *user_data)
{
    wsl_sck_t   *sck, **ptr;
    wsl_proto_t *up;
    int          port;
    void        *aptr;
    char         abuf[256];
    const char  *astr;

    switch (sa->sa_family) {
    case AF_INET:
        aptr = &((struct sockaddr_in *)sa)->sin_addr;
        port = ntohs(((struct sockaddr_in *)sa)->sin_port);
        break;
    case AF_INET6:
        aptr = &((struct sockaddr_in6 *)sa)->sin6_addr;
        port = ntohs(((struct sockaddr_in6 *)sa)->sin6_port);
        break;
    default:
        errno = EINVAL;
        return NULL;
    }

    astr = inet_ntop(sa->sa_family, aptr, abuf, sizeof(abuf));

    if (astr == NULL)
        return NULL;

    up = find_context_protocol(ctx, protocol);

    if (up == NULL) {
        errno = ENOPROTOOPT;
        return NULL;
    }

    sck = mrp_allocz(sizeof(*sck));
    ptr = mrp_allocz(sizeof(*ptr));

    if (sck != NULL && ptr != NULL) {
        /*
         * Now we need to create and connect a new libwebsocket instance
         * within the given context. We also need to set up a one-to-one
         * mapping between the underlying libwebsocket and our wsl_sck_t
         * so that we can handle both top-down (sending) and bottom-up
         * (receiving) event propagation in the stack.
         *
         * We use the user data associated with the libwebsocket instance
         * to store a back pointer to us. Whenever the socket instance
         * is deleted locally (as opposed to our peer closing the session)
         * we need to prevent the propagation of any potentially pending
         * events to our deleted wsl_sck_t (which might have been freed).
         * This we do by clearing the back pointer from the instance to us.
         *
         * However, since libwebsockets does not provide an API for this,
         * as a trick we use an indirect back pointer and store a pointer
         * to the actual back pointer also in wsl_sck_t here. This way we
         * can always clear the back pointer when we need to.
         *
         * Also note, that memory management for the associated user data
         * is asymmetric in many sense. For client connections, we allocate
         * the data buffer and pass it on to libwebsockets. For incoming
         * connections the user data buffer is allocated by libwebsockets
         * and we only get a chance to fill it in the event handler for
         * connection establishment. However, for both incoming and outgoing
         * connections libwebsockets will free the buffer on behalf of us.
         *
         * The exact same notes apply to wsl_accept_pending below...
         */

        mrp_list_init(&sck->hook);
        sck->ctx   = wsl_ref_context(ctx);
        sck->proto = up;
        sck->buf   = mrp_fragbuf_create(/*up->framed*/TRUE, 0);

        if (sck->buf != NULL) {
            sck->user_data = user_data;

            if (strncmp(protocol, "http", 4)) { /* Think harder, Homer ! */
                *ptr           = sck;
                sck->sckptr    = ptr;
            }
            else
                mrp_list_append(&ctx->pure_http, &sck->hook);

            sck->sck = libwebsocket_client_connect_extended(ctx->ctx,
                                                            astr, port,
                                                            LWS_NO_SSL,
                                                            "/", astr, astr,
                                                            protocol, -1,
                                                            ptr);

            if (sck->sck != NULL)
                return sck;

            mrp_fragbuf_destroy(sck->buf);
            mrp_list_delete(&sck->hook);
        }

        wsl_unref_context(ctx);
        mrp_free(ptr);
        mrp_free(sck);
    }

    return NULL;
}


wsl_sck_t *wsl_accept_pending(wsl_ctx_t *ctx, void *user_data)
{
    wsl_sck_t  *sck, **ptr;

    if (ctx->pending == NULL || ctx->pending_proto == NULL)
        return NULL;

    mrp_debug("accepting pending websocket connection %p/%p", ctx->pending,
              ctx->pending_user);

    sck = mrp_allocz(sizeof(*sck));

    if (sck != NULL) {
        mrp_list_init(&sck->hook);

        /*
         * Notes:
         *     The same notes apply here for context creation as for
         *     wsl_connect above...
         */
        sck->ctx = wsl_ref_context(ctx);
        sck->buf = mrp_fragbuf_create(/*ctx->pending_proto->framed*/TRUE, 0);

        if (sck->buf != NULL) {
            sck->proto     = ctx->pending_proto;
            sck->user_data = user_data;
            sck->sck       = ctx->pending;
            ptr            = (wsl_sck_t **)ctx->pending_user;
            sck->sckptr    = ptr;

            if (ptr != NULL)             /* genuine websocket */
                *ptr = sck;
            else                         /* pure http socket */
                mrp_list_append(&ctx->pure_http, &sck->hook);

            /* let the event handler know we accepted the client */
            ctx->pending       = NULL;
            /* for pure http communicate sck back in pending_user */
            ctx->pending_user  = (ptr == NULL ? sck : NULL);
            ctx->pending_proto = NULL;

            return sck;
        }

        wsl_unref_context(ctx);
        mrp_free(sck);
    }

    return NULL;
}


void wsl_reject_pending(wsl_ctx_t *ctx)
{
    mrp_debug("reject pending websocket (%s) connection %p/%p",
              ctx->pending_proto->name, ctx->pending, ctx->pending_user);

    /*
     * Nothing to do here really... just don't clear ctx->pending so the
     * event handler will know to reject once it regains control.
     */
}


void *wsl_close(wsl_sck_t *sck)
{
    wsl_ctx_t *ctx;
    void      *user_data;
    int        status;

    user_data = NULL;

    if (sck != NULL) {
        if (sck->sck != NULL && sck->busy <= 0) {
            mrp_debug("closing websocket %p/%p", sck, sck->sck);

            status = LWS_CLOSE_STATUS_NORMAL;
            ctx    = sck->ctx;

            sck->closing = TRUE;
            libwebsocket_close_and_free_session(ctx->ctx, sck->sck, status);
            sck->sck     = NULL;

            if (sck->sckptr != NULL)     /* genuine websocket */
                *sck->sckptr = NULL;
            else                         /* pure http socket */
                mrp_list_delete(&sck->hook);

            if (ctx != NULL) {
                user_data = ctx->user_data;
                wsl_unref_context(ctx);
                sck->ctx = NULL;
            }

            mrp_fragbuf_destroy(sck->buf);
            sck->buf = NULL;

            mrp_debug("freeing websocket %p", sck);
            mrp_free(sck);
        }
        else {
            mrp_debug("marking websocket %p/%p for closing", sck, sck->sck);
            sck->closing = TRUE;
        }
    }

    return user_data;
}


static int check_closed(wsl_sck_t *sck)
{
    if (sck->closing && sck->busy <= 0) {
        wsl_close(sck);
        return TRUE;
    }
    else
        return FALSE;
}


int wsl_set_sendmode(wsl_sck_t *sck, wsl_sendmode_t mode)
{
    const char *name;

    switch (mode) {
    case WSL_SEND_TEXT:   name = "text";   break;
    case WSL_SEND_BINARY: name = "binary"; break;
    default:                               return FALSE;
    }

    mrp_debug("websocket %p/%p mode changed to %s", sck->sck, sck, name);
    sck->send_mode = mode;

    return TRUE;
}


int wsl_send(wsl_sck_t *sck, void *payload, size_t size)
{
    unsigned char *buf;
    size_t         pre, post, total;
    uint32_t      *len;

    if (sck != NULL && sck->sck != NULL) {
        if (sck->proto->framed) {
            pre  = LWS_SEND_BUFFER_PRE_PADDING;
            post = LWS_SEND_BUFFER_POST_PADDING;
            buf  = alloca(pre + sizeof(*len) + size + post);
            len  = (uint32_t *)(buf + pre);
            *len = htobe32(size);

            memcpy(buf + pre + sizeof(*len), payload, size);
            total = sizeof(*len) + size;
        }
        else {
            pre  = LWS_SEND_BUFFER_PRE_PADDING;
            post = LWS_SEND_BUFFER_POST_PADDING;
            buf  = alloca(pre + size + post);

            memcpy(buf + pre, payload, size);
            total = size;
        }

#if (WSL_SEND_TEXT != 0)
        if (!sck->send_mode)
            sck->send_mode = WSL_SEND_TEXT;
#endif

        if (libwebsocket_write(sck->sck, buf + pre, total, sck->send_mode) >= 0)
            return TRUE;
    }

    return FALSE;
}


int wsl_serve_http_file(wsl_sck_t *sck, const char *path, const char *type)
{
    mrp_debug("serving file '%s' (%s) over websocket %p", path, type, sck->sck);

    if (libwebsockets_serve_http_file(sck->ctx->ctx, sck->sck, path, type) == 0)
        return TRUE;
    else
        return FALSE;
}

static int http_event(lws_ctx_t *ws_ctx, lws_t *ws, lws_event_t event,
                      void *user, void *in, size_t len)
{
    wsl_ctx_t   *ctx = libwebsocket_context_user(ws_ctx);
    wsl_sck_t   *sck;
    wsl_proto_t *up;
    const char  *ext, *uri;
    int          fd, mask, status, accepted;

    switch (event) {
    case LWS_CALLBACK_ESTABLISHED:
        mrp_debug("client-handshake completed on websocket %p/%p", ws, user);
        return LWS_EVENT_OK;

    case LWS_CALLBACK_CLOSED:
        mrp_debug("websocket %p/%p closed", ws, user);
        return LWS_EVENT_OK;

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        mrp_debug("server-handshake completed on websocket %p/%p", ws, user);
        return LWS_EVENT_OK;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        mrp_debug("client connection failed");
        return LWS_EVENT_OK;

    case LWS_CALLBACK_RECEIVE:
        mrp_debug("received HTTP data from client");
        return LWS_EVENT_OK;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        mrp_debug("recived HTTP data from server");
        return LWS_EVENT_OK;

    case LWS_CALLBACK_BROADCAST:
        mrp_debug("denying broadcast");
        return LWS_EVENT_DENY;

    case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
        mrp_debug("client received pong");
        return LWS_EVENT_OK;

        /*
         * mainloop integration
         */
    case LWS_CALLBACK_ADD_POLL_FD:
        fd   = (ptrdiff_t)user;
        mask = (int)len;
        mrp_debug("start polling fd %d for events 0x%x", fd, mask);
        if (add_fd(ctx, fd, mask))
            return LWS_EVENT_OK;
        else
            return LWS_EVENT_ERROR;

    case LWS_CALLBACK_DEL_POLL_FD:
        fd = (ptrdiff_t)user;
        mrp_debug("stop polling fd %d", fd);
        if (del_fd(ctx, fd))
            return LWS_EVENT_OK;
        else
            return LWS_EVENT_ERROR;

    case LWS_CALLBACK_SET_MODE_POLL_FD:
        fd   = (ptrdiff_t)user;
        mask = (int)len;
        mrp_debug("enable poll events 0x%x for fd %d", mask, fd);
        if (mod_fd(ctx, fd, mask, FALSE))
            return LWS_EVENT_OK;
        else
            return LWS_EVENT_ERROR;

    case LWS_CALLBACK_CLEAR_MODE_POLL_FD:
        fd   = (ptrdiff_t)user;
        mask = (int)len;
        mrp_debug("disable poll events 0x%x for fd %d", mask, fd);
        if (mod_fd(ctx, fd, mask, TRUE))
            return LWS_EVENT_OK;
        else
            return LWS_EVENT_ERROR;

    case LWS_CALLBACK_SERVER_WRITEABLE:
        mrp_debug("socket server side writeable again");
        return LWS_EVENT_OK;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        mrp_debug("socket client side writeable again");
        return LWS_EVENT_OK;

        /*
         * clients wanting to stay pure HTTP clients
         *
         * Notes:
         *     Clients that stay pure HTTP clients (ie. do not negotiate a
         *     websocket connection) never get an LWS_CALLBACK_ESTABLISHED
         *     event emitted for. This is a bit unfortunate, since that is
         *     the event we map to the incoming connection event of our
         *     transport layer.
         *
         *     However, we'd really like to keep pure HTTP and websocket
         *     connections as much equal as possible. First and foremost
         *     this means that we'd like to associate our own websocklib
         *     wsl_sck_t socket context to lws_t and vice versa. Also
         *     similarly to websocket connections we want to give the upper
         *     layer a chance to accept or reject the connection.
         *
         *     Since there is no ESTABLISHED event for pure HTTP clients,
         *     we have to emulate one such here. We need to check if test
         *     ws belongs to a known connection by checking if it has an
         *     associated wsl_sck_t. If not we need to call the upper layer
         *     to let it accept or reject the connection. If it has already
         *     we need to call the reception handler of the upper layer.
         *
         *     However, unfortunately libwebsockets never allocates user
         *     data for the HTTP websockets even we specify a non-zero size
         *     for protocol 0. Hence, we cannot use our normal mechanism of
         *     associating the upper layer wsl_sck_t context using the ws
         *     user data. Instead we need to separately keep track of HTTP
         *     websockets and look up the associated wsl_sck_t using this
         *     secondary bookkeeping.
         */

    case LWS_CALLBACK_HTTP:
        uri = (const char *)in;

        if (!ctx->has_http) {
            mrp_debug("denying HTTP request of '%s' for httpless context", uri);
            return LWS_EVENT_DENY;
        }

        sck = find_pure_http(ctx, ws);

        if (sck != NULL) {               /* known socket, deliver event */
        deliver_event:
            up = sck->proto;

            if (up != NULL) {
                SOCKET_BUSY_REGION(sck, {
                        up->cbs.recv(sck, in, strlen(uri), sck->user_data,
                                     up->proto_data);
                        up->cbs.check(sck, sck->user_data, up->proto_data);
                    });

                if (check_closed(sck))
                    return 0;
            }

            status = LWS_EVENT_OK;
        }
        else {                           /* unknown socket, needs to accept */
            if (ctx->pending != NULL) {
                mrp_log_error("Multiple pending connections, rejecting.");
                return LWS_EVENT_DENY;
            }

            up = &ctx->protos[0];


            ctx->pending       = ws;
            ctx->pending_user  = NULL;
            ctx->pending_proto = up;

            wsl_ref_context(ctx);
            up->cbs.connection(ctx, "XXX TODO dig out peer address", up->name,
                               ctx->user_data, up->proto_data);
            sck = ctx->pending_user;
            ctx->pending_user = NULL;

            /* XXX TODO
             * check if sockets gets properly closed and freed if
             * cb->connection calls close on the 'listening' websocket in
             * the transport layer...
             */

            accepted = (ctx->pending == NULL);
            wsl_unref_context(ctx);

            if (accepted)
                goto deliver_event;
            else
                status = LWS_EVENT_DENY;
        }

        return status;

    case LWS_CALLBACK_HTTP_FILE_COMPLETION:
        uri = (const char *)in;
        mrp_debug("serving '%s' over HTTP completed", uri);
        return LWS_EVENT_OK;

        /*
         * events always routed to protocols[0]
         */

    case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:
        fd = (ptrdiff_t)user;
        /* we don't filter based on the socket/address */
        return LWS_EVENT_OK;

    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
        /* we don't filter based on headers */
        return LWS_EVENT_OK;

    case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
    case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_SERVER_VERIFY_CERTS:
    case LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION:
        /* we don't support or do anything for SSL at the moment */
        return LWS_EVENT_OK;

    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
        /* no extra headers we'd like to add */
        return LWS_EVENT_OK;

    case LWS_CALLBACK_CONFIRM_EXTENSION_OKAY:
        ext = (const char *)in;
        /* deny all extensions on the server side */
        mrp_debug("denying server extension '%s'", ext);
        return LWS_EVENT_DENY;

    case LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED:
        ext = (const char *)in;
        /* deny all extensions on the client side */
        mrp_debug("denying client extension '%s'", ext);
        return LWS_EVENT_DENY;

    }

    return LWS_EVENT_DENY;
}


static int wsl_event(lws_ctx_t *ws_ctx, lws_t *ws, lws_event_t event,
                     void *user, void *in, size_t len)
{
    wsl_ctx_t   *ctx = libwebsocket_context_user(ws_ctx);
    wsl_sck_t   *sck;
    wsl_proto_t *up;
    void        *data;
    size_t       size;
    uint32_t     total;
    const char  *ext;
    lws_proto_t *proto;
    int          status;

    MRP_UNUSED(ext);
    MRP_UNUSED(ws_ctx);

    switch (event) {
    case LWS_CALLBACK_ESTABLISHED:
        mrp_debug("client-handshake completed on websocket %p/%p", ws, user);

        /*
         * Connection acceptance is a bit tricky. Once libwebsockets
         * has completed its handshaking phase with the client it lets
         * us know about a new established connection. This is what we
         * want to map to an incoming connection attempt. Since we don't
         * want to know about the internals of the upper layer, neither
         * want the upper layer to know about our internals, the only
         * way to pass information about the connection around in the
         * context at this point.
         *
         * To keep things simple we only prepare and handle once
         * outstanding connection attemp at a time. This is equivalent
         * to listening on a stream-socket with a backlog of 1. Since we
         * run single-threaded it shouldn't ever be possible to have more
         * than one pending connection if the upper layer does things
         * right but we do check for this and reject multiple pending
         * connections here...
         *
         * We store the pending websocket instance and its associated
         * user data in the context then call the connection notifier
         * callback. If the upper layer wants to accept the connection
         * it calls wsl_accept_pending. That in turn digs these out from
         * the context to set up and hook together things properly. If all
         * goes fine wsl_accept_pending clears pending and pending_user
         * from the context. If something fails or the upper layer decides
         * not to accept the connection, pending and pending_user stay
         * intact in which case we'll reject the client here once the
         * callback returns.
         */

        if (ctx->pending != NULL) {
            mrp_log_error("Multiple pending connections, rejecting.");
            return LWS_EVENT_DENY;
        }


        proto = (lws_proto_t *)libwebsockets_get_protocol(ws);
        up    = find_context_protocol(ctx, proto->name);

        if (up == NULL) {
            mrp_debug("unknown protocol '%s' requested, rejecting",
                      proto ? proto->name : "<none>");
            return LWS_EVENT_DENY;
        }
        else
            mrp_debug("found descriptor %p for protocol '%s'", up, up->name);

        ctx->pending       = ws;
        ctx->pending_user  = user;
        ctx->pending_proto = up;

        wsl_ref_context(ctx);
        up->cbs.connection(ctx, "XXX TODO dig out peer address", up->name,
                           ctx->user_data, up->proto_data);

        /* XXX TODO
         * check if sockets gets properly closed and freed if
         * cb->connection calls close on the 'listening' websocket in
         * the transport layer...
         */

        if (ctx->pending == NULL)        /* connection accepted */
            status = LWS_EVENT_OK;
        else                             /* connection rejected */
            status = LWS_EVENT_DENY;
        wsl_unref_context(ctx);

        return status;

    case LWS_CALLBACK_CLOSED:
        proto = (lws_proto_t *)libwebsockets_get_protocol(ws);
        up    = find_context_protocol(ctx, proto->name);
        mrp_debug("websocket %p/%p (%s) closed", ws, user,
                  up ? up->name : "<unknown>");

        sck = *(wsl_sck_t **)user;
        up  = sck ? sck->proto : NULL;

        if (up != NULL) {
            SOCKET_BUSY_REGION(sck, {
                    up->cbs.closed(sck, 0, sck->user_data, up->proto_data);
                    up->cbs.check(sck, sck->user_data, up->proto_data);
                });

            check_closed(sck);
        }
        return LWS_EVENT_OK;

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        mrp_debug("server-handshake completed on websocket %p/%p", ws, user);
        return LWS_EVENT_OK;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        mrp_debug("client connection failed");
        return LWS_EVENT_OK;

    case LWS_CALLBACK_RECEIVE:
    case LWS_CALLBACK_CLIENT_RECEIVE:
        mrp_debug("%d bytes received on websocket %p/%p", len, ws, user);
        mrp_debug("%zd remaining from this message",
                  libwebsockets_remaining_packet_payload(ws));

        sck = *(wsl_sck_t **)user;
        up  = sck ? sck->proto : NULL;

        if (up != NULL) {
            if (!up->framed && !mrp_fragbuf_missing(sck->buf)) {
                /* new packet of an unframed protocol, push message size */
                total = len + libwebsockets_remaining_packet_payload(ws);
                mrp_debug("unframed protocol, total message size %u", total);

                total = htobe32(total);
                mrp_fragbuf_push(sck->buf, &total, sizeof(total));
            }

            if (mrp_fragbuf_push(sck->buf, in, len)) {
                data = NULL;
                size = 0;

                while (mrp_fragbuf_pull(sck->buf, &data, &size)) {
                    mrp_debug("websocket %p/%p has a message of %zd bytes",
                              ws, user, size);

                    SOCKET_BUSY_REGION(sck, {
                            up->cbs.recv(sck, data, size, sck->user_data,
                                         up->proto_data);
                            up->cbs.check(sck, sck->user_data, up->proto_data);
                        });

                    if (check_closed(sck))
                        break;
                }
            }
            else {
                mrp_log_error("failed to push data to fragment buffer");

                SOCKET_BUSY_REGION(sck, {
                        sck->closing = TRUE;    /* make sure sck gets closed */
                        up->cbs.closed(sck, ENOBUFS, sck->user_data,
                                       up->proto_data);
                        libwebsocket_close_and_free_session(ctx->ctx, sck->sck,
                                                            LWS_INTERNAL_ERROR);
                        up->cbs.check(sck, sck->user_data, up->proto_data);
                    });

                check_closed(sck);
            }
        }
        return LWS_EVENT_OK;

    case LWS_CALLBACK_BROADCAST:
        mrp_debug("denying broadcast");
        return LWS_EVENT_DENY;

    case LWS_CALLBACK_SERVER_WRITEABLE:
        mrp_debug("socket server side writeable again");
        return LWS_EVENT_OK;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        mrp_debug("socket client side writeable again");
        return LWS_EVENT_OK;

    default:
        break;
    }

    return LWS_EVENT_OK;
}



/*
 * logging
 */

static void libwebsockets(const char *line)
{
    const char *ts, *ll;
    const char *b, *e, *lvl;
    int         l, ls;
    uint32_t    mask;

    if ((mask = mrp_log_get_mask()) == 0)
        return;

    /*
     * Notes:
     *     libwebsockets logging infrastructure has independently maskable
     *     log classes and supports overriding its default logger. The log
     *     classes are the regular error, warning, info, and debug classes
     *     plus the libwebsockets-specific parser, header, extension, and
     *     client classes. The logging infra filters the messages based on
     *     their class, then formats the message and passes it on to the
     *     (default builtin, or externally set) logger function. This gets
     *     a fully formatted log message that consists of a timestamp, a
     *     log class prefix and the message itself which typically contains
     *     at least one terminating newline.
     *
     *     Because of the semantic content of the messages coming from
     *     libwebsockets we'd like to preserve the class of errors and
     *     warnings but convert the rest to debug messages. Additionally,
     *     we'd like to keep the message format as consistent with the
     *     murphy infra as possible with a reasonable effort. This means
     *     stripping the timestamp and log class, as these are provided
     *     by the murphy infra (if configured so). However, for the
     *     libwebsockets-specific parser-, header-, extension-, and client-
     *     classes we want to keep the extra information carried by the
     *     log class as part of the message.
     *
     *     Because the libwebsockets log messages are terminated by '\n',
     *     we also prepare here to properly bridge multiline messages to
     *     the murphy infra (although I'm not sure the library ever issues
     *     such messages).
     *
     *     So to sum it up the necessary steps to bridge messages here are:
     *       1) strip timestamp,
     *       2) dig out and strip log class
     *       3) map log class to murphy infra, ie.
     *          keep errors and warnings, squash the rest to debug
     *       4) break multiline messages to lines
     *       5) pass each line on to the murphy infra,
     *          for parser-, header-, extension-, and client-messages
     *          prefix each line with the class
     *
     */

    ts = strchr(line, '[');
    ll = strchr(ts, ']');

    /* strip timestamp, dig out log level, find beginning of the message */
    if (ll != NULL && ll[1] == ' ') {
        ll += 2;
        b   = strchr(ll, ':');

        if (b != NULL && b[1] == ' ') {
            b += 2;

            while (*b == ' ')
                b++;

            /* map log level: debug, info, err, warn, or other */
            switch (*ll) {
            case 'D':
                if (!(mask & MRP_LOG_MASK_DEBUG))
                    return;
                lvl = "d";
                break;
            case 'I':
                if (!(mask & MRP_LOG_MASK_INFO))
                    return;
                lvl = "i";
                break;
            case 'W':
                if (!(mask & MRP_LOG_MASK_WARNING))
                    return;
                lvl = "w";
                break;
            case 'E':
                if (ll[1] == 'R') {
                    if (!(mask & MRP_LOG_MASK_ERROR))
                        return;
                    lvl = "e";
                }
                else {
                other:
                    if (!(mask & MRP_LOG_MASK_DEBUG))
                        return;
                    lvl = ll;
                    e  = strchr(lvl, ':');

                    if (e != NULL)
                        ls = e - lvl;
                    else {
                        lvl = "???:";
                        ls  = 4;
                    }
                }
                break;

            default:
                goto other;
            }
        }
        else
            goto unknown;
    }
    else {
    unknown:
        /* if we get confused with the format, default to logging it all */
        lvl = NULL;
        b   = line;
    }

    /* break the message to lines and pass it on to the murphy infra */
    e = strchr(b, '\n');
    while (e || b) {
        if (e)
            l = e - b;
        else
            l = strlen(b);

        if (!l)
            break;

        if (lvl != NULL) {
            switch (*lvl) {
            case 'd': mrp_debug("%*.*s", l, l, b);                  break;
            case 'i': mrp_debug("%*.*s", l, l, b);                  break;
            case 'w': mrp_log_warning("libwebsockets: %*.*s", l, l, b); break;
            case 'e': mrp_log_error("libwebsockets: %*.*s", l, l, b);   break;
            default:  mrp_debug("[%*.*s] %*.*s", ls, ls, lvl, l, l, b);
            }
        }
        else
            mrp_debug("%*.*s", l, l, b);

        if (e != NULL) {
            b = e + 1;
            e = strchr(b, '\n');
        }
        else
            b = NULL;
    }
}


void wsl_set_loglevel(wsl_loglevel_t mask)
{
    lws_set_log_level(mask, libwebsockets);
}
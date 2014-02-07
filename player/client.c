#include <stddef.h>
#include <stdlib.h>
#include <assert.h>

#include "common/common.h"
#include "common/msg.h"
#include "common/msg_control.h"
#include "input/input.h"
#include "misc/ring.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "options/m_property.h"
#include "osdep/threads.h"

#include "command.h"
#include "core.h"
#include "client.h"

// This basically stores the list of all clients.
struct mp_client_api {
    struct MPContext *mpctx;

    pthread_mutex_t lock;

    // -- protected by lock
    struct mpv_handle **clients;
    int num_clients;
};

struct mpv_handle {
    // -- immmutable
    char *name;
    struct mp_log *log;
    struct MPContext *mpctx;
    struct mp_client_api *clients;

    // -- not thread-safe
    struct mpv_event_data *cur_event;

    pthread_mutex_t lock;
    pthread_cond_t wakeup;

    // -- protected by lock

    uint64_t alloc_reply_id;
    uint64_t event_mask;
    bool queued_wakeup;
    bool shutdown;
    bool choke_warning;
    void (*wakeup_cb)(void *d);
    void *wakeup_cb_ctx;

    struct mp_ring *events;     // stores mpv_event_data
    int max_events;             // allocated number of entries in events
    int reserved_events;        // number of entries reserved for replies

    struct mp_log_buffer *messages;
    int messages_level;
};

void mp_clients_init(struct MPContext *mpctx)
{
    mpctx->clients = talloc_ptrtype(NULL, mpctx->clients);
    *mpctx->clients = (struct mp_client_api) {
        .mpctx = mpctx,
    };
    pthread_mutex_init(&mpctx->clients->lock, NULL);
}

void mp_clients_destroy(struct MPContext *mpctx)
{
    if (!mpctx->clients)
        return;
    assert(mpctx->clients->num_clients == 0);
    pthread_mutex_destroy(&mpctx->clients->lock);
    talloc_free(mpctx->clients);
    mpctx->clients = NULL;
}

int mp_clients_num(struct MPContext *mpctx)
{
    pthread_mutex_lock(&mpctx->clients->lock);
    int num_clients = mpctx->clients->num_clients;
    pthread_mutex_unlock(&mpctx->clients->lock);
    return num_clients;
}

static struct mpv_handle *find_client(struct mp_client_api *clients,
                                      const char *name)
{
    for (int n = 0; n < clients->num_clients; n++) {
        if (strcmp(clients->clients[n]->name, name) == 0)
            return clients->clients[n];
    }
    return NULL;
}

struct mpv_handle *mp_new_client(struct mp_client_api *clients, const char *name)
{
    pthread_mutex_lock(&clients->lock);

    char *unique_name = NULL;
    if (find_client(clients, name)) {
        for (int n = 2; n < 1000; n++) {
            unique_name = talloc_asprintf(NULL, "%s%d", name, n);
            if (!find_client(clients, unique_name))
                break;
            talloc_free(unique_name);
            unique_name = NULL;
        }
        if (!unique_name) {
            pthread_mutex_unlock(&clients->lock);
            return NULL;
        }
    }
    if (!unique_name)
        unique_name = talloc_strdup(NULL, name);

    int num_events = 1000;

    struct mpv_handle *client = talloc_ptrtype(NULL, client);
    *client = (struct mpv_handle){
        .name = talloc_steal(client, unique_name),
        .log = mp_log_new(client, clients->mpctx->log, unique_name),
        .mpctx = clients->mpctx,
        .clients = clients,
        .cur_event = talloc_zero(client, struct mpv_event_data),
        .events = mp_ring_new(client, num_events * sizeof(struct mpv_event_data)),
        .max_events = num_events,
        .event_mask = ((uint64_t)-1) & ~(1ULL << MPV_EVENT_TICK),
    };
    pthread_mutex_init(&client->lock, NULL);
    pthread_cond_init(&client->wakeup, NULL);

    MP_TARRAY_APPEND(clients, clients->clients, clients->num_clients, client);

    pthread_mutex_unlock(&clients->lock);

    return client;
}

const char *mpv_client_name(mpv_handle *ctx)
{
    return ctx->name;
}

struct mp_log *mp_client_get_log(struct mpv_handle *ctx)
{
    return ctx->log;
}

static void wakeup_client(struct mpv_handle *ctx)
{
    pthread_cond_signal(&ctx->wakeup);
    if (ctx->wakeup_cb)
        ctx->wakeup_cb(ctx->wakeup_cb_ctx);
}

void mpv_set_wakeup_callback(mpv_handle *ctx, void (*cb)(void *d), void *d)
{
    pthread_mutex_lock(&ctx->lock);
    ctx->wakeup_cb = cb;
    ctx->wakeup_cb_ctx = d;
    pthread_mutex_unlock(&ctx->lock);
}

void mpv_suspend(mpv_handle *ctx)
{
    mp_dispatch_suspend(ctx->mpctx->dispatch);
}

void mpv_resume(mpv_handle *ctx)
{
    mp_dispatch_resume(ctx->mpctx->dispatch);
}

void mpv_destroy(mpv_handle *ctx)
{
    struct mp_client_api *clients = ctx->clients;

    pthread_mutex_lock(&clients->lock);
    for (int n = 0; n < clients->num_clients; n++) {
        if (clients->clients[n] == ctx) {
            MP_TARRAY_REMOVE_AT(clients->clients, clients->num_clients, n);
            while (mp_ring_buffered(ctx->events)) {
                struct mpv_event_data event;
                int r = mp_ring_read(ctx->events, (unsigned char *)&event,
                                     sizeof(event));
                assert(r == sizeof(event));
                talloc_free(event.data);
            }
            mp_msg_log_buffer_destroy(ctx->messages);
            pthread_cond_destroy(&ctx->wakeup);
            pthread_mutex_destroy(&ctx->lock);
            talloc_free(ctx);
            ctx = NULL;
            // shutdown_clients() sleeps to avoid wasting CPU
            mp_input_wakeup(clients->mpctx->input);
            // TODO: make core quit if there are no clients
            break;
        }
    }
    pthread_mutex_unlock(&clients->lock);
    assert(!ctx);
}

mpv_handle *mpv_create(void)
{
    struct MPContext *mpctx = mp_create();
    mpv_handle *ctx = mp_new_client(mpctx->clients, "main");
    if (ctx) {
        // Set some defaults.
        mpv_set_option_string(ctx, "idle", "yes");
        mpv_set_option_string(ctx, "terminal", "no");
        mpv_set_option_string(ctx, "osc", "no");
    } else {
        mp_destroy(mpctx);
    }
    return ctx;
}

static void *playback_thread(void *p)
{
    struct MPContext *mpctx = p;

    pthread_detach(pthread_self());

    mp_play_files(mpctx);

    // This actually waits until all clients are gone before actually
    // destroying mpctx.
    mp_destroy(mpctx);

    return NULL;
}

int mpv_initialize(mpv_handle *ctx)
{
    if (mp_initialize(ctx->mpctx) < 0)
        return MPV_ERROR_INVALID_PARAMETER;

    pthread_t thread;
    if (pthread_create(&thread, NULL, playback_thread, ctx->mpctx) != 0)
        return MPV_ERROR_NOMEM;

    return 0;
}

// Reserve an entry in the ring buffer, as well as a reply ID. This can be
// used to guarantee that the reply can be made, even if the buffer becomes
// congested _after_ sending the request.
// Returns an error code if the buffer is full.
static int64_t reserve_reply(struct mpv_handle *ctx)
{
    int64_t res = MPV_ERROR_EVENT_BUFFER_FULL;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->reserved_events < ctx->max_events) {
        ctx->reserved_events++;
        res = ++ctx->alloc_reply_id;
    }
    pthread_mutex_unlock(&ctx->lock);
    return res;
}

static int send_event(struct mpv_handle *ctx, struct mpv_event_data *event)
{
    pthread_mutex_lock(&ctx->lock);
    if (!(ctx->event_mask & (1ULL << event->event_id))) {
        pthread_mutex_unlock(&ctx->lock);
        return 0;
    }
    int num_events = mp_ring_available(ctx->events) / sizeof(*event);
    int r = 0;
    if (num_events > ctx->reserved_events) {
        r = mp_ring_write(ctx->events, (unsigned char *)event, sizeof(*event));
        if (r != sizeof(*event))
            abort();
        wakeup_client(ctx);
    }
    if (!r && !ctx->choke_warning) {
        mp_err(ctx->log, "Too many events queued.\n");
        ctx->choke_warning = true;
    }
    pthread_mutex_unlock(&ctx->lock);
    return r ? 0 : -1;
}

// Send a reply; the reply must have been previously reserved with
// reserve_reply (otherwise, use send_event()).
static void send_reply(struct mpv_handle *ctx, int64_t reply_id,
                       struct mpv_event_data *event)
{
    pthread_mutex_lock(&ctx->lock);
    assert(ctx->reserved_events > 0);
    ctx->reserved_events--;
    int r = mp_ring_write(ctx->events, (unsigned char *)event, sizeof(*event));
    if (r != sizeof(*event))
        abort();
    wakeup_client(ctx);
    pthread_mutex_unlock(&ctx->lock);
}

static void send_error_reply(struct mpv_handle *ctx, int64_t reply_id, int err)
{
    struct mpv_event_data event = {
        .event_id = MPV_EVENT_ERROR,
        .error = err,
    };
    send_reply(ctx, reply_id, &event);
}

void mp_client_status_reply(struct mpv_handle *ctx, int64_t reply_id, int status)
{
    if (status < 0) {
        send_error_reply(ctx, reply_id, status);
    } else {
        struct mpv_event_data reply = {
            .event_id = MPV_EVENT_OK,
        };
        send_reply(ctx, reply_id, &reply);
    }
}

void mp_client_broadcast_event(struct MPContext *mpctx, int event, void *data)
{
    struct mp_client_api *clients = mpctx->clients;

    struct mpv_event_data event_data = {
        .event_id = event,
        .data = data,
    };

    pthread_mutex_lock(&clients->lock);

    for (int n = 0; n < clients->num_clients; n++)
        send_event(clients->clients[n], &event_data);

    pthread_mutex_unlock(&clients->lock);

    talloc_free(data);
}

int mp_client_send_event(struct MPContext *mpctx, const char *client_name,
                         int event, void *data)
{
    struct mp_client_api *clients = mpctx->clients;
    int r = 0;

    struct mpv_event_data event_data = {
        .event_id = event,
        .data = data,
    };

    pthread_mutex_lock(&clients->lock);

    struct mpv_handle *ctx = find_client(clients, client_name);
    if (ctx) {
        r = send_event(ctx, &event_data);
    } else {
        r = -1;
        talloc_free(data);
    }

    pthread_mutex_unlock(&clients->lock);

    return r;
}

int mpv_request_event(mpv_handle *ctx, mpv_event event, int enable)
{
    if (!mpv_event_name(event) || enable < 0 || enable > 1)
        return MPV_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&ctx->lock);
    uint64_t bit = 1LLU << event;
    ctx->event_mask = enable ? ctx->event_mask | bit : ctx->event_mask & ~bit;
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

mpv_event_data *mpv_wait_event(mpv_handle *ctx, double timeout)
{
    mpv_event_data *event = ctx->cur_event;

    struct timespec deadline = mpthread_get_deadline(timeout);

    pthread_mutex_lock(&ctx->lock);

    *event = (mpv_event_data){0};
    talloc_free_children(event);

    while (1) {
        if (mp_ring_buffered(ctx->events)) {
            int r =
                mp_ring_read(ctx->events, (unsigned char*)event, sizeof(*event));
            if (r != sizeof(*event))
                abort();
            talloc_steal(event, event->data);
            break;
        }
        if (ctx->shutdown) {
            event->event_id = MPV_EVENT_SHUTDOWN;
            break;
        }
        if (ctx->messages) {
            // Poll the log message queue. Currently we can't/don't do better.
            struct mp_log_buffer_entry *msg =
                mp_msg_log_buffer_read(ctx->messages);
            if (msg) {
                event->event_id = MPV_EVENT_LOG_MESSAGE;
                struct mpv_event_log_message *cmsg = talloc_ptrtype(event, cmsg);
                *cmsg = (struct mpv_event_log_message){
                    .prefix = talloc_steal(event, msg->prefix),
                    .level = mp_log_levels[msg->level],
                    .text = talloc_steal(event, msg->text),
                };
                event->data = cmsg;
                talloc_free(msg);
                break;
            }
        }
        if (ctx->queued_wakeup)
            break;
        if (timeout <= 0)
            break;
        pthread_cond_timedwait(&ctx->wakeup, &ctx->lock, &deadline);
    }
    ctx->queued_wakeup = false;

    pthread_mutex_unlock(&ctx->lock);

    return event;
}

void mpv_wakeup(mpv_handle *ctx)
{
    pthread_mutex_lock(&ctx->lock);
    ctx->queued_wakeup = true;
    wakeup_client(ctx);
    pthread_mutex_unlock(&ctx->lock);
}

int mpv_set_option(mpv_handle *ctx, const char *name, mpv_format format,
                   void *data)
{
    if (ctx->mpctx->initialized) {
        char prop[100];
        snprintf(prop, sizeof(prop), "options/%s", name);
        return mpv_set_property(ctx, name, format, data);
    } else {
        if (format != MPV_FORMAT_STRING)
            return MPV_ERROR_INVALID_PARAMETER;
        const char *value = data;
        int err = m_config_set_option0(ctx->mpctx->mconfig, name, value);
        switch (err) {
        case M_OPT_MISSING_PARAM:
        case M_OPT_INVALID:
        case M_OPT_OUT_OF_RANGE:
            return MPV_ERROR_INVALID_PARAMETER;
        case M_OPT_UNKNOWN:
            return MPV_ERROR_NOT_FOUND;
        default:
            if (err >= 0)
                return 0;
            return MPV_ERROR_INVALID_PARAMETER;
        }
    }
}

int mpv_set_option_string(mpv_handle *ctx, const char *name, const char *data)
{
    return mpv_set_option(ctx, name, MPV_FORMAT_STRING, (void *)data);
}

// Run a command in the playback thread.
// Note: once some things are fixed (like vo_opengl not being safe to be
//       called from any thread other than the playback thread), this can
//       be replaced by a simpler method.
static void run_locked(mpv_handle *ctx, void (*fn)(void *fn_data), void *fn_data)
{
    mp_dispatch_run(ctx->mpctx->dispatch, fn, fn_data);
}

// Run a command asynchronously. It's the responsibility of the caller to
// actually send the reply. This helper merely saves a small part of the
// required boilerplate to do so.
//  req_reply_id: where to store the reply_id. This should point into the struct
//                the fn_data points to, so that the fn callback can use it to
//                send a reply with e.g. send_reply().
//  fn: callback to execute the request
//  fn_data: opaque caller-defined argument for fn. This will be automatically
//           freed with talloc_free(fn_data).
static int64_t run_async(mpv_handle *ctx, int64_t *req_reply_id,
                         void (*fn)(void *fn_data), void *fn_data)
{
    int64_t reply_id = reserve_reply(ctx);
    if (reply_id < 0) {
        talloc_free(fn_data);
        return reply_id;
    }
    *req_reply_id = reply_id;
    mp_dispatch_enqueue_autofree(ctx->mpctx->dispatch, fn, fn_data);
    return reply_id;
}

struct cmd_request {
    struct MPContext *mpctx;
    struct mp_cmd *cmd;
    int status;
    struct mpv_handle *reply_ctx;
    int64_t reply_id;
};

static void cmd_fn(void *data)
{
    struct cmd_request *req = data;
    run_command(req->mpctx, req->cmd);
    req->status = 0;
    talloc_free(req->cmd);
    if (req->reply_ctx)
        mp_client_status_reply(req->reply_ctx, req->reply_id, req->status);
}

static int run_client_command(mpv_handle *ctx, struct mp_cmd *cmd)
{
    if (!ctx->mpctx->initialized)
        return MPV_ERROR_UNINITIALIZED;
    if (!cmd)
        return MPV_ERROR_INVALID_PARAMETER;

    struct cmd_request req = {
        .mpctx = ctx->mpctx,
        .cmd = cmd,
    };
    run_locked(ctx, cmd_fn, &req);
    return req.status;
}

int mpv_command(mpv_handle *ctx, const char **args)
{
    return run_client_command(ctx, mp_input_parse_cmd_strv(ctx->log, 0, args,
                                                           ctx->name));
}

int mpv_command_string(mpv_handle *ctx, const char *args)
{
    return run_client_command(ctx,
        mp_input_parse_cmd(ctx->mpctx->input, bstr0((char*)args), ctx->name));
}

mpv_reply_id mpv_command_async(mpv_handle *ctx, const char **args)
{
    if (!ctx->mpctx->initialized)
        return MPV_ERROR_UNINITIALIZED;

    struct mp_cmd *cmd = mp_input_parse_cmd_strv(ctx->log, 0, args, "<client>");
    if (!cmd)
        return MPV_ERROR_INVALID_PARAMETER;

    struct cmd_request *req = talloc_ptrtype(NULL, req);
    *req = (struct cmd_request){
        .mpctx = ctx->mpctx,
        .cmd = cmd,
        .reply_ctx = ctx,
    };
    return run_async(ctx, &req->reply_id, cmd_fn, req);
}

static int translate_property_error(int errc)
{
    switch (errc) {
    case M_PROPERTY_OK:                 return 0;
    case M_PROPERTY_ERROR:              return MPV_ERROR_PROPERTY;
    case M_PROPERTY_UNAVAILABLE:        return MPV_ERROR_PROPERTY_UNAVAILABLE;
    case M_PROPERTY_NOT_IMPLEMENTED:    return MPV_ERROR_PROPERTY;
    case M_PROPERTY_UNKNOWN:            return MPV_ERROR_NOT_FOUND;
    // shouldn't happen
    default:                            return MPV_ERROR_PROPERTY;
    }
}

struct setproperty_request {
    struct MPContext *mpctx;
    const char *name;
    int cmd;
    void *data;
    int status;
    struct mpv_handle *reply_ctx;
    int64_t reply_id;
};

static void setproperty_fn(void *arg)
{
    struct setproperty_request *req = arg;
    int err = mp_property_do(req->name, req->cmd, req->data, req->mpctx);
    req->status = translate_property_error(err);
    if (req->reply_ctx)
        mp_client_status_reply(req->reply_ctx, req->reply_id, req->status);
}

int mpv_set_property(mpv_handle *ctx, const char *name, mpv_format format,
                     void *data)
{
    if (!ctx->mpctx->initialized)
        return MPV_ERROR_UNINITIALIZED;
    if (format != MPV_FORMAT_STRING)
        return MPV_ERROR_INVALID_PARAMETER;

    struct setproperty_request req = {
        .name = name,
        .data = data,
    };
    run_locked(ctx, setproperty_fn, &req);
    return req.status;
}

int mpv_set_property_string(mpv_handle *ctx, const char *name, const char *data)
{
    return mpv_set_property(ctx, name, MPV_FORMAT_STRING, (void *)data);
}

mpv_reply_id mpv_set_property_async(mpv_handle *ctx, const char *name,
                                    mpv_format format, void *data)
{
    if (!ctx->mpctx->initialized)
        return MPV_ERROR_UNINITIALIZED;
    if (format != MPV_FORMAT_STRING)
        return MPV_ERROR_INVALID_PARAMETER;

    struct setproperty_request *req = talloc_ptrtype(NULL, req);
    *req = (struct setproperty_request){
        .mpctx = ctx->mpctx,
        .name = talloc_strdup(req, name),
        .data = talloc_strdup(req, data), // for now always a string
        .reply_ctx = ctx,
    };
    return run_async(ctx, &req->reply_id, setproperty_fn, req);
}

static int property_format_to_cmd(int format)
{
    switch (format) {
    case MPV_FORMAT_STRING:     return M_PROPERTY_GET_STRING;
    case MPV_FORMAT_OSD_STRING: return M_PROPERTY_PRINT;
    default:                    return MPV_ERROR_INVALID_PARAMETER;
    }
}

struct getproperty_request {
    struct MPContext *mpctx;
    const char *name;
    mpv_format format;
    void *data;
    int status;
    struct mpv_handle *reply_ctx;
    int64_t reply_id;
};

static void getproperty_fn(void *arg)
{
    struct getproperty_request *req = arg;

    char *xdata = NULL; // currently, we support strings only
    void *data = req->data ? req->data : &xdata;

    int cmd = property_format_to_cmd(req->format);
    if (cmd < 0) {
        req->status = cmd;
    } else {
        int err = mp_property_do(req->name, cmd, data, req->mpctx);
        req->status = translate_property_error(err);
    }

    if (req->reply_ctx) {
        if (req->status < 0) {
            send_error_reply(req->reply_ctx, req->reply_id, req->status);
        } else {
            struct mpv_event_property *prop = talloc_ptrtype(NULL, prop);
            *prop = (struct mpv_event_property){
                .name = talloc_steal(prop, (char *)req->name),
                .format = req->format,
                .data = talloc_steal(prop, xdata),
            };
            struct mpv_event_data reply = {
                .event_id = MPV_EVENT_PROPERTY,
                .data = prop,
            };
            send_reply(req->reply_ctx, req->reply_id, &reply);
        }
    }
}

int mpv_get_property(mpv_handle *ctx, const char *name, mpv_format format,
                     void *data)
{
    if (!ctx->mpctx->initialized)
        return MPV_ERROR_UNINITIALIZED;

    struct getproperty_request req = {
        .mpctx = ctx->mpctx,
        .name = name,
        .format = format,
        .data = data,
    };
    run_locked(ctx, getproperty_fn, &req);
    return req.status;
}

char *mpv_get_property_string(mpv_handle *ctx, const char *name)
{
    char *str = NULL;
    mpv_get_property(ctx, name, MPV_FORMAT_STRING, &str);
    return str;
}

char *mpv_get_property_osd_string(mpv_handle *ctx, const char *name)
{
    char *str = NULL;
    mpv_get_property(ctx, name, MPV_FORMAT_OSD_STRING, &str);
    return str;
}

mpv_reply_id mpv_get_property_async(mpv_handle *ctx, const char *name,
                                    mpv_format format)
{
    if (!ctx->mpctx->initialized)
        return MPV_ERROR_UNINITIALIZED;

    struct getproperty_request *req = talloc_ptrtype(NULL, req);
    *req = (struct getproperty_request){
        .mpctx = ctx->mpctx,
        .name = talloc_strdup(req, name),
        .format = format,
        .reply_ctx = ctx,
    };
    return run_async(ctx, &req->reply_id, getproperty_fn, req);
}

int mpv_request_log_messages(mpv_handle *ctx, const char *min_level)
{
    int level = -1;
    for (int n = 0; n < MSGL_MAX + 1; n++) {
        if (mp_log_levels[n] && strcmp(min_level, mp_log_levels[n]) == 0) {
            level = n;
            break;
        }
    }
    if (level < 0 && strcmp(min_level, "no") != 0)
        return MPV_ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&ctx->lock);

    if (!ctx->messages)
        ctx->messages_level = -1;

    if (ctx->messages_level != level) {
        mp_msg_log_buffer_destroy(ctx->messages);
        ctx->messages = NULL;
        if (level >= 0) {
            ctx->messages =
                mp_msg_log_buffer_new(ctx->mpctx->global, 1000, level);
        }
        ctx->messages_level = level;
    }

    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

unsigned long mpv_client_api_version(void)
{
    return MPV_CLIENT_API_VERSION;
}

static const char *err_table[] = {
    [-MPV_ERROR_SUCCESS] = "success",
    [-MPV_ERROR_EVENT_BUFFER_FULL] = "request buffer full",
    [-MPV_ERROR_INVALID_PARAMETER] = "invalid parameter",
    [-MPV_ERROR_NOMEM] = "memory allocation failed",
    [-MPV_ERROR_NOT_FOUND] = "not found",
    [-MPV_ERROR_PROPERTY] = "error accessing property",
    [-MPV_ERROR_PROPERTY_UNAVAILABLE] = "property unavailable",
    [-MPV_ERROR_UNINITIALIZED] = "core not initialized",
};

const char *mpv_error_string(int error)
{
    error = -error;
    if (error < 0)
        error = 0;
    const char *name = NULL;
    if (error < MP_ARRAY_SIZE(err_table))
        name = err_table[error];
    return name ? name : "unknown error";
}

static const char *event_table[] = {
    [MPV_EVENT_NONE] = "none",
    [MPV_EVENT_OK] = "ok",
    [MPV_EVENT_ERROR] = "error",
    [MPV_EVENT_SHUTDOWN] = "shutdown",
    [MPV_EVENT_LOG_MESSAGE] = "log-message",
    [MPV_EVENT_TICK] = "tick",
    [MPV_EVENT_PROPERTY] = "property",
    [MPV_EVENT_START_FILE] = "start-file",
    [MPV_EVENT_END_FILE] = "end-file",
    [MPV_EVENT_PLAYBACK_START] = "playback-start",
    [MPV_EVENT_TRACKS_CHANGED] = "tracks-changed",
    [MPV_EVENT_TRACK_SWITCHED] = "track-switched",
    [MPV_EVENT_IDLE] = "idle",
    [MPV_EVENT_PAUSE] = "pause",
    [MPV_EVENT_UNPAUSE] = "unpause",
    [MPV_EVENT_SCRIPT_INPUT_DISPATCH] = "script-input-dispatch",
};

const char *mpv_event_name(mpv_event event)
{
    if (event < 0 || event >= MP_ARRAY_SIZE(event_table))
        return NULL;
    return event_table[event];
}

void mpv_free(void *data)
{
    talloc_free(data);
}

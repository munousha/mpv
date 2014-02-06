#ifndef MPV_CLIENT_API_H_
#define MPV_CLIENT_API_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The version is incremented on each change. The 16 lower bits are incremented
 * if something in mpv is changed that might affect the client API, but doesn't
 * change C API itself (like the removal of an option or a property). The higher
 * 16 bits are incremented if the C API itself changes.
 */
#define MPV_CLIENT_API_VERSION 0x00000000UL

/**
 * Return the MPV_CLIENT_API_VERSION the mpv source has been compiled with.
 */
unsigned long mpv_client_api_version(void);

/**
 * Client context used by the client API. Every client has its own private
 * handle.
 */
typedef struct mpv_handle mpv_handle;

/**
 * The client API includes asynchronous functions. These allow you to send
 * requests instantly, and get replies as events at a later point.
 * Reply IDs are used to associate requests with replies. As such, they are
 * returned by functions which send requests.
 *
 * Normal reply IDs are always > 0 and strictly monotonously increasing.
 *
 * A value of 0 is used in events which are not directly in reply to a request.
 *
 * A negative value is used on error situations. In this case the value is
 * not a reply ID, but an error code. This can happen for example when sending
 * a request and the request queue is full, or if sending a request requires
 * memory allocation and allocation failed.
 *
 * This type is always int64_t. The typedef is only for documentation.
 */
typedef int64_t mpv_reply_id;

/**
 * List of error codes than can be returned by API functions. 0 and positive
 * return values always mean success, negative values are always errors.
 */
typedef enum mpv_error {
    /**
     * No error happened (used to signal successful operation).
     * Keep in mind that many API functions returning error codes can also
     * return positive values, which also indicate success. API users can
     * hardcode the fact that ">= 0" means success.
     */
    MPV_ERROR_SUCCESS           = 0,
    /**
     * The event ringbuffer is full. This means the client is choked, and can't
     * receive any events. This can happen when too many asynchronous requests
     * have been made, but not answered. Probably never happens in practice,
     * unless the mpv core is frozen for some reason, and the client keeps
     * making asynchronous requests. (Bugs in the client API implementation
     * could also trigger this, e.g. if events become "lost".)
     */
    MPV_ERROR_EVENT_BUFFER_FULL = -1,
    /**
     */
    MPV_ERROR_INVALID_PARAMETER = -2,
    /**
     * Memory allocation failed.
     */
    MPV_ERROR_NOMEM             = -3,
    /**
     * The accessed property/option/command was not found.
     */
    MPV_ERROR_NOT_FOUND         = -4,
    /**
     * Error setting or getting a property.
     */
    MPV_ERROR_PROPERTY          = -5,
    /**
     * The property is not available. This usually happens when the associated
     * subsystem is not active, e.g. trying to change volume while audio is
     * disabled.
     */
    MPV_ERROR_PROPERTY_UNAVAILABLE = -6,
    /**
     * The mpv core wasn't configured and started yet. See the notes in
     * mpv_create().
     */
    MPV_ERROR_UNINITIALIZED = -7,
} mpv_error;

/**
 * Return a string describing the error. For unknown errors, the string
 * "unknown error" is returned.
 *
 * @param error error number, see enum mpv_error
 * @return A static string describing the error. The string is completely
 *         static, i.e. doesn't need to be deallocated, and is valid forever.
 */
const char *mpv_error_string(int error);

/**
 * General function to deallocate memory returned by any of the API functions.
 * Calling this on mpv memory not owned by the caller is not allowed and will
 * lead to undefined behavior. In other words, call this only if it's explicitly
 * documented as allowed.
 *
 * @param data A valid pointer return by the API, or NULL.
 */
void mpv_free(void *data);

/**
 * Return the name of this client handle.
 *
 * @return The client name. The string is read-only and is valid until
 *         mpv_destroy() is called.
 */
const char *mpv_client_name(mpv_handle *ctx);

/**
 * Create a new mpv instance. This instance is in a pre-initialized state,
 * and needs to be initialized to be actually used with most other API
 * functions.
 *
 * Unlike the command line player, this will have initial settings suitable
 * for embedding in applications. The following settings are different:
 * - stdin/stdout/stderr and the terminal will never be accessed. This is
 *   equivalent to setting the --no-terminal option.
 *   (Technically, this also suppresses C signal handling.)
 * - No config files will be loaded. This is equivalent to using --no-config.
 * - Idle mode is enabled, which means the playback core will not exit if
 *   there are no more files, but enter idle mode instead. This is equivalent
 *   to the --idle option.
 * - Disable input handling.
 *
 * All this assumes that API users do not want to use mpv user settings, and
 * that they want to provide their own GUI. You can re-enable disabled features
 * by setting the appropriate options.
 *
 * The point of separating handle creation and actual startup is that you can
 * configure things which can't be changed during runtime.
 *
 * Most API functions will return MPV_ERROR_UNINITIALIZED in this state. You
 * can call mpv_set_option() (and variants, e.g. mpv_set_option_string()) to
 * set initial options. After this, call mpv_initialize() to start the player,
 * and then use mpv_command() to start playback of a file.
 *
 * @return a new mpv client API handle
 */
mpv_handle *mpv_create(void);

/**
 * Start an unconfigured mpv instance and put it into the running state. If
 * this function was already called successfully, this function fails.
 *
 * Upon successful return of this function, you can make full use of the client
 * API.
 *
 * @return error code
 */
int mpv_initialize(mpv_handle *ctx);

/**
 * Disconnect and destroy the client context. ctx will be deallocated with this
 * API call. This leaves the player running.
 */
void mpv_destroy(mpv_handle *ctx);

/**
 * Stop the playback thread. Normally, the client API stops the playback thread
 * automatically in order to process requests. However, stopping the playback
 * thread might take a long time: after processing requests, it will just
 * continue to attempt to display the next video frame, for which it will take
 * up to 50ms. (Internally, it first renders the video and other things, and
 * then blocks until it can be displayed - and it won't react to anything else
 * in that time. The main reason for that is that the VO is in a "in between"
 * state, in which it can't process normal requests - for example, OSD redrawing
 * or screenshots would be broken.)
 *
 * Suspending the playback thread allows you to prevent the playback thread from
 * running, so that you can make multiple accesses without being blocked.
 *
 * Suspension is reentrant and recursive for convenience. Any thread can call
 * the suspend function multiple times, and the playback thread will remain
 * suspended until the last thread resumes it.
 *
 * Call mpv_resume() to resume the playback thread. You must call mpv_resume()
 * for each mpv_suspend() call. Calling mpv_resume() more often than
 * mpv_suspend() is not allowed, and the core will try to crash if you do.
 *
 * Note: the need for this call might go away at some point.
 */
void mpv_suspend(mpv_handle *ctx);

/**
 * See mpv_suspend().
 */
void mpv_resume(mpv_handle *ctx);

/**
 * Data format for options and properties.
 */
typedef enum mpv_format {
    /**
     * Invalid.
     */
    MPV_FORMAT_NONE             = 0,
    /**
     * The basic type is char*. It returns the raw property string, like
     * using ${=property} in input.conf (see input.rst).
     */
    MPV_FORMAT_STRING           = 1,
    /**
     * The basic type is char*. It returns the OSD property string, like
     * using ${property} in input.conf (see input.rst). In many cases, this
     * is the same as the raw string, but in other cases it's formatted for
     * display on OSD. It's intended to be human readable. Do not attempt to
     * parse these strings.
     *
     * Only valid when doing read access.
     */
    MPV_FORMAT_OSD_STRING       = 2,
} mpv_format;

/**
 * Set an option. This works only in idle mode. To change options during
 * playback, you can try to use mpv_set_property(), although not all options
 * can be changed.
 *
 * @param name Option name. This is the same as on the mpv command, but without
 *             the leading "--".
 * @param format see enum mpv_format. Only MPV_FORMAT_STRING is valid.
 * @param[in] data Option value.
 * @return error code
 */
int mpv_set_option(mpv_handle *ctx, const char *name, mpv_format format,
                   void *data);

/**
 * Convenience function to set an option to a string value.
 *
 * This is like calling mpv_set_option() with MPV_FORMAT_STRING.
 */
int mpv_set_option_string(mpv_handle *ctx, const char *name, const char *data);

/**
 * Send a command to the player. Commands are the same as those used in
 * input.conf, except that this function takes parameters in a pre-split
 * form.
 *
 * Caveat: currently, commands do not report whether they run successfully or
 *         not, so always success will be returned.
 *
 * @param[in] args NULL-terminated list of strings. Usually, the first item
 *                 is the command, and the following items are arguments.
 *                 TODO: document the list of commands? link to input.rst?
 * @return error code
 */
int mpv_command(mpv_handle *ctx, const char **args);

/**
 * Same as mpv_command, but use input.conf parsing for splitting arguments.
 */
int mpv_command_string(mpv_handle *ctx, const char *args);

/**
 * Same as mpv_command, but run the command asynchronously.
 *
 * Commands are executed asynchronously. You will receive a
 * MPV_EVENT_OK or MPV_EVENT_ERROR event. In some cases,
 * more specialized event types might be used for the reply.
 *
 * @warning Commands that cancel current playback can be reordered, and cancel
 *          earlier commands.
 *          TODO: explain exact semantics, also check with seek commands.
 *
 * @return Reply ID for the confirmation event. On out of memory situations,
 *         or if the player isn't running, return a negative error code. Note
 *         that the command as well as the parameters are checked in the
 *         player thread, after sending the request. These errors will be
 *         returned as error events.
 */
mpv_reply_id mpv_command_async(mpv_handle *ctx, const char **args);

/**
 * Set a property to a given value.
 *
 * @param name The property name.
 * @param format see enum mpv_format. Only MPV_FORMAT_STRING is valid.
 * @param[in] data Option value.
 * @return error code
 */
int mpv_set_property(mpv_handle *ctx, const char *name, mpv_format format,
                     void *data);

/**
 * Convenience function to set a property to a string value.
 *
 * This is like calling mpv_set_property() with MPV_FORMAT_STRING.
 */
int mpv_set_property_string(mpv_handle *ctx, const char *name, const char *data);

/**
 * Set a property asynchronously. You will receive the result of the operation
 * as MPV_EVENT_OK or MPV_EVENT_ERROR event.
 *
 * @param name The property name.
 * @param format see enum mpv_format. Only MPV_FORMAT_STRING is valid.
 * @param[in] data Option value. The value will be copied.
 * @return Reply ID for the confirmation event, or negative error code.
 */
mpv_reply_id mpv_set_property_async(mpv_handle *ctx, const char *name,
                                    mpv_format format, void *data);

/**
 * Read the value of the given property.
 *
 * @param name The property name.
 * @param format see enum mpv_format.
 * @param[out] data Pointer to the variable holding the option value. On
 *                  success, the variable will be set to a copy of the option
 *                  value. You can free the value with mpv_free().
 * @return error code
 */
int mpv_get_property(mpv_handle *ctx, const char *name, mpv_format format,
                     void *data);

/**
 * Return the value of the property with the given name as string.
 * On error, NULL is returned.
 * Use mpv_get_property() if you want fine-grained error reporting.
 *
 * @param name The property name.
 * @return Property value, or NULL if the property can't be retrieved. Free
 *         the string with mpv_free().
 */
char *mpv_get_property_string(mpv_handle *ctx, const char *name);

/**
 * Same as mpv_get_property, but return the value as "OSD" string.
 */
char *mpv_get_property_osd_string(mpv_handle *ctx, const char *name);

/**
 * Get a property asynchronously. You will receive the result of the operation
 * as well as the property data with the MPV_EVENT_PROPERTY or
 * MPV_EVENT_ERROR event.
 *
 * @param name The property name.
 * @param format see enum mpv_format.
 * @return Reply ID, or negative error code if sending the request failed.
 */
mpv_reply_id mpv_get_property_async(mpv_handle *ctx, const char *name,
                                    mpv_format format);

typedef enum mpv_event {
    /**
     * Nothing happened. Happens on timeouts or sporadic wakeups.
     */
    MPV_EVENT_NONE              = 0,
    /**
     * Generic reply to a successfully run asynchronous request or command.
     *
     * This is bound to the request referenced in the in_reply_to field.
     */
    MPV_EVENT_OK                = 1,
    /**
     * A client API-level error happened. This is for errors that happen on
     * client API requests and so on, rather than mpv level errors (such as
     * failed playback).
     *
     * This is bound to the request referenced with the in_reply_to field. If
     * this field is 0, the event signals a mpv_wait_event() error.
     */
    MPV_EVENT_ERROR             = 2,
    /**
     * Happens when the player quits. The player enters a state where it tries
     * to disconnect all clients. Most requests to the player will fail, and
     * mpv_wait_event() will always return instantly (returning new shutdown
     * events if no other events are queued). The client should react to this
     * and quit with mpv_destroy() as soon as possible.
     */
    MPV_EVENT_SHUTDOWN          = 3,
    /**
     * See mpv_request_log_messages().
     */
    MPV_EVENT_LOG_MESSAGE       = 4,
    /**
     * Sent every time a video frame is displayed (or in lower frequency if
     * there is no video, or playback is paused).
     */
    MPV_EVENT_TICK              = 5,
    /**
     * Reply to a mpv_get_property_async() request.
     *
     * This is bound to the request referenced in the in_reply_to field.
     */
    MPV_EVENT_PROPERTY          = 6,
    /**
     * Happens before playback start of a file.
     */
    MPV_EVENT_START_FILE        = 7,
    /**
     * Happens after playback if a file has finished, and the file was unloaded.
     */
    MPV_EVENT_END_FILE          = 8,
    /**
     * Happens after the file has been loaded (headers read etc.), and decoding
     * starts.
     */
    MPV_EVENT_PLAYBACK_START    = 9,
    /**
     * The list of video/audio/subtitle tracks was changed.
     */
    MPV_EVENT_TRACKS_CHANGED    = 10,
    /**
     * A video/audio/subtitle track was switched on or off.
     */
    MPV_EVENT_TRACK_SWITCHED    = 11,
    /**
     * Idle mode was entered. In this mode, no file is played, and the playback
     * core waits for new commands.
     */
    MPV_EVENT_IDLE              = 12,
    /**
     * Playback was paused.
     */
    MPV_EVENT_PAUSE             = 13,
    /**
     * Playback was unpaused.
     */
    MPV_EVENT_UNPAUSE           = 14,
    /**
     * Triggered by the script_dispatch input command. The command uses the
     * client name (see mpv_client_name()) to dispatch keyboard or mouse input
     * to a client.
     */
    MPV_EVENT_SCRIPT_INPUT_DISPATCH = 15,
} mpv_event;

/**
 * Return a string describing the event. For unknown events, NULL is returned.
 *
 * Note that all events actually returned by the API will also yield a non-NULL
 * string with this function.
 *
 * @param event event ID, see see enum mpv_event
 * @return A static string giving a short symbolic name of the event. It
 *         consists of lower-case ASCII characters and can include "-"
 *         characters. This string is suitable for use in e.g. scripting
 *         interfaces.
 *         The string is completely static, i.e. doesn't need to be deallocated,
 *         and is valid forever.
 */
const char *mpv_event_name(mpv_event event);

typedef struct mpv_event_property {
    /**
     * Name of the property.
     */
    const char *name;
    /**
     * Format of the given data. See enum mpv_format.
     */
    mpv_format format;
    /**
     * Received property value. Depends on the format.
     */
    void *data;
} mpv_event_property;

typedef struct mpv_event_log_message {
    /**
     * The module prefix, identifies the sender of the message.
     */
    const char *prefix;
    /**
     * The log level as string. See mpv_request_log_messages() for possible
     * values.
     */
    const char *level;
    /**
     * The log message. Note that this is the direct output of a printf()
     * style output API. The text will contain embedded newlines, and it's
     * possible that a single message contains multiple lines, or that a
     * message contains a partial line. It's safe to display messages only
     * if they end with a newline character, and to buffer them otherwise.
     */
    const char *text;
} mpv_event_log_message;

typedef struct mpv_event_script_input_dispatch {
    /**
     * Arbitrary integer value that was provided as argument to the
     * script_dispatch input command.
     */
    int arg0;
    /**
     * Type of the input. Currently either "keyup_follows" (basically a key
     * down event), or "press" (either a single key event, or a key up event
     * following a "keyup_follows" event).
     */
    const char *type;
} mpv_event_script_input_dispatch;

typedef struct mpv_event_data {
    /**
     * If the event is in reply to a request (made with this API and this
     * API handle), this is set to the reply ID used by the request.
     * Otherwise, this field is 0.
     */
    mpv_reply_id in_reply_to;
    /**
     * One of mpv_event. Keep in mind that later ABI compatible releases might
     * add new event types. These should be ignored by the API user.
     */
    mpv_event event_id;
    /**
     * This is used for MPV_EVENT_ERROR only, and contains the error code.
     * It is set to 0 for all other events.
     */
    int error;
    /**
     * The meaning and contents of data member depends on the event_id:
     *  MPV_EVENT_PROPERTY:     struct mpv_event_property
     *  MPV_EVENT_LOG_MESSAGE:  struct mpv_event_log_message
     *  MPV_EVENT_SCRIPT_INPUT_DISPATCH: struct mpv_event_script_input_dispatch
     *  other: NULL, or ignore the value if it's not NULL
     */
    void *data;
} mpv_event_data;

/**
 * Enable or disable the given event.
 *
 * Some events are enabled by default, and some events can't be disabled.
 *
 * (Informational note: currently, all events are enabled by default, except
 *  MPV_EVENT_TICK.)
 *
 * @param event See enum mpv_event.
 * @param enable 1 to enable receiving this event, 0 to disable it.
 * @return error code
 */
int mpv_request_event(mpv_handle *ctx, mpv_event event, int enable);

/**
 * Enable or disable receiving of log messages. These are the messages the
 * command line player prints to the terminal. This call sets the minimum
 * required log level for a message to be received with MPV_EVENT_LOG_MESSAGE.
 *
 * @param min_level Minimal log level as string. Valid log levels:
 *                      no fatal error warn info status v debug trace
 *                  The value "no" disables all messages. This is the default.
 */
int mpv_request_log_messages(mpv_handle *ctx, const char *min_level);

/**
 * Wait for the next event, or until the timeout expires, or if a call to
 * mpv_wakeup() is made.
 *
 * Only one thread is allowed to call this at a time. The API won't complain
 * if more than one thread calls this, but it will cause race conditions in
 * the client when accessing the shared mpv_event struct. Note that most other
 * API functions are not restricted by this, and no API function internally
 * calls mpv_wait_event().
 *
 * @param timeout Timeout in seconds, after which the function returns even if
 *                no event was received. A MPV_EVENT_NONE is returned on
 *                timeout.
 * @return A struct containing the event ID and other data. The pointer (and
 *         fields in the struct) stay valid until the next mpv_wait_event()
 *         call, or until mpv_destroy() is called. You must not write to
 *         the struct, and all memory referenced by it will be automatically
 *         released by the API. The return value is never NULL. If the function
 *         itself encounters an error, the event is set to MPV_EVENT_ERROR
 *         with in_reply_to set to 0.
 */
mpv_event_data *mpv_wait_event(mpv_handle *ctx, double timeout);

/**
 * Interrupt the current mpv_wait_event() call. This will wake up the thread
 * currently waiting in mpv_wait_event(). If no thread is waiting, the next
 * mpv_wait_event() call will immediately return (this is to avoid lost
 * wakeups).
 *
 * mpv_wait_event() will receive a MPV_EVENT_NONE if it's woken up due to
 * this call. But note that this dummy event might be skipped if there are
 * already another events queued. All what counts is that the waiting thread
 * is woken up.
 */
void mpv_wakeup(mpv_handle *ctx);

/**
 * Set a custom function that should be called when there are new events. Use
 * this if blocking in mpv_wait_event() to wait for new events is not feasible.
 * Keep in mind that the callback will be called from foreign threads. You
 * must not make any assumptions of the environment, and you must return as
 * soon as possible. You are not allowed to call any client API functions
 * inside of the callback. In particular, you should not do any processing in
 * the callback, but wake up another thread that does all the work.
 *
 * In general, the client API expects you to call mpv_wait_event() to receive
 * notifications, and the wakeup callback is merely a helper utility to make
 * this easier in certain situations.
 *
 * If you actually want to do processing in a callback, spawn a thread that
 * does nothing but call mpv_wait_event() in a loop and dispatches the result
 * to a callback.
 */
void mpv_set_wakeup_callback(mpv_handle *ctx, void (*cb)(void *d), void *d);

#ifdef __cplusplus
}
#endif

#endif

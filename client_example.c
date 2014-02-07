#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "player/client_api.h"

static void check_error(int status)
{
    if (status < 0) {
        printf("mpv API error: %s\n", mpv_error_string(status));
        exit(1);
    }
}

int main(int argc, char *argv[])
{
    mpv_handle *ctx = mpv_create();
    if (!ctx) {
        printf("failed creating context\n");
        return 1;
    }

    check_error(mpv_initialize(ctx));

    // Load a random file.
    check_error(mpv_command_string(ctx, "loadfile test.mkv"));

    // Create another player, because why the hell not? (no error checking)
    mpv_handle *ctx2 = mpv_create();
    if (!ctx2) {
        printf("failed creating context (2)\n");
        return 1;
    }
    check_error(mpv_set_option_string(ctx2, "title", "number 2"));
    check_error(mpv_initialize(ctx2));
    check_error(mpv_command_string(ctx2, "loadfile test.mkv"));

    // Let it play, and wait until the user quits.
    while (1) {
        mpv_event *event = mpv_wait_event(ctx, 10000);
        printf("event: %s\n", mpv_event_name(event->event_id));
        if (event->event_id == MPV_EVENT_SHUTDOWN)
            break;
    }

    check_error(mpv_command_string(ctx, "quit"));
    check_error(mpv_command_string(ctx2, "quit"));

    mpv_destroy(ctx);
    mpv_destroy(ctx2);
    return 0;
}

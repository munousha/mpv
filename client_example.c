#include <stddef.h>
#include <stdio.h>

#include "player/client_api.h"

int main(int argc, char *argv[])
{
    mpv_handle *ctx = mpv_create();
    if (!ctx) {
        printf("failed creating context\n");
        return 1;
    }

    if (mpv_initialize(ctx) < 0) {
        printf("failed initializing\n");
        return 1;
    }

    // Load a random file.
    mpv_command_string(ctx, "loadfile test.mkv");

    // Create another player, because why the hell not? (no error checking)
    mpv_handle *ctx2 = mpv_create();
    mpv_set_option_string(ctx2, "title", "number 2");
    mpv_initialize(ctx2);
    mpv_command_string(ctx2, "loadfile test.mkv");

    // Let it play, and wait until the user quits.
    while (1) {
        mpv_event_data *event = mpv_wait_event(ctx, 10000);
        printf("event: %s\n", mpv_event_name(event->event_id));
        if (event->event_id == MPV_EVENT_SHUTDOWN)
            break;
    }

    mpv_destroy(ctx);
    mpv_destroy(ctx2);
    return 0;
}

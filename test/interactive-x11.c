/*
 * Copyright © 2013 Ran Benita <ran234@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <locale.h>

#include "xkbcommon/xkbcommon-x11.h"
#include "test.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <xcb/xkb.h>
#pragma GCC diagnostic pop

/*
 * Note: This program only handles the core keyboard device for now.
 * It should be straigtforward to change struct keyboard to a list of
 * keyboards with device IDs, as in test/interactive-x11.c. This would
 * require:
 *
 * - Initially listing the keyboard devices.
 * - Listening to device changes.
 * - Matching events to their devices.
 *
 * XKB itself knows about xinput1 devices, and most requests and events are
 * device-specific.
 *
 * In order to list the devices and react to changes, you need xinput1/2.
 * You also need xinput for the key press/release event, since the core
 * protocol key press event does not carry a device ID to match on.
 */

struct keyboard {
    xcb_connection_t *conn;
    uint8_t first_xkb_event;
    struct xkb_context *ctx;

    struct xkb_keymap *keymap;
    struct xkb_state *state;
    int32_t device_id;
};

static bool terminate;

static int
select_xkb_events_for_device(xcb_connection_t *conn, int32_t device_id)
{
    static const xcb_xkb_map_part_t required_map_parts =
        (XCB_XKB_MAP_PART_KEY_TYPES |
         XCB_XKB_MAP_PART_KEY_SYMS |
         XCB_XKB_MAP_PART_MODIFIER_MAP |
         XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS |
         XCB_XKB_MAP_PART_KEY_ACTIONS |
         XCB_XKB_MAP_PART_VIRTUAL_MODS |
         XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP);

    static const xcb_xkb_event_type_t required_events =
        (XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY |
         XCB_XKB_EVENT_TYPE_MAP_NOTIFY |
         XCB_XKB_EVENT_TYPE_STATE_NOTIFY);

    xcb_void_cookie_t cookie =
        xcb_xkb_select_events_checked(conn,
                                      device_id,
                                      required_events,
                                      0,
                                      required_events,
                                      required_map_parts,
                                      required_map_parts,
                                      0);

    xcb_generic_error_t *error = xcb_request_check(conn, cookie);
    if (error) {
        free(error);
        return -1;
    }

    return 0;
}

static int
update_keymap(struct keyboard *kbd)
{
    struct xkb_keymap *new_keymap;
    struct xkb_state *new_state;

    new_keymap = xkb_x11_keymap_new_for_device(kbd->ctx, kbd->conn,
                                               kbd->device_id, 0);
    if (!new_keymap)
        goto err_out;

    new_state = xkb_x11_state_new_for_device(new_keymap, kbd->conn,
                                             kbd->device_id);
    if (!new_state)
        goto err_keymap;

    if (kbd->keymap)
        printf("Keymap updated!\n");

    xkb_state_unref(kbd->state);
    xkb_keymap_unref(kbd->keymap);
    kbd->keymap = new_keymap;
    kbd->state = new_state;
    return 0;

err_keymap:
    xkb_keymap_unref(new_keymap);
err_out:
    return -1;
}

static int
init_kbd(struct keyboard *kbd, xcb_connection_t *conn, uint8_t first_xkb_event,
         int32_t device_id, struct xkb_context *ctx)
{
    int ret;

    kbd->conn = conn;
    kbd->first_xkb_event = first_xkb_event;
    kbd->ctx = ctx;
    kbd->keymap = NULL;
    kbd->state = NULL;
    kbd->device_id = device_id;

    ret = update_keymap(kbd);
    if (ret)
        goto err_out;

    ret = select_xkb_events_for_device(conn, device_id);
    if (ret)
        goto err_state;

    return 0;

err_state:
    xkb_state_unref(kbd->state);
    xkb_keymap_unref(kbd->keymap);
err_out:
    return -1;
}

static void
deinit_kbd(struct keyboard *kbd)
{
    xkb_state_unref(kbd->state);
    xkb_keymap_unref(kbd->keymap);
}

static void
process_xkb_event(xcb_generic_event_t *gevent, struct keyboard *kbd)
{
    union xkb_event {
        struct {
            uint8_t response_type;
            uint8_t xkbType;
            uint16_t sequence;
            xcb_timestamp_t time;
            uint8_t deviceID;
        } any;
        xcb_xkb_new_keyboard_notify_event_t new_keyboard_notify;
        xcb_xkb_map_notify_event_t map_notify;
        xcb_xkb_state_notify_event_t state_notify;
    } *xkb_event = (union xkb_event *) gevent;

    /*
     * XkbNewKkdNotify and XkbMapNotify together capture all sorts of keymap
     * updates (e.g. xmodmap, xkbcomp, setxkbmap), with minimal redundent
     * recompilations.
     */
    switch (xkb_event->any.xkbType) {
    case XCB_XKB_NEW_KEYBOARD_NOTIFY: {
        xcb_xkb_new_keyboard_notify_event_t *event =
            &xkb_event->new_keyboard_notify;

        if (event->deviceID == kbd->device_id &&
            event->changed & XCB_XKB_NKN_DETAIL_KEYCODES) {
            update_keymap(kbd);
        }

        break;
    }
    case XCB_XKB_MAP_NOTIFY: {
        xcb_xkb_map_notify_event_t *event = &xkb_event->map_notify;

        if (event->deviceID == kbd->device_id) {
            update_keymap(kbd);
        }

        break;
    }
    case XCB_XKB_STATE_NOTIFY: {
        xcb_xkb_state_notify_event_t *event = &xkb_event->state_notify;

        if (event->deviceID == kbd->device_id) {
            xkb_state_update_mask(kbd->state,
                                  event->baseMods,
                                  event->latchedMods,
                                  event->lockedMods,
                                  event->baseGroup,
                                  event->latchedGroup,
                                  event->lockedGroup);
        }

        break;
    }
    }
}

static void
process_event(xcb_generic_event_t *gevent, struct keyboard *kbd)
{
    switch (gevent->response_type) {
    case XCB_KEY_PRESS: {
        xcb_key_press_event_t *event = (xcb_key_press_event_t *) gevent;
        xkb_keycode_t keycode = event->detail;

        test_print_keycode_state(kbd->state, keycode);

        /* Exit on ESC. */
        if (keycode == 9)
            terminate = true;
        break;
    }
    default:
        if (gevent->response_type == kbd->first_xkb_event)
            process_xkb_event(gevent, kbd);
        break;
    }
}

static int
loop(xcb_connection_t *conn, struct keyboard *kbd)
{
    while (!terminate) {
        xcb_generic_event_t *event = xcb_wait_for_event(conn);
        process_event(event, kbd);
        free(event);
    }

    return 0;
}

static int
create_capture_window(xcb_connection_t *conn)
{
    xcb_generic_error_t *error;
    xcb_void_cookie_t cookie;
    xcb_screen_t *screen =
        xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    xcb_window_t window = xcb_generate_id(conn);
    uint32_t values[2] = {
        screen->white_pixel,
        XCB_EVENT_MASK_KEY_PRESS,
    };

    cookie = xcb_create_window_checked(conn, XCB_COPY_FROM_PARENT,
                                       window, screen->root,
                                       10, 10, 100, 100, 1,
                                       XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                       screen->root_visual,
                                       XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
                                       values);
    if ((error = xcb_request_check(conn, cookie)) != NULL) {
        free(error);
        return -1;
    }

    cookie = xcb_map_window_checked(conn, window);
    if ((error = xcb_request_check(conn, cookie)) != NULL) {
        free(error);
        return -1;
    }

    xcb_flush(conn);
    return 0;
}

int
main(int argc, char *argv[])
{
    int ret;
    xcb_connection_t *conn;
    uint8_t first_xkb_event;
    int32_t core_kbd_device_id;
    struct xkb_context *ctx;
    struct keyboard core_kbd;

    setlocale(LC_ALL, "");

    conn = xcb_connect(NULL, NULL);
    if (!conn) {
        fprintf(stderr, "Couldn't connect to X server\n");
        ret = -1;
        goto err_out;
    }

    ret = xkb_x11_util_setup_extension(conn,
                                       XCB_XKB_MAJOR_VERSION,
                                       XCB_XKB_MINOR_VERSION,
                                       0, NULL, NULL, &first_xkb_event, NULL);
    if (!ret) {
        fprintf(stderr, "Couldn't setup XKB extension\n");
        goto err_conn;
    }

    ctx = test_get_context(0);
    if (!ctx) {
        ret = -1;
        fprintf(stderr, "Couldn't create xkb context\n");
        goto err_conn;
    }

    core_kbd_device_id = xkb_x11_util_get_core_keyboard_device_id(conn);
    if (core_kbd_device_id == -1) {
        fprintf(stderr, "Couldn't find core keyboard device\n");
        goto err_ctx;
    }

    ret = init_kbd(&core_kbd, conn, first_xkb_event, core_kbd_device_id, ctx);
    if (ret) {
        fprintf(stderr, "Couldn't initialize core keyboard device\n");
        goto err_ctx;
    }

    ret = create_capture_window(conn);
    if (ret) {
        fprintf(stderr, "Couldn't create a capture window\n");
        goto err_core_kbd;
    }

    system("stty -echo");
    ret = loop(conn, &core_kbd);
    system("stty echo");

err_core_kbd:
    deinit_kbd(&core_kbd);
err_ctx:
    xkb_context_unref(ctx);
err_conn:
    xcb_disconnect(conn);
err_out:
    exit(ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

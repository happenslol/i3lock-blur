/*
 * vim:ts=4:sw=4:expandtab
 *
 * © 2010 Michael Stapelberg
 *
 * See LICENSE for licensing information
 *
 */
#include <X11/Xlib.h>
#include <cairo.h>
#include <cairo/cairo-xcb.h>
#include <ev.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

#include "blur.h"
#include "i3lock.h"
#include "unlock_indicator.h"
#include "xcb.h"
#include "randr.h"

/*******************************************************************************
 * Variables defined in i3lock.c.
 ******************************************************************************/

extern bool debug_mode;

/* The current position in the input buffer. Useful to determine if any
 * characters of the password have already been entered or not. */
int input_position;
static int last_input_position = 0;

/* The lock window. */
extern xcb_window_t win;

/* The current resolution of the X11 root window. */
extern uint32_t last_resolution[2];
uint32_t button_diameter_physical;

/* List of pressed modifiers, or NULL if none are pressed. */
extern char *modifier_string;

/* A Cairo surface containing the specified image (-i), if any. */
extern cairo_surface_t *img;
extern bool dpms_capable;
extern int blur_radius;
extern float blur_sigma;
/* to blur the screen only once */
extern bool live;
/* The background color to use (in hex). */
extern char color[7];
extern Display *display;

/* Whether the failed attempts should be displayed. */
extern bool show_failed_attempts;
/* Number of failed unlock attempts. */
extern int failed_attempts;

/*******************************************************************************
 * Variables defined in xcb.c.
 ******************************************************************************/

/* The root screen, to determine the DPI. */
extern xcb_screen_t *screen;

/*******************************************************************************
 * Local variables.
 ******************************************************************************/

/* Cache the screen’s visual, necessary for creating a Cairo context. */
static xcb_visualtype_t *vistype;

/* Maintain the current unlock/PAM state to draw the appropriate unlock
 * indicator. */
unlock_state_t unlock_state;
auth_state_t auth_state;

/* A surface for the unlock indicator */
static cairo_surface_t *unlock_indicator_surface = NULL;

/*
 * Returns the scaling factor of the current screen. E.g., on a 227 DPI MacBook
 * Pro 13" Retina screen, the scaling factor is 227/96 = 2.36.
 *
 */
/* static double scaling_factor(void) { */
/*     const int dpi = (double)screen->height_in_pixels * 25.4 / */
/*                     (double)screen->height_in_millimeters; */
/*     return (dpi / 96.0); */
/* } */

static void string_repeat(char *dest, const char *str, int n) {
    if (n <= 0) return;

    char *pa, *pb;
    int slen = strlen(str);

    pa = dest + (n-1)*slen;
    strcpy(pa, str);
    pb = --pa + slen; 
    while (pa>=dest) *pa-- = *pb--;
}

static void draw_unlock_indicator() {
    /* Initialise the surface if not yet done */
    if (unlock_indicator_surface == NULL) {
        unlock_indicator_surface = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32,
            screen->width_in_pixels,
            screen->height_in_pixels
        );
    }

    cairo_t *ctx = cairo_create(unlock_indicator_surface);

    /* clear the surface */
    cairo_save(ctx);
    cairo_set_operator(ctx, CAIRO_OPERATOR_CLEAR);
    cairo_paint(ctx);
    cairo_restore(ctx);

    if (
        unlock_state >= STATE_KEY_PRESSED ||
        auth_state > STATE_AUTH_IDLE ||
        input_position > 0
    ) {
        if (input_position > 0)
            last_input_position = input_position;

        /* Display a (centered) text of the current PAM state. */
        char* text = malloc(256);

        if (auth_state == STATE_AUTH_WRONG || auth_state == STATE_I3LOCK_LOCK_FAILED)
            string_repeat(text, "•", last_input_position);
        else
            string_repeat(text, "•", input_position);

        cairo_select_font_face(
            ctx, "sans-serif",
            CAIRO_FONT_SLANT_NORMAL,
            CAIRO_FONT_WEIGHT_NORMAL
        );

        cairo_set_font_size(ctx, 80.0);

        cairo_text_extents_t extents;
        double x, y;

        switch (auth_state) {
            case STATE_AUTH_VERIFY:
            case STATE_AUTH_LOCK:
                cairo_set_source_rgb(ctx, 84.0f / 255, 110.0f / 255, 122.0f / 255);
                break;
            case STATE_AUTH_WRONG:
            case STATE_I3LOCK_LOCK_FAILED:
                if (unlock_state < STATE_KEY_PRESSED)
                    cairo_set_source_rgb(ctx, 255.0f / 255, 83.0f / 255, 112.0f / 255);
                else
                    cairo_set_source_rgb(ctx, 1, 1, 1);
                break;
            default:
                if (unlock_state == STATE_NOTHING_TO_DELETE) strcpy(text, "");
                cairo_set_source_rgb(ctx, 1, 1, 1);
                break;
        }

        cairo_text_extents(ctx, text, &extents);

        x = (screen->width_in_pixels / 2) - (extents.width / 2);
        y = (screen->height_in_pixels / 2) - (extents.height / 2);
        cairo_move_to(ctx, x, y);
        cairo_show_text(ctx, text);

        free(text);
    }

    cairo_destroy(ctx);
}

/*
 * Draws global image with fill color onto a pixmap with the given
 * resolution and returns it.
 *
 */
xcb_pixmap_t draw_image(uint32_t *resolution) {
    xcb_pixmap_t bg_pixmap = XCB_NONE;

    if (!vistype)
        vistype = get_root_visual_type(screen);

    bg_pixmap = create_fg_pixmap(conn, screen, resolution);

    /* Initialize cairo: Create one in-memory surface to render the unlock
     * indicator on, create one XCB surface to actually draw (one or more,
     * depending on the amount of screens) unlock indicators on. */
    cairo_surface_t *xcb_output = cairo_xcb_surface_create(
        conn, bg_pixmap, vistype,
        resolution[0], resolution[1]
    );

    cairo_t *xcb_ctx = cairo_create(xcb_output);

    if (live) {
        post_process_pixmap(0, bg_pixmap, last_resolution[0], last_resolution[1]);

        cairo_surface_t *tmp = cairo_xcb_surface_create(
            conn, bg_pixmap, get_root_visual_type(screen),
            last_resolution[0], last_resolution[1]
        );

        cairo_set_source_surface(xcb_ctx, tmp, 0, 0);
        cairo_paint(xcb_ctx);
        cairo_surface_destroy(tmp);
    } else {
        cairo_set_source_surface(xcb_ctx, img, 0, 0);
        cairo_paint(xcb_ctx);
    }

    if (xr_screens > 0) {
        // TODO: Only show indicator on the main screen
        /* Composite the unlock indicator in the middle of each screen. */
        for (int screen = 0; screen < xr_screens; screen++) {
            int w = xr_resolutions[screen].width;
            int h = xr_resolutions[screen].height;
            int x = xr_resolutions[screen].x;
            int y = xr_resolutions[screen].y;

            cairo_set_source_surface(xcb_ctx, unlock_indicator_surface, x, y);
            cairo_rectangle(xcb_ctx, x, y, w, h);
            cairo_fill(xcb_ctx);
        }
    } else {
        /* We have no information about the screen sizes/positions, so we just
         * place the unlock indicator in the middle of the X root window and
         * hope for the best. */
        int w = last_resolution[0];
        int h = last_resolution[1];
        int x = 0;
        int y = 0;
        cairo_set_source_surface(xcb_ctx, unlock_indicator_surface, x, y);
        cairo_rectangle(xcb_ctx, x, y, w, h);
        cairo_fill(xcb_ctx);
    }

    cairo_surface_destroy(xcb_output);
    cairo_destroy(xcb_ctx);
    return bg_pixmap;
}

/*
 * Calls draw_image on a new pixmap and swaps that with the current pixmap
 *
 */
void redraw_screen(void) {
    /* avoid drawing if monitor state is not on */
    if (dpms_capable) {
        xcb_dpms_info_reply_t *dpms_info =
            xcb_dpms_info_reply(conn, xcb_dpms_info(conn), NULL);

        if (dpms_info) {
            /* monitor is off when DPMS state is enabled and power level is not
             * DPMS_MODE_ON */
            uint8_t monitor_off =
                dpms_info->state &&
                dpms_info->power_level != XCB_DPMS_DPMS_MODE_ON;

            free(dpms_info);
            if (monitor_off)
                return;
        }
    }

    DEBUG(
        "redraw_screen(unlock_state = %d, auth_state = %d)\n",
        unlock_state, auth_state
    );

    xcb_pixmap_t bg_pixmap = draw_image(last_resolution);
    xcb_change_window_attributes(
        conn, win, XCB_CW_BACK_PIXMAP,
        (uint32_t[1]){bg_pixmap}
    );

    xcb_clear_area(conn, 0, win, 0, 0, last_resolution[0], last_resolution[1]);
    xcb_flush(conn);
}

/*
 * Redraws screen and also redraws unlock indicator
 *
 */
void redraw_unlock_indicator(void) {
    draw_unlock_indicator();
    redraw_screen();
}

/*
 * Hides the unlock indicator completely when there is no content in the
 * password buffer.
 *
 */
void clear_indicator(void) {
    if (input_position == 0) {
        unlock_state = STATE_STARTED;
    } else
        unlock_state = STATE_KEY_PRESSED;

    redraw_unlock_indicator();
}

/*
 * Clears the old unlock indicator surface so
 */

void resize_screen(void) {
    cairo_surface_destroy(unlock_indicator_surface);
    unlock_indicator_surface = NULL;
}

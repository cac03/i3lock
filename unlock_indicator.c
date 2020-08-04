/*
 * vim:ts=4:sw=4:expandtab
 *
 * © 2010-2014 Michael Stapelberg
 *
 * See LICENSE for licensing information
 *
 */
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <xcb/xcb.h>
#include <ev.h>
#include <cairo.h>
#include <cairo/cairo-xcb.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XKBrules.h>

#include "i3lock.h"
#include "xcb.h"
#include "unlock_indicator.h"
#include "xinerama.h"

extern int BUTTON_RADIUS;
extern double PRIMARY_FONT_SIZE;
extern double SECONDARY_FONT_SIZE;
#define BUTTON_SPACE (BUTTON_RADIUS + 5)
#define BUTTON_CENTER (BUTTON_RADIUS + 5)
#define BUTTON_DIAMETER (2 * BUTTON_SPACE)
#define TIME_FORMAT_12 "%l:%M %p"
#define TIME_FORMAT_24 "%k:%M"

/*******************************************************************************
 * Variables defined in i3lock.c.
 ******************************************************************************/
static struct ev_periodic *time_redraw_tick;

extern bool debug_mode;

/* The current position in the input buffer. Useful to determine if any
 * characters of the password have already been entered or not. 
 */
extern int input_position;

/* The lock window. */
extern xcb_window_t win;

/* The current resolution of the X11 root window. */
extern uint32_t last_resolution[2];

/* Whether the unlock indicator is enabled (defaults to true). */
extern bool unlock_indicator;

/* A Cairo surface containing the specified image (-i), if any. */
extern cairo_surface_t *img;

/* Whether the image should be tiled. */
extern bool tile;

/* The background color to use (in hex). */
extern char color[7];

/* Verify color to use (in hex). */
extern char verifycolor[7];

/* Wrong/Error color to use (in hex). */
extern char wrongcolor[7];

/* Idle color to use (in hex). */
extern char idlecolor[7];

/* Use 24 hour time format */
extern bool use24hour;

/* Number of failed unlock attempts. */
extern int failed_attempts;

/* The display, to determine if Caps Lock is triggered and to determine keyboard layout*/
extern Display* _display;

/* Char array holding password */
extern char password[512];

/* Strings representing keyboard layouts group */
extern char kb_layouts_group[][3];

/*
 * Current password length
 * Symbols from non ASCII table require more than one byte to be stored in password array
 * so we have to store current password length which determined independently of password string length
 * Not sure we really need it.
 * */
extern int password_length;

/*
 * Allows user to select main circle opacity
 * Passes as argument to program with -O option
 * */
extern double CIRCLE_OPACITY;

/*
* Allows user to select main circle opacity.
* Passes as argument to program with -T option
* */
extern double LINE_AND_TEXT_OPACITY;

/* Allows user to select whether he wants see caps lock state */
extern int SHOW_CAPS_LOCK_STATE;

/* Allows user to select whether he wants see current keyboard layout */
extern int SHOW_KEYBOARD_LAYOUT;

/* Allow user to select whether he wants see input visualisation */
extern int SHOW_INPUT_VISUALISATION;

/* Allow user to select unlock indicator position */
extern bool USE_CUSTOM_INDICATOR_POSITION;

/* User specified offsets of the unlock indicator from right bottom corner */
extern int unlock_indicator_offset_x;
extern int unlock_indicator_offset_y;


/*******************************************************************************
 * Variables defined in xcb.c.
 ******************************************************************************/

/* The root screen, to determine the DPI. */
extern xcb_screen_t *screen;

/*******************************************************************************
 * Local variables.
 ******************************************************************************/

/* Caps lock state string showing when caps lock is active */
char CAPS_LOCK_STRING[] = "CAPS";

/* Cache the screen’s visual, necessary for creating a Cairo context. */
static xcb_visualtype_t *vistype;

/* Maintain the current unlock/PAM state to draw the appropriate unlock
 * indicator. 
 */
unlock_state_t unlock_state;
pam_state_t pam_state;

/* Keyboard state to determine keyboard layout */
extern XkbStateRec xkbState;

/* Used to determine caps lock state */
XKeyboardState xKeyboardState;

/*
 * Returns the scaling factor of the current screen. E.g., on a 227 DPI MacBook
 * Pro 13" Retina screen, the scaling factor is 227/96 = 2.36.
 */
static double scaling_factor(void) {
    const int dpi = (double)screen->height_in_pixels * 25.4 /
                    (double)screen->height_in_millimeters;
    return (dpi / 96.0);
}

/*
 * Draws global image with fill color onto a pixmap with the given
 * resolution and returns it.
 */
xcb_pixmap_t draw_image(uint32_t *resolution) {
    xcb_pixmap_t bg_pixmap = XCB_NONE;
    int button_diameter_physical = ceil(scaling_factor() * BUTTON_DIAMETER);
    DEBUG("scaling_factor is %.f, physical diameter is %d px\n",
          scaling_factor(), button_diameter_physical);

    if (!vistype)
        vistype = get_root_visual_type(screen);
    bg_pixmap = create_bg_pixmap(conn, screen, resolution, color);
    /* 
     * Initialize cairo: Create one in-memory surface to render the unlock
     * indicator on, create one XCB surface to actually draw (one or more,
     * depending on the amount of screens) unlock indicators on. 
     */
    cairo_surface_t *output = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, button_diameter_physical, button_diameter_physical);
    cairo_t *ctx = cairo_create(output);

    cairo_surface_t *xcb_output = cairo_xcb_surface_create(conn, bg_pixmap, vistype, resolution[0], resolution[1]);
    cairo_t *xcb_ctx = cairo_create(xcb_output);

    /* Creates color array from command line arguments */
    uint32_t * color_array(char* colorarg) {
        uint32_t *rgb16 = malloc(sizeof(uint32_t)*3);

        char strgroups[3][3] = {{colorarg[0], colorarg[1], '\0'},
                                {colorarg[2], colorarg[3], '\0'},
                                {colorarg[4], colorarg[5], '\0'}};

        for (int i=0; i < 3; i++) {
            rgb16[i] = strtol(strgroups[i], NULL, 16);
        }

        return rgb16;
    }

    /* Sets the color based on argument (color/background, verify, wrong, idle)
     * and type (line, background and fill). Type defines alpha value and tint.
     * Utilizes color_array() and frees after use.
     */
    void set_color(cairo_t *cr, char *colorarg, char colortype) {
        uint32_t *rgb16 = color_array(colorarg);

        switch(colortype) {
            case 'b': /* Background */
                cairo_set_source_rgb(cr, rgb16[0] / 255.0, rgb16[1] / 255.0, rgb16[2] / 255.0);
                break;
            case 'l': /* Line and text */
                cairo_set_source_rgba(cr, rgb16[0] / 255.0, rgb16[1] / 255.0, rgb16[2] / 255.0, LINE_AND_TEXT_OPACITY);
                break;
            case 'f': /* Fill */
                /* Use a lighter tint of the user defined color for circle fill */

                for (int i=0; i < 3; i++) {
                    rgb16[i] = ((255 - rgb16[i]) * .5) + rgb16[i];
                }

                cairo_set_source_rgba(cr, rgb16[0] / 255.0, rgb16[1] / 255.0, rgb16[2] / 255.0, CIRCLE_OPACITY);
                break;
        }
        free(rgb16);
    }

    if (img) {
        if (!tile) {
            cairo_set_source_surface(xcb_ctx, img, 0, 0);
            cairo_paint(xcb_ctx);
        } else {
            /* create a pattern and fill a rectangle as big as the screen */
            cairo_pattern_t *pattern;
            pattern = cairo_pattern_create_for_surface(img);
            cairo_set_source(xcb_ctx, pattern);
            cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
            cairo_rectangle(xcb_ctx, 0, 0, resolution[0], resolution[1]);
            cairo_fill(xcb_ctx);
            cairo_pattern_destroy(pattern);
        }
    } else {
        set_color(xcb_ctx,color,'b'); /* If not image, use color to fill background */
        cairo_rectangle(xcb_ctx, 0, 0, resolution[0], resolution[1]);
        cairo_fill(xcb_ctx);
    }

    if (unlock_indicator) {
        cairo_scale(ctx, scaling_factor(), scaling_factor());
        /* Draw a (centered) circle with transparent background. */
        cairo_set_line_width(ctx, 3.0);
        cairo_arc(ctx,
                  BUTTON_CENTER /* x */,
                  BUTTON_CENTER /* y */,
                  BUTTON_RADIUS /* radius */,
                  0 /* start */,
                  2 * M_PI /* end */);

        /* Use the appropriate color for the different PAM states
         * (currently verifying, wrong password, or idle) 
         */

        void set_pam_color(char colortype) {
            switch (pam_state) {
                case STATE_PAM_VERIFY:
                    set_color(ctx,verifycolor,colortype);
                    break;
                case STATE_PAM_WRONG:
                    set_color(ctx,wrongcolor,colortype);
                    break;
                case STATE_PAM_IDLE:
                    if (unlock_state == STATE_BACKSPACE_ACTIVE) {
                        set_color(ctx,wrongcolor,colortype);
                    }
                    else {
                        set_color(ctx,idlecolor,colortype);  
                    }
                    break;
            }
        }

        set_pam_color('f');
        cairo_fill_preserve(ctx);

        /* Circle border */
        set_pam_color('l');
        cairo_stroke(ctx);

        /* Display (centered) Time */
        char *timetext = malloc(10);

        time_t curtime = time(NULL);
        struct tm *tm = localtime(&curtime);
        if (use24hour)
            strftime(timetext, 100, TIME_FORMAT_24, tm);
        else
            strftime(timetext, 100, TIME_FORMAT_12, tm);

        /* Text */
        set_pam_color('l');
        cairo_set_font_size(ctx, PRIMARY_FONT_SIZE);

        cairo_text_extents_t time_extents;
        double time_x, time_y;

        cairo_text_extents(ctx, timetext, &time_extents);
        time_x = BUTTON_CENTER - ((time_extents.width / 2) + time_extents.x_bearing);
        time_y = BUTTON_CENTER - ((time_extents.height / 2) + time_extents.y_bearing);

        cairo_move_to(ctx, time_x, time_y);
        cairo_show_text(ctx, timetext);
        cairo_close_path(ctx);

        free(timetext);

        /* All other signs we write with secondary font size(0.6 * primary) */
        cairo_set_font_size(ctx, SECONDARY_FONT_SIZE);
        /* Input visualisation */
        if (SHOW_INPUT_VISUALISATION) {
            /* Set visualisation string */
            char *input_visualisation = malloc(password_length + 1);
            memset(input_visualisation, '*', password_length);
            input_visualisation[password_length] = '\0';


            /* Determine position */
            cairo_text_extents_t input_visualisation_extents;
            cairo_text_extents(ctx, input_visualisation, &input_visualisation_extents);
            /* At circle center */
            double input_visualisation_x = BUTTON_CENTER -
                                           (input_visualisation_extents.width / 2.0);

            double input_visualisation_y = time_y
                                           + time_extents.height
                                           - SECONDARY_FONT_SIZE / 4;

            /* Show visualisation string */
            cairo_move_to(ctx, input_visualisation_x, input_visualisation_y);
            cairo_show_text(ctx, input_visualisation);
            free(input_visualisation);
        }

        // Keyboard layout
        if (SHOW_KEYBOARD_LAYOUT) {
            char *kb_layout = kb_layouts_group[xkbState.group];

            /* Determine position */
            cairo_text_extents_t kb_layout_extents;
            cairo_text_extents(ctx, kb_layout, &kb_layout_extents);

            /* Align at right side of time text */
            double kb_layout_x = BUTTON_CENTER
                                 + (time_extents.width / 2)
                                 - kb_layout_extents.width;

            double kb_layout_y = time_y
                                 - time_extents.height
                                 - SECONDARY_FONT_SIZE / 4.0;
            XkbGetState(_display, XkbUseCoreKbd, &xkbState);

            cairo_move_to(ctx, kb_layout_x, kb_layout_y);
            cairo_show_text(ctx, kb_layouts_group[xkbState.group]);
            cairo_close_path(ctx);
        }


        /* Caps Lock state */
        if (SHOW_CAPS_LOCK_STATE) {
            double caps_lock_state_x = BUTTON_CENTER
                                       - time_extents.width / 2;
            double caps_lock_state_y = time_y
                                       - time_extents.height
                                       - SECONDARY_FONT_SIZE / 4.0;;
            XGetKeyboardControl(_display, &xKeyboardState);
            /* if caps lock is switched on */
            if (xKeyboardState.led_mask % 2 == 1) {
                cairo_move_to(ctx, caps_lock_state_x, caps_lock_state_y);
                cairo_show_text(ctx, CAPS_LOCK_STRING);
            }
        }



        /* After the user pressed any valid key or the backspace key, we
         * highlight a random part of the unlock indicator to confirm this
         * keypress. */
        if (unlock_state == STATE_KEY_ACTIVE ||
            unlock_state == STATE_BACKSPACE_ACTIVE) {
            cairo_set_line_width(ctx, 4);
            cairo_new_sub_path(ctx);
            double highlight_start = (rand() % (int)(2 * M_PI * 100)) / 100.0;
            cairo_arc(ctx,
                      BUTTON_CENTER /* x */,
                      BUTTON_CENTER /* y */,
                      BUTTON_RADIUS /* radius */,
                      highlight_start,
                      highlight_start + (M_PI / 2.5)); 

            /* Set newly drawn lines to erase what they're drawn over */
            cairo_set_operator(ctx,CAIRO_OPERATOR_CLEAR); 
            cairo_stroke(ctx);

            /* Back to normal operator */
            cairo_set_operator(ctx,CAIRO_OPERATOR_OVER); 
            cairo_set_line_width(ctx, 10);

            /* Change color of separators based on backspace/active keypress */
            set_pam_color('l');

            /* Separator 1 */
            cairo_arc(ctx,
                BUTTON_CENTER /* x */,
                BUTTON_CENTER /* y */,
                BUTTON_RADIUS /* radius */,
                highlight_start /* start */,
                highlight_start + (M_PI / 128.0) /* end */);
            cairo_stroke(ctx);

            /* Separator 2 */
            cairo_arc(ctx,
                BUTTON_CENTER /* x */,
                BUTTON_CENTER /* y */,
                BUTTON_RADIUS /* radius */,
                highlight_start + (M_PI / 2.5) /* start */,
                (highlight_start + (M_PI / 2.5)) + (M_PI / 128.0) /* end */);
            cairo_stroke(ctx);
        }
    }

    if (xr_screens > 0) {
        /* for each screen */
        for (int screen = 0; screen < xr_screens; screen++) {
            int x, y;
            if (USE_CUSTOM_INDICATOR_POSITION){
                /* Composite the unlock indicator in the right bottom corner with user specified offsets. */
                x = xr_resolutions[screen].x + xr_resolutions[screen].width - button_diameter_physical / 2 - unlock_indicator_offset_x;
                y = xr_resolutions[screen].y + xr_resolutions[screen].height - button_diameter_physical / 2 - unlock_indicator_offset_y;
            } else {
                /* Composite the unlock indicator in the middle of each screen. */
                x = (xr_resolutions[screen].x +
                         ((xr_resolutions[screen].width / 2) - (button_diameter_physical / 2)));
                y = (xr_resolutions[screen].y +
                         ((xr_resolutions[screen].height / 2) - (button_diameter_physical / 2)));
            }
            cairo_set_source_surface(xcb_ctx, output, x, y);
            cairo_rectangle(xcb_ctx, x, y, button_diameter_physical, button_diameter_physical);
            cairo_fill(xcb_ctx);
        }
    } else {
        /* We have no information about the screen sizes/positions */
        int x, y;
        if (!USE_CUSTOM_INDICATOR_POSITION) {
            /* Place the unlock indicator in the middle of the X root window and
             * hope for the best. */
            x = (last_resolution[0] / 2) - (button_diameter_physical / 2);
            y = (last_resolution[1] / 2) - (button_diameter_physical / 2);
        } else {
            /* Place the unlock indicator in the bottom right corner of the X root
             * window with user specified offsets*/
            x = last_resolution[0] - button_diameter_physical / 2 - unlock_indicator_offset_x;
            y = last_resolution[1] - button_diameter_physical / 2 - unlock_indicator_offset_y;
        }

        cairo_set_source_surface(xcb_ctx, output, x, y);
        cairo_rectangle(xcb_ctx, x, y, button_diameter_physical, button_diameter_physical);
        cairo_fill(xcb_ctx);
    }

    cairo_surface_destroy(xcb_output);
    cairo_surface_destroy(output);
    cairo_destroy(ctx);
    cairo_destroy(xcb_ctx);
    return bg_pixmap;
}

/* Calls draw_image on a new pixmap and swaps that with the current pixmap */
void redraw_screen(void) {
    xcb_pixmap_t bg_pixmap = draw_image(last_resolution);
    xcb_change_window_attributes(conn, win, XCB_CW_BACK_PIXMAP, (uint32_t[1]){ bg_pixmap });
    /* XXX: Possible optimization: Only update the area in the middle of the
     * screen instead of the whole screen. */
    xcb_clear_area(conn, 0, win, 0, 0, last_resolution[0], last_resolution[1]);
    xcb_free_pixmap(conn, bg_pixmap);
    xcb_flush(conn);
}

/* Always show unlock indicator. */

void clear_indicator(void) {
    unlock_state = STATE_KEY_PRESSED;
    redraw_screen();
}

/* Periodic redraw for clock updates - taken from github.com/ravinrabbid/i3lock-clock */

static void time_redraw_cb(struct ev_loop *loop, ev_periodic *w, int revents) {
    redraw_screen();
}

void start_time_redraw_tick(struct ev_loop* main_loop) {
    if (time_redraw_tick) {
        ev_periodic_set(time_redraw_tick, 1.0, 60., 0);
        ev_periodic_again(main_loop, time_redraw_tick);
    } else {
        /* When there is no memory, we just don’t have a timeout. We cannot
        * exit() here, since that would effectively unlock the screen. */
        if (!(time_redraw_tick = calloc(sizeof(struct ev_periodic), 1)))
        return;
        ev_periodic_init(time_redraw_tick,time_redraw_cb, 1.0, 60., 0);
        ev_periodic_start(main_loop, time_redraw_tick);
    }
}

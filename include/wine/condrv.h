/*
 * Console driver ioctls
 *
 * Copyright 2020 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef _INC_CONDRV
#define _INC_CONDRV

#include "winioctl.h"
#include "wincon.h"

/* common console input and output ioctls */
#define IOCTL_CONDRV_GET_MODE              CTL_CODE(FILE_DEVICE_CONSOLE,  0, METHOD_BUFFERED, FILE_READ_PROPERTIES)
#define IOCTL_CONDRV_SET_MODE              CTL_CODE(FILE_DEVICE_CONSOLE,  1, METHOD_BUFFERED, FILE_WRITE_PROPERTIES)

/* console input ioctls */
#define IOCTL_CONDRV_READ_INPUT            CTL_CODE(FILE_DEVICE_CONSOLE, 10, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_CONDRV_WRITE_INPUT           CTL_CODE(FILE_DEVICE_CONSOLE, 11, METHOD_BUFFERED, FILE_WRITE_PROPERTIES)
#define IOCTL_CONDRV_PEEK                  CTL_CODE(FILE_DEVICE_CONSOLE, 12, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_CONDRV_GET_INPUT_INFO        CTL_CODE(FILE_DEVICE_CONSOLE, 13, METHOD_BUFFERED, FILE_READ_PROPERTIES)
#define IOCTL_CONDRV_SET_INPUT_INFO        CTL_CODE(FILE_DEVICE_CONSOLE, 14, METHOD_BUFFERED, FILE_WRITE_PROPERTIES)
#define IOCTL_CONDRV_GET_TITLE             CTL_CODE(FILE_DEVICE_CONSOLE, 15, METHOD_BUFFERED, FILE_READ_PROPERTIES)
#define IOCTL_CONDRV_SET_TITLE             CTL_CODE(FILE_DEVICE_CONSOLE, 16, METHOD_BUFFERED, FILE_WRITE_PROPERTIES)
#define IOCTL_CONDRV_CTRL_EVENT            CTL_CODE(FILE_DEVICE_CONSOLE, 17, METHOD_BUFFERED, FILE_WRITE_PROPERTIES)

/* console output ioctls */
#define IOCTL_CONDRV_READ_OUTPUT           CTL_CODE(FILE_DEVICE_CONSOLE, 30, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_CONDRV_WRITE_OUTPUT          CTL_CODE(FILE_DEVICE_CONSOLE, 31, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_CONDRV_GET_OUTPUT_INFO       CTL_CODE(FILE_DEVICE_CONSOLE, 32, METHOD_BUFFERED, FILE_READ_PROPERTIES)
#define IOCTL_CONDRV_SET_OUTPUT_INFO       CTL_CODE(FILE_DEVICE_CONSOLE, 33, METHOD_BUFFERED, FILE_WRITE_PROPERTIES)
#define IOCTL_CONDRV_ACTIVATE              CTL_CODE(FILE_DEVICE_CONSOLE, 34, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_CONDRV_FILL_OUTPUT           CTL_CODE(FILE_DEVICE_CONSOLE, 35, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_CONDRV_SCROLL                CTL_CODE(FILE_DEVICE_CONSOLE, 36, METHOD_BUFFERED, FILE_WRITE_DATA)

/* console renderer ioctls */
#define IOCTL_CONDRV_GET_RENDERER_EVENTS   CTL_CODE(FILE_DEVICE_CONSOLE, 70, METHOD_BUFFERED, FILE_READ_PROPERTIES)
#define IOCTL_CONDRV_ATTACH_RENDERER       CTL_CODE(FILE_DEVICE_CONSOLE, 71, METHOD_BUFFERED, FILE_READ_PROPERTIES)

/* ioctls used for communication between driver and host */
#define IOCTL_CONDRV_INIT_OUTPUT           CTL_CODE(FILE_DEVICE_CONSOLE, 90, METHOD_BUFFERED, 0)
#define IOCTL_CONDRV_CLOSE_OUTPUT          CTL_CODE(FILE_DEVICE_CONSOLE, 91, METHOD_BUFFERED, 0)

/* console handle type */
typedef unsigned int condrv_handle_t;

/* convert an object handle to a server handle */
static inline condrv_handle_t condrv_handle( HANDLE handle )
{
    if ((int)(INT_PTR)handle != (INT_PTR)handle) return 0xfffffff0;  /* some invalid handle */
    return (INT_PTR)handle;
}

/* structure for console char/attribute info */
typedef struct
{
    WCHAR          ch;
    unsigned short attr;
} char_info_t;

/* IOCTL_CONDRV_GET_INPUT_INFO result */
struct condrv_input_info
{
    unsigned int  input_cp;       /* console input codepage */
    unsigned int  output_cp;      /* console output codepage */
    unsigned int  history_mode;   /* whether we duplicate lines in history */
    unsigned int  history_size;   /* number of lines in history */
    unsigned int  history_index;  /* number of used lines in history */
    unsigned int  edition_mode;   /* index to the edition mode flavors */
    unsigned int  input_count;    /* number of available input records */
    condrv_handle_t win;          /* renderer window handle */
};

/* IOCTL_CONDRV_SET_INPUT_INFO params */
struct condrv_input_info_params
{
    unsigned int  mask;               /* setting mask */
    struct condrv_input_info info;    /* input_info */
};

#define SET_CONSOLE_INPUT_INFO_EDITION_MODE     0x01
#define SET_CONSOLE_INPUT_INFO_INPUT_CODEPAGE   0x02
#define SET_CONSOLE_INPUT_INFO_OUTPUT_CODEPAGE  0x04
#define SET_CONSOLE_INPUT_INFO_WIN              0x08
#define SET_CONSOLE_INPUT_INFO_HISTORY_MODE     0x10
#define SET_CONSOLE_INPUT_INFO_HISTORY_SIZE     0x20

/* IOCTL_CONDRV_WRITE_OUTPUT and IOCTL_CONDRV_READ_OUTPUT params */
struct condrv_output_params
{
    unsigned int  x;                  /* destination position */
    unsigned int  y;
    unsigned int  mode;               /* char info mode */
    unsigned int  width;              /* width of output rectangle, 0 for wrapped mode */
    /* followed by an array of data with type depending on mode */
};

enum char_info_mode
{
    CHAR_INFO_MODE_TEXT,              /* characters only */
    CHAR_INFO_MODE_ATTR,              /* attributes only */
    CHAR_INFO_MODE_TEXTATTR,          /* both characters and attributes */
    CHAR_INFO_MODE_TEXTSTDATTR,       /* characters but use standard attributes */
};

/* IOCTL_CONDRV_GET_OUTPUT_INFO result */
struct condrv_output_info
{
    short int     cursor_size;        /* size of cursor (percentage filled) */
    short int     cursor_visible;     /* cursor visibility flag */
    short int     cursor_x;           /* position of cursor (x, y) */
    short int     cursor_y;
    short int     width;              /* width of the screen buffer */
    short int     height;             /* height of the screen buffer */
    short int     attr;               /* default fill attributes (screen colors) */
    short int     popup_attr;         /* pop-up color attributes */
    short int     win_left;           /* window actually displayed by renderer */
    short int     win_top;            /* the rect area is expressed within the */
    short int     win_right;          /* boundaries of the screen buffer */
    short int     win_bottom;
    short int     max_width;          /* maximum size (width x height) for the window */
    short int     max_height;
    short int     font_width;         /* font size (width x height) */
    short int     font_height;
    short int     font_weight;        /* font weight */
    short int     font_pitch_family;  /* font pitch & family */
    unsigned int  color_map[16];      /* color table */
};

/* IOCTL_CONDRV_SET_OUTPUT_INFO params */
struct condrv_output_info_params
{
    unsigned int  mask;               /* setting mask */
    struct condrv_output_info info;   /* output info */
};

#define SET_CONSOLE_OUTPUT_INFO_CURSOR_GEOM     0x0001
#define SET_CONSOLE_OUTPUT_INFO_CURSOR_POS      0x0002
#define SET_CONSOLE_OUTPUT_INFO_SIZE            0x0004
#define SET_CONSOLE_OUTPUT_INFO_ATTR            0x0008
#define SET_CONSOLE_OUTPUT_INFO_DISPLAY_WINDOW  0x0010
#define SET_CONSOLE_OUTPUT_INFO_MAX_SIZE        0x0020
#define SET_CONSOLE_OUTPUT_INFO_FONT            0x0040
#define SET_CONSOLE_OUTPUT_INFO_COLORTABLE      0x0080
#define SET_CONSOLE_OUTPUT_INFO_POPUP_ATTR      0x0100

/* IOCTL_CONDRV_FILL_OUTPUT params */
struct condrv_fill_output_params
{
    int            x;                 /* position where to start writing */
    int            y;
    int            mode;              /* char info mode */
    int            count;             /* number to write */
    int            wrap;              /* wrap around at end of line? */
    WCHAR          ch;                /* character to write */
    unsigned short attr;              /* attribute to write */
};

/* IOCTL_CONDRV_SCROLL params */
struct condrv_scroll_params
{
    SMALL_RECT   scroll;              /* source rectangle */
    COORD        origin;              /* destination coordinates */
    SMALL_RECT   clip;                /* clipping rectangle */
    char_info_t  fill;                /* empty character info */
};

/* IOCTL_CONDRV_GET_RENDERER_EVENTS result */
struct condrv_renderer_event
{
    short event;
    union
    {
        struct
        {
            short top;
            short bottom;
        } update;
        struct
        {
            short width;
            short height;
        } resize;
        struct
        {
            short x;
            short y;
        } cursor_pos;
        struct
        {
            short visible;
            short size;
        } cursor_geom;
        struct
        {
            short left;
            short top;
            short width;
            short height;
        } display;
    } u;
};

enum condrv_renderer_event_type
{
    CONSOLE_RENDERER_NONE_EVENT,
    CONSOLE_RENDERER_TITLE_EVENT,
    CONSOLE_RENDERER_SB_RESIZE_EVENT,
    CONSOLE_RENDERER_UPDATE_EVENT,
    CONSOLE_RENDERER_CURSOR_POS_EVENT,
    CONSOLE_RENDERER_CURSOR_GEOM_EVENT,
    CONSOLE_RENDERER_DISPLAY_EVENT,
    CONSOLE_RENDERER_EXIT_EVENT,
};

/* IOCTL_CONDRV_CTRL_EVENT params */
struct condrv_ctrl_event
{
    int          event;         /* the event to send */
    unsigned int group_id;      /* the group to send the event to */
};

/* Wine specific values for console inheritance (params->ConsoleHandle) */
#define CONSOLE_HANDLE_ALLOC  ((HANDLE)1)

#endif /* _INC_CONDRV */

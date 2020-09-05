/*
 * Copyright 1998 Alexandre Julliard
 * Copyright 2001 Eric Pouech
 * Copyright 2012 Detlef Riekenberg
 * Copyright 2020 Jacek Caban
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

#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <winnls.h>
#include <winternl.h>

#include "wine/condrv.h"
#include "wine/server.h"
#include "wine/rbtree.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(conhost);

struct history_line
{
    size_t len;
    WCHAR  text[1];
};

struct font_info
{
    short int width;
    short int height;
    short int weight;
    short int pitch_family;
    WCHAR    *face_name;
    size_t    face_len;
};

struct console
{
    HANDLE                server;              /* console server handle */
    unsigned int          mode;                /* input mode */
    struct screen_buffer *active;              /* active screen buffer */
    INPUT_RECORD         *records;             /* input records */
    unsigned int          record_count;        /* number of input records */
    unsigned int          record_size;         /* size of input records buffer */
    size_t                pending_read;        /* size of pending read buffer */
    WCHAR                *title;               /* console title */
    size_t                title_len;           /* length of console title */
    struct history_line **history;             /* lines history */
    unsigned int          history_size;        /* number of entries in history array */
    unsigned int          history_index;       /* number of used entries in history array */
    unsigned int          history_mode;        /* mode of history (non zero means remove doubled strings */
    unsigned int          edition_mode;        /* index to edition mode flavors */
    unsigned int          input_cp;            /* console input codepage */
    unsigned int          output_cp;           /* console output codepage */
    unsigned int          win;                 /* window handle if backend supports it */
    HANDLE                tty_output;          /* handle to tty output stream */
    char                  tty_buffer[4096];    /* tty output buffer */
    size_t                tty_buffer_count;    /* tty buffer size */
    unsigned int          tty_cursor_x;        /* tty cursor position */
    unsigned int          tty_cursor_y;
    unsigned int          tty_attr;            /* current tty char attributes */
    int                   tty_cursor_visible;  /* tty cursor visibility flag */
};

struct screen_buffer
{
    struct console       *console;       /* console reference */
    unsigned int          id;            /* screen buffer id */
    unsigned int          mode;          /* output mode */
    unsigned int          width;         /* size (w-h) of the screen buffer */
    unsigned int          height;
    unsigned int          cursor_size;   /* size of cursor (percentage filled) */
    unsigned int          cursor_visible;/* cursor visibility flag */
    unsigned int          cursor_x;      /* position of cursor */
    unsigned int          cursor_y;      /* position of cursor */
    unsigned short        attr;          /* default fill attributes (screen colors) */
    unsigned short        popup_attr;    /* pop-up color attributes */
    unsigned int          max_width;     /* size (w-h) of the window given font size */
    unsigned int          max_height;
    char_info_t          *data;          /* the data for each cell - a width x height matrix */
    unsigned int          color_map[16]; /* color table */
    RECT                  win;           /* current visible window on the screen buffer */
    struct font_info      font;          /* console font information */
    struct wine_rb_entry  entry;         /* map entry */
};

static const char_info_t empty_char_info = { ' ', 0x0007 };  /* white on black space */

static void *ioctl_buffer;
static size_t ioctl_buffer_size;

static void *alloc_ioctl_buffer( size_t size )
{
    if (size > ioctl_buffer_size)
    {
        void *new_buffer;
        if (!(new_buffer = realloc( ioctl_buffer, size ))) return NULL;
        ioctl_buffer = new_buffer;
        ioctl_buffer_size = size;
    }
    return ioctl_buffer;
}

static int screen_buffer_compare_id( const void *key, const struct wine_rb_entry *entry )
{
    struct screen_buffer *screen_buffer = WINE_RB_ENTRY_VALUE( entry, struct screen_buffer, entry );
    return PtrToLong(key) - screen_buffer->id;
}

static struct wine_rb_tree screen_buffer_map = { screen_buffer_compare_id };

static struct screen_buffer *create_screen_buffer( struct console *console, int id, int width, int height )
{
    struct screen_buffer *screen_buffer;
    unsigned int i;

    if (!(screen_buffer = malloc( sizeof(*screen_buffer) ))) return NULL;
    screen_buffer->console        = console;
    screen_buffer->id             = id;
    screen_buffer->mode           = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;
    screen_buffer->cursor_size    = 100;
    screen_buffer->cursor_visible = 1;
    screen_buffer->cursor_x       = 0;
    screen_buffer->cursor_y       = 0;
    screen_buffer->width          = width;
    screen_buffer->height         = height;
    screen_buffer->attr           = 0x07;
    screen_buffer->popup_attr     = 0xf5;
    screen_buffer->max_width      = 80;
    screen_buffer->max_height     = 25;
    screen_buffer->win.left       = 0;
    screen_buffer->win.right      = screen_buffer->max_width - 1;
    screen_buffer->win.top        = 0;
    screen_buffer->win.bottom     = screen_buffer->max_height - 1;
    screen_buffer->font.width     = 0;
    screen_buffer->font.height    = 0;
    screen_buffer->font.weight    = FW_NORMAL;
    screen_buffer->font.pitch_family = FIXED_PITCH | FF_DONTCARE;
    screen_buffer->font.face_name = NULL;
    screen_buffer->font.face_len  = 0;
    memset( screen_buffer->color_map, 0, sizeof(screen_buffer->color_map) );

    if (!(screen_buffer->data = malloc( screen_buffer->width * screen_buffer->height *
                                        sizeof(*screen_buffer->data) )))
    {
        free( screen_buffer );
        return NULL;
    }

    /* clear the first row */
    for (i = 0; i < screen_buffer->width; i++) screen_buffer->data[i] = empty_char_info;
    /* and copy it to all other rows */
    for (i = 1; i < screen_buffer->height; i++)
        memcpy( &screen_buffer->data[i * screen_buffer->width], screen_buffer->data,
                screen_buffer->width * sizeof(char_info_t) );

    if (wine_rb_put( &screen_buffer_map, LongToPtr(id), &screen_buffer->entry ))
    {
        ERR( "id %x already exists\n", id );
        return NULL;
    }

    return screen_buffer;
}

static void destroy_screen_buffer( struct screen_buffer *screen_buffer )
{
    if (screen_buffer->console->active == screen_buffer)
        screen_buffer->console->active = NULL;
    wine_rb_remove( &screen_buffer_map, &screen_buffer->entry );
    free( screen_buffer );
}

static BOOL is_active( struct screen_buffer *screen_buffer )
{
    return screen_buffer == screen_buffer->console->active;
}

static void tty_flush( struct console *console )
{
    if (!console->tty_output || !console->tty_buffer_count) return;
    TRACE("%s\n", debugstr_an(console->tty_buffer, console->tty_buffer_count));
    if (!WriteFile( console->tty_output, console->tty_buffer, console->tty_buffer_count,
                    NULL, NULL ))
        WARN( "write failed: %u\n", GetLastError() );
    console->tty_buffer_count = 0;
}

static void tty_write( struct console *console, const char *buffer, size_t size )
{
    if (!size || !console->tty_output) return;
    if (console->tty_buffer_count + size > sizeof(console->tty_buffer))
        tty_flush( console );
    if (console->tty_buffer_count + size <= sizeof(console->tty_buffer))
    {
        memcpy( console->tty_buffer + console->tty_buffer_count, buffer, size );
        console->tty_buffer_count += size;
    }
    else
    {
        assert( !console->tty_buffer_count );
        if (!WriteFile( console->tty_output, buffer, size, NULL, NULL ))
            WARN( "write failed: %u\n", GetLastError() );
    }
}

static void *tty_alloc_buffer( struct console *console, size_t size )
{
    void *ret;
    if (console->tty_buffer_count + size > sizeof(console->tty_buffer)) return NULL;
    ret = console->tty_buffer + console->tty_buffer_count;
    console->tty_buffer_count += size;
    return ret;
}

static void hide_tty_cursor( struct console *console )
{
    if (console->tty_cursor_visible)
    {
        tty_write(  console, "\x1b[25l", 5 );
        console->tty_cursor_visible = FALSE;
    }
}

static void set_tty_cursor( struct console *console, unsigned int x, unsigned int y )
{
    char buf[64];

    if (console->tty_cursor_x == x && console->tty_cursor_y == y) return;

    if (!x && y == console->tty_cursor_y + 1) strcpy( buf, "\r\n" );
    else if (!x && y == console->tty_cursor_y) strcpy( buf, "\r" );
    else if (y == console->tty_cursor_y)
    {
        if (x + 1 == console->tty_cursor_x) strcpy( buf, "\b" );
        else if (x > console->tty_cursor_x) sprintf( buf, "\x1b[%uC", x - console->tty_cursor_x );
        else sprintf( buf, "\x1b[%uD", console->tty_cursor_x - x );
    }
    else if (x || y)
    {
        hide_tty_cursor( console );
        sprintf( buf, "\x1b[%u;%uH", y + 1, x + 1);
    }
    else strcpy( buf, "\x1b[H" );
    console->tty_cursor_x = x;
    console->tty_cursor_y = y;
    tty_write( console, buf, strlen(buf) );
}

static void set_tty_attr( struct console *console, unsigned int attr )
{
    char buf[8];

    if ((attr & 0x0f) != (console->tty_attr & 0x0f))
    {
        if ((attr & 0x0f) != 7)
        {
            unsigned int n = 30;
            if (attr & FOREGROUND_BLUE)  n += 4;
            if (attr & FOREGROUND_GREEN) n += 2;
            if (attr & FOREGROUND_RED)   n += 1;
            if (attr & FOREGROUND_INTENSITY) n += 60;
            sprintf(buf, "\x1b[%um", n);
            tty_write( console, buf, strlen(buf) );
        }
        else tty_write( console, "\x1b[m", 3 );
    }

    if ((attr & 0xf0) != (console->tty_attr & 0xf0) && attr != 7)
    {
        unsigned int n = 40;
        if (attr & BACKGROUND_BLUE)  n += 4;
        if (attr & BACKGROUND_GREEN) n += 2;
        if (attr & BACKGROUND_RED)   n += 1;
        if (attr & BACKGROUND_INTENSITY) n += 60;
        sprintf(buf, "\x1b[%um", n);
        tty_write( console, buf, strlen(buf) );
    }

    console->tty_attr = attr;
}

static void tty_sync( struct console *console )
{
    if (!console->tty_output) return;

    if (console->active->cursor_visible)
    {
        set_tty_cursor( console, console->active->cursor_x, console->active->cursor_y );
        if (!console->tty_cursor_visible)
        {
            tty_write( console, "\x1b[?25h", 6 ); /* show cursor */
            console->tty_cursor_visible = TRUE;
        }
    }
    else if (console->tty_cursor_visible)
        hide_tty_cursor( console );
    tty_flush( console );
}

static void init_tty_output( struct console *console )
{
    /* initialize tty output, but don't flush */
    tty_write( console, "\x1b[2J", 4 ); /* clear screen */
    set_tty_attr( console, console->active->attr );
    tty_write( console, "\x1b[H", 3 );  /* move cursor to (0,0) */
    console->tty_cursor_visible = TRUE;
}

static void update_output( struct screen_buffer *screen_buffer, const RECT *rect )
{
    int x, y, size, trailing_spaces;
    char_info_t *ch;
    char buf[8];

    if (!is_active( screen_buffer ) || !screen_buffer->console->tty_output) return;
    TRACE( "%s\n", wine_dbgstr_rect( rect ));

    hide_tty_cursor( screen_buffer->console );

    for (y = rect->top; y <= rect->bottom; y++)
    {
        for (trailing_spaces = 0; trailing_spaces < screen_buffer->width; trailing_spaces++)
        {
            ch = &screen_buffer->data[(y + 1) * screen_buffer->width - trailing_spaces - 1];
            if (ch->ch != ' ' || ch->attr != 7) break;
        }
        if (trailing_spaces < 4) trailing_spaces = 0;

        for (x = rect->left; x <= rect->right; x++)
        {
            ch = &screen_buffer->data[y * screen_buffer->width + x];
            set_tty_attr( screen_buffer->console, ch->attr );
            set_tty_cursor( screen_buffer->console, x, y );

            if (x + trailing_spaces >= screen_buffer->width)
            {
                tty_write( screen_buffer->console, "\x1b[K", 3 );
                break;
            }

            size = WideCharToMultiByte( CP_UTF8, 0, &ch->ch, 1, buf, sizeof(buf), NULL, NULL );
            tty_write( screen_buffer->console, buf, size );
            screen_buffer->console->tty_cursor_x++;
        }
    }
}

static NTSTATUS read_console_input( struct console *console, size_t out_size )
{
    size_t count = min( out_size / sizeof(INPUT_RECORD), console->record_count );
    NTSTATUS status;

    TRACE("count %u\n", count);

    SERVER_START_REQ( get_next_console_request )
    {
        req->handle = wine_server_obj_handle( console->server );
        req->signal = count < console->record_count;
        req->read   = 1;
        req->status = STATUS_SUCCESS;
        wine_server_add_data( req, console->records, count * sizeof(*console->records) );
        status = wine_server_call( req );
    }
    SERVER_END_REQ;
    if (status)
    {
        ERR( "failed: %#x\n", status );
        return status;
    }

    if (count < console->record_count)
        memmove( console->records, console->records + count,
                 (console->record_count - count) * sizeof(*console->records) );
    console->record_count -= count;
    return STATUS_SUCCESS;
}

/* add input events to a console input queue */
static NTSTATUS write_console_input( struct console *console, const INPUT_RECORD *records,
                                     unsigned int count )
{
    TRACE( "%u\n", count );

    if (!count) return STATUS_SUCCESS;
    if (console->record_count + count > console->record_size)
    {
        INPUT_RECORD *new_rec;
        if (!(new_rec = realloc( console->records, (console->record_size * 2 + count) * sizeof(INPUT_RECORD) )))
            return STATUS_NO_MEMORY;
        console->records = new_rec;
        console->record_size = console->record_size * 2 + count;
    }
    memcpy( console->records + console->record_count, records, count * sizeof(INPUT_RECORD) );

    if (console->mode & ENABLE_PROCESSED_INPUT)
    {
        unsigned int i = 0;
        while (i < count)
        {
            if (records[i].EventType == KEY_EVENT &&
		records[i].Event.KeyEvent.uChar.UnicodeChar == 'C' - 64 &&
		!(records[i].Event.KeyEvent.dwControlKeyState & ENHANCED_KEY))
            {
                if (i != count - 1)
                    memcpy( &console->records[console->record_count + i],
                            &console->records[console->record_count + i + 1],
                            (count - i - 1) * sizeof(INPUT_RECORD) );
                count--;
                if (records[i].Event.KeyEvent.bKeyDown)
                {
                    struct condrv_ctrl_event ctrl_event;
                    IO_STATUS_BLOCK io;

                    ctrl_event.event = CTRL_C_EVENT;
                    ctrl_event.group_id = 0;
                    NtDeviceIoControlFile( console->server, NULL, NULL, NULL, &io, IOCTL_CONDRV_CTRL_EVENT,
                                           &ctrl_event, sizeof(ctrl_event), NULL, 0 );

                }
            }
            else i++;
        }
    }
    console->record_count += count;
    if (count && console->pending_read)
    {
        read_console_input( console, console->pending_read );
        console->pending_read = 0;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS screen_buffer_activate( struct screen_buffer *screen_buffer )
{
    RECT update_rect;
    TRACE( "%p\n", screen_buffer );
    screen_buffer->console->active = screen_buffer;
    SetRect( &update_rect, 0, 0, screen_buffer->width - 1, screen_buffer->height - 1);
    update_output( screen_buffer, &update_rect );
    tty_sync( screen_buffer->console );
    return STATUS_SUCCESS;
}

static NTSTATUS get_output_info( struct screen_buffer *screen_buffer, size_t *out_size )
{
    struct condrv_output_info *info;

    TRACE( "%p\n", screen_buffer );

    *out_size = min( *out_size, sizeof(*info) + screen_buffer->font.face_len );
    if (!(info = alloc_ioctl_buffer( *out_size ))) return STATUS_NO_MEMORY;

    info->cursor_size    = screen_buffer->cursor_size;
    info->cursor_visible = screen_buffer->cursor_visible;
    info->cursor_x       = screen_buffer->cursor_x;
    info->cursor_y       = screen_buffer->cursor_y;
    info->width          = screen_buffer->width;
    info->height         = screen_buffer->height;
    info->attr           = screen_buffer->attr;
    info->popup_attr     = screen_buffer->popup_attr;
    info->win_left       = screen_buffer->win.left;
    info->win_top        = screen_buffer->win.top;
    info->win_right      = screen_buffer->win.right;
    info->win_bottom     = screen_buffer->win.bottom;
    info->max_width      = screen_buffer->max_width;
    info->max_height     = screen_buffer->max_height;
    info->font_width     = screen_buffer->font.width;
    info->font_height    = screen_buffer->font.height;
    info->font_weight    = screen_buffer->font.weight;
    info->font_pitch_family = screen_buffer->font.pitch_family;
    memcpy( info->color_map, screen_buffer->color_map, sizeof(info->color_map) );
    if (*out_size > sizeof(*info)) memcpy( info + 1, screen_buffer->font.face_name, *out_size - sizeof(*info) );
    return STATUS_SUCCESS;
}

static NTSTATUS change_screen_buffer_size( struct screen_buffer *screen_buffer, int new_width, int new_height )
{
    int i, old_width, old_height, copy_width, copy_height;
    char_info_t *new_data;

    if (!(new_data = malloc( new_width * new_height * sizeof(*new_data) ))) return STATUS_NO_MEMORY;

    old_width = screen_buffer->width;
    old_height = screen_buffer->height;
    copy_width = min( old_width, new_width );
    copy_height = min( old_height, new_height );

    /* copy all the rows */
    for (i = 0; i < copy_height; i++)
    {
        memcpy( &new_data[i * new_width], &screen_buffer->data[i * old_width],
                copy_width * sizeof(char_info_t) );
    }

    /* clear the end of each row */
    if (new_width > old_width)
    {
        /* fill first row */
        for (i = old_width; i < new_width; i++) new_data[i] = empty_char_info;
        /* and blast it to the other rows */
        for (i = 1; i < copy_height; i++)
            memcpy( &new_data[i * new_width + old_width], &new_data[old_width],
                    (new_width - old_width) * sizeof(char_info_t) );
    }

    /* clear remaining rows */
    if (new_height > old_height)
    {
        /* fill first row */
        for (i = 0; i < new_width; i++) new_data[old_height * new_width + i] = empty_char_info;
        /* and blast it to the other rows */
        for (i = old_height+1; i < new_height; i++)
            memcpy( &new_data[i * new_width], &new_data[old_height * new_width],
                    new_width * sizeof(char_info_t) );
    }
    free( screen_buffer->data );
    screen_buffer->data   = new_data;
    screen_buffer->width  = new_width;
    screen_buffer->height = new_height;
    return STATUS_SUCCESS;
}

static NTSTATUS set_output_info( struct screen_buffer *screen_buffer,
                                 const struct condrv_output_info_params *params, size_t extra_size )
{
    const struct condrv_output_info *info = &params->info;
    WCHAR *font_name;
    NTSTATUS status;

    TRACE( "%p\n", screen_buffer );

    extra_size -= sizeof(*params);

    if (params->mask & SET_CONSOLE_OUTPUT_INFO_CURSOR_GEOM)
    {
        if (info->cursor_size < 1 || info->cursor_size > 100) return STATUS_INVALID_PARAMETER;

        screen_buffer->cursor_size    = info->cursor_size;
        screen_buffer->cursor_visible = !!info->cursor_visible;
    }
    if (params->mask & SET_CONSOLE_OUTPUT_INFO_CURSOR_POS)
    {
        if (info->cursor_x < 0 || info->cursor_x >= screen_buffer->width ||
            info->cursor_y < 0 || info->cursor_y >= screen_buffer->height)
        {
            return STATUS_INVALID_PARAMETER;
        }

        if (screen_buffer->cursor_x != info->cursor_x || screen_buffer->cursor_y != info->cursor_y)
        {
            screen_buffer->cursor_x = info->cursor_x;
            screen_buffer->cursor_y = info->cursor_y;
        }
    }
    if (params->mask & SET_CONSOLE_OUTPUT_INFO_SIZE)
    {
        /* new screen-buffer cannot be smaller than actual window */
        if (info->width < screen_buffer->win.right - screen_buffer->win.left + 1 ||
            info->height < screen_buffer->win.bottom - screen_buffer->win.top + 1)
        {
            return STATUS_INVALID_PARAMETER;
        }
        /* FIXME: there are also some basic minimum and max size to deal with */
        if ((status = change_screen_buffer_size( screen_buffer, info->width, info->height ))) return status;

        /* scroll window to display sb */
        if (screen_buffer->win.right >= info->width)
        {
            screen_buffer->win.right -= screen_buffer->win.left;
            screen_buffer->win.left = 0;
        }
        if (screen_buffer->win.bottom >= info->height)
        {
            screen_buffer->win.bottom -= screen_buffer->win.top;
            screen_buffer->win.top = 0;
        }
        if (screen_buffer->cursor_x >= info->width)  screen_buffer->cursor_x = info->width - 1;
        if (screen_buffer->cursor_y >= info->height) screen_buffer->cursor_y = info->height - 1;

        if (is_active( screen_buffer ) && screen_buffer->console->mode & ENABLE_WINDOW_INPUT)
        {
            INPUT_RECORD ir;
            ir.EventType = WINDOW_BUFFER_SIZE_EVENT;
            ir.Event.WindowBufferSizeEvent.dwSize.X = info->width;
            ir.Event.WindowBufferSizeEvent.dwSize.Y = info->height;
            write_console_input( screen_buffer->console, &ir, 1 );
        }
    }
    if (params->mask & SET_CONSOLE_OUTPUT_INFO_ATTR)
    {
        screen_buffer->attr = info->attr;
    }
    if (params->mask & SET_CONSOLE_OUTPUT_INFO_POPUP_ATTR)
    {
        screen_buffer->popup_attr = info->popup_attr;
    }
    if (params->mask & SET_CONSOLE_OUTPUT_INFO_DISPLAY_WINDOW)
    {
        if (info->win_left < 0 || info->win_left > info->win_right ||
            info->win_right >= screen_buffer->width ||
            info->win_top < 0  || info->win_top > info->win_bottom ||
            info->win_bottom >= screen_buffer->height)
        {
            return STATUS_INVALID_PARAMETER;
        }
        if (screen_buffer->win.left != info->win_left || screen_buffer->win.top != info->win_top ||
            screen_buffer->win.right != info->win_right || screen_buffer->win.bottom != info->win_bottom)
        {
            screen_buffer->win.left   = info->win_left;
            screen_buffer->win.top    = info->win_top;
            screen_buffer->win.right  = info->win_right;
            screen_buffer->win.bottom = info->win_bottom;
        }
    }
    if (params->mask & SET_CONSOLE_OUTPUT_INFO_MAX_SIZE)
    {
        screen_buffer->max_width  = info->max_width;
        screen_buffer->max_height = info->max_height;
    }
    if (params->mask & SET_CONSOLE_OUTPUT_INFO_COLORTABLE)
    {
        memcpy( screen_buffer->color_map, info->color_map, sizeof(screen_buffer->color_map) );
    }
    if (params->mask & SET_CONSOLE_OUTPUT_INFO_FONT)
    {
        screen_buffer->font.width  = info->font_width;
        screen_buffer->font.height = info->font_height;
        screen_buffer->font.weight = info->font_weight;
        screen_buffer->font.pitch_family = info->font_pitch_family;
        if (extra_size)
        {
            const WCHAR *params_font = (const WCHAR *)(params + 1);
            extra_size = extra_size / sizeof(WCHAR) * sizeof(WCHAR);
            font_name = malloc( extra_size );
            if (font_name)
            {
                memcpy( font_name, params_font, extra_size );
                free( screen_buffer->font.face_name );
                screen_buffer->font.face_name = font_name;
                screen_buffer->font.face_len  = extra_size;
            }
        }
    }

    if (is_active( screen_buffer )) tty_sync( screen_buffer->console );
    return STATUS_SUCCESS;
}

static NTSTATUS write_output( struct screen_buffer *screen_buffer, const struct condrv_output_params *params,
                              size_t in_size, size_t *out_size )
{
    unsigned int i, entry_size, entry_cnt, x, y;
    char_info_t *dest;
    char *src;

    entry_size = params->mode == CHAR_INFO_MODE_TEXTATTR ? sizeof(char_info_t) : sizeof(WCHAR);
    entry_cnt = (in_size - sizeof(*params)) / entry_size;

    TRACE( "(%u,%u) cnt %u\n", params->x, params->y, entry_cnt );

    if (params->x >= screen_buffer->width)
    {
        *out_size = 0;
        return STATUS_SUCCESS;
    }

    for (i = 0, src = (char *)(params + 1); i < entry_cnt; i++, src += entry_size)
    {
        if (params->width)
        {
            x = params->x + i % params->width;
            y = params->y + i / params->width;
            if (x >= screen_buffer->width) continue;
        }
        else
        {
            x = (params->x + i) % screen_buffer->width;
            y = params->y + (params->x + i) / screen_buffer->width;
        }
        if (y >= screen_buffer->height) break;

        dest = &screen_buffer->data[y * screen_buffer->width + x];
        switch(params->mode)
        {
        case CHAR_INFO_MODE_TEXT:
            dest->ch = *(const WCHAR *)src;
            break;
        case CHAR_INFO_MODE_ATTR:
            dest->attr = *(const unsigned short *)src;
            break;
        case CHAR_INFO_MODE_TEXTATTR:
            *dest = *(const char_info_t *)src;
            break;
        case CHAR_INFO_MODE_TEXTSTDATTR:
            dest->ch   = *(const WCHAR *)src;
            dest->attr = screen_buffer->attr;
            break;
        default:
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (i && is_active( screen_buffer ))
    {
        RECT update_rect;

        update_rect.left = params->x;
        update_rect.top  = params->y;
        if (params->width)
        {
            update_rect.bottom = min( params->y + entry_cnt / params->width, screen_buffer->height ) - 1;
            update_rect.right  = min( params->x + params->width, screen_buffer->width ) - 1;
        }
        else
        {
            update_rect.bottom = params->y + (params->x + i - 1) / screen_buffer->width;
            if (update_rect.bottom != params->y)
            {
                update_rect.left = 0;
                update_rect.right = screen_buffer->width - 1;
            }
            else
            {
                update_rect.right = params->x + i - 1;
            }
        }
        update_output( screen_buffer, &update_rect );
        tty_sync( screen_buffer->console );
    }

    if (*out_size == sizeof(SMALL_RECT))
    {
        SMALL_RECT *region;
        unsigned int width = params->width;
        x = params->x;
        y = params->y;
        if (!(region = alloc_ioctl_buffer( sizeof(*region )))) return STATUS_NO_MEMORY;
        region->Left   = x;
        region->Top    = y;
        region->Right  = min( x + width, screen_buffer->width ) - 1;
        region->Bottom = min( y + entry_cnt / width, screen_buffer->height ) - 1;
    }
    else
    {
        DWORD *result;
        if (!(result = alloc_ioctl_buffer( sizeof(*result )))) return STATUS_NO_MEMORY;
        *result = i;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS read_output( struct screen_buffer *screen_buffer, const struct condrv_output_params *params,
                             size_t *out_size )
{
    enum char_info_mode mode;
    unsigned int x, y, width;
    unsigned int i, count;

    x = params->x;
    y = params->y;
    mode  = params->mode;
    width = params->width;
    TRACE( "(%u %u) mode %u width %u\n", x, y, mode, width );

    switch(mode)
    {
    case CHAR_INFO_MODE_TEXT:
        {
            WCHAR *data;
            char_info_t *src;
            if (x >= screen_buffer->width || y >= screen_buffer->height)
            {
                *out_size = 0;
                return STATUS_SUCCESS;
            }
            src = screen_buffer->data + y * screen_buffer->width + x;
            count = min( screen_buffer->data + screen_buffer->height * screen_buffer->width - src,
                         *out_size / sizeof(*data) );
            *out_size = count * sizeof(*data);
            if (!(data = alloc_ioctl_buffer( *out_size ))) return STATUS_NO_MEMORY;
            for (i = 0; i < count; i++) data[i] = src[i].ch;
        }
        break;
    case CHAR_INFO_MODE_ATTR:
        {
            unsigned short *data;
            char_info_t *src;
            if (x >= screen_buffer->width || y >= screen_buffer->height)
            {
                *out_size = 0;
                return STATUS_SUCCESS;
            }
            src = screen_buffer->data + y * screen_buffer->width + x;
            count = min( screen_buffer->data + screen_buffer->height * screen_buffer->width - src,
                         *out_size / sizeof(*data) );
            *out_size = count * sizeof(*data);
            if (!(data = alloc_ioctl_buffer( *out_size ))) return STATUS_NO_MEMORY;
            for (i = 0; i < count; i++) data[i] = src[i].attr;
        }
        break;
    case CHAR_INFO_MODE_TEXTATTR:
        {
            SMALL_RECT *region;
            char_info_t *data;
            if (!width || *out_size < sizeof(*region) || x >= screen_buffer->width || y >= screen_buffer->height)
                return STATUS_INVALID_PARAMETER;
            count = min( (*out_size - sizeof(*region)) / (width * sizeof(*data)), screen_buffer->height - y );
            width = min( width, screen_buffer->width - x );
            *out_size = sizeof(*region) + width * count * sizeof(*data);
            if (!(region = alloc_ioctl_buffer( *out_size ))) return STATUS_NO_MEMORY;
            region->Left   = x;
            region->Top    = y;
            region->Right  = x + width - 1;
            region->Bottom = y + count - 1;
            data = (char_info_t *)(region + 1);
            for (i = 0; i < count; i++)
            {
                memcpy( &data[i * width], &screen_buffer->data[(y + i) * screen_buffer->width + x],
                        width * sizeof(*data) );
            }
        }
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS fill_output( struct screen_buffer *screen_buffer, const struct condrv_fill_output_params *params )
{
    char_info_t *end, *dest;
    DWORD i, count, *result;

    TRACE( "(%u %u) mode %u\n", params->x, params->y, params->mode );

    dest = screen_buffer->data + params->y * screen_buffer->width + params->x;

    if (params->y >= screen_buffer->height) return STATUS_SUCCESS;

    if (params->wrap)
        end = screen_buffer->data + screen_buffer->height * screen_buffer->width;
    else
        end = screen_buffer->data + (params->y + 1) * screen_buffer->width;

    count = params->count;
    if (count > end - dest) count = end - dest;

    switch(params->mode)
    {
    case CHAR_INFO_MODE_TEXT:
        for (i = 0; i < count; i++) dest[i].ch = params->ch;
        break;
    case CHAR_INFO_MODE_ATTR:
        for (i = 0; i < count; i++) dest[i].attr = params->attr;
        break;
    case CHAR_INFO_MODE_TEXTATTR:
        for (i = 0; i < count; i++)
        {
            dest[i].ch   = params->ch;
            dest[i].attr = params->attr;
        }
        break;
    case CHAR_INFO_MODE_TEXTSTDATTR:
        for (i = 0; i < count; i++)
        {
            dest[i].ch   = params->ch;
            dest[i].attr = screen_buffer->attr;
        }
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    if (count && is_active(screen_buffer))
    {
        RECT update_rect;
        SetRect( &update_rect,
                 params->x % screen_buffer->width,
                 params->y + params->x / screen_buffer->width,
                 (params->x + i - 1) % screen_buffer->width,
                 params->y + (params->x + i - 1) / screen_buffer->width );
        update_output( screen_buffer, &update_rect );
        tty_sync( screen_buffer->console );
    }

    if (!(result = alloc_ioctl_buffer( sizeof(*result) ))) return STATUS_NO_MEMORY;
    *result = count;
    return STATUS_SUCCESS;
}

static NTSTATUS scroll_output( struct screen_buffer *screen_buffer, const struct condrv_scroll_params *params )
{
    int x, y, xsrc, ysrc, w, h;
    char_info_t *psrc, *pdst;
    SMALL_RECT src, dst;
    RECT update_rect;
    SMALL_RECT clip;

    xsrc = params->scroll.Left;
    ysrc = params->scroll.Top;
    w = params->scroll.Right - params->scroll.Left + 1;
    h = params->scroll.Bottom - params->scroll.Top + 1;

    TRACE( "(%d %d) -> (%u %u) w %u h %u\n", xsrc, ysrc, params->origin.X, params->origin.Y, w, h );

    clip.Left   = max( params->clip.Left, 0 );
    clip.Top    = max( params->clip.Top,  0 );
    clip.Right  = min( params->clip.Right,  screen_buffer->width - 1 );
    clip.Bottom = min( params->clip.Bottom, screen_buffer->height - 1 );
    if (clip.Left > clip.Right || clip.Top > clip.Bottom || params->scroll.Left < 0 || params->scroll.Top < 0 ||
        params->scroll.Right >= screen_buffer->width || params->scroll.Bottom >= screen_buffer->height ||
        params->scroll.Right < params->scroll.Left || params->scroll.Top > params->scroll.Bottom ||
        params->origin.X < 0 || params->origin.X >= screen_buffer->width || params->origin.Y < 0 ||
        params->origin.Y >= screen_buffer->height)
        return STATUS_INVALID_PARAMETER;

    src.Left   = max( xsrc, clip.Left );
    src.Top    = max( ysrc, clip.Top );
    src.Right  = min( xsrc + w - 1, clip.Right );
    src.Bottom = min( ysrc + h - 1, clip.Bottom );

    dst.Left   = params->origin.X;
    dst.Top    = params->origin.Y;
    dst.Right  = params->origin.X + w - 1;
    dst.Bottom = params->origin.Y + h - 1;

    if (dst.Left < clip.Left)
    {
        xsrc += clip.Left - dst.Left;
        w -= clip.Left - dst.Left;
        dst.Left = clip.Left;
    }
    if (dst.Top < clip.Top)
    {
        ysrc += clip.Top - dst.Top;
        h -= clip.Top - dst.Top;
        dst.Top = clip.Top;
    }
    if (dst.Right  > clip.Right)  w -= dst.Right  - clip.Right;
    if (dst.Bottom > clip.Bottom) h -= dst.Bottom - clip.Bottom;

    if (w > 0 && h > 0)
    {
        if (ysrc < dst.Top)
        {
            psrc = &screen_buffer->data[(ysrc + h - 1) * screen_buffer->width + xsrc];
            pdst = &screen_buffer->data[(dst.Top + h - 1) * screen_buffer->width + dst.Left];

            for (y = h; y > 0; y--)
            {
                memcpy( pdst, psrc, w * sizeof(*pdst) );
                pdst -= screen_buffer->width;
                psrc -= screen_buffer->width;
            }
        }
        else
        {
            psrc = &screen_buffer->data[ysrc * screen_buffer->width + xsrc];
            pdst = &screen_buffer->data[dst.Top * screen_buffer->width + dst.Left];

            for (y = 0; y < h; y++)
            {
                /* we use memmove here because when psrc and pdst are the same,
                 * copies are done on the same row, so the dst and src blocks
                 * can overlap */
                memmove( pdst, psrc, w * sizeof(*pdst) );
                pdst += screen_buffer->width;
                psrc += screen_buffer->width;
            }
        }
    }

    for (y = src.Top; y <= src.Bottom; y++)
    {
        int left  = src.Left;
        int right = src.Right;
        if (dst.Top <= y && y <= dst.Bottom)
        {
            if (dst.Left <= src.Left) left  = max( left, dst.Right + 1 );
            if (dst.Left >= src.Left) right = min( right, dst.Left - 1 );
        }
        for (x = left; x <= right; x++) screen_buffer->data[y * screen_buffer->width + x] = params->fill;
    }

    SetRect( &update_rect, min( src.Left, dst.Left ), min( src.Top, dst.Top ),
             max( src.Right, dst.Right ), max( src.Bottom, dst.Bottom ));
    update_output( screen_buffer, &update_rect );
    tty_sync( screen_buffer->console );
    return STATUS_SUCCESS;
}

static NTSTATUS set_console_title( struct console *console, const WCHAR *in_title, size_t size )
{
    WCHAR *title = NULL;

    TRACE( "%s\n", debugstr_wn(in_title, size / sizeof(WCHAR)) );

    if (size)
    {
        if (!(title = malloc( size ))) return STATUS_NO_MEMORY;
        memcpy( title, in_title, size );
    }
    free( console->title );
    console->title     = title;
    console->title_len = size;

    if (console->tty_output)
    {
        size_t len;
        char *vt;

        tty_write( console, "\x1b]0;", 4 );
        len = WideCharToMultiByte( CP_UTF8, 0, console->title, size / sizeof(WCHAR), NULL, 0, NULL, NULL);
        if ((vt = tty_alloc_buffer( console, len )))
            WideCharToMultiByte( CP_UTF8, 0, console->title, size / sizeof(WCHAR), vt, len, NULL, NULL );
        tty_write( console, "\x07", 1 );
        tty_sync( console );
    }
    return STATUS_SUCCESS;
}

static NTSTATUS screen_buffer_ioctl( struct screen_buffer *screen_buffer, unsigned int code,
                                     const void *in_data, size_t in_size, size_t *out_size )
{
    switch (code)
    {
    case IOCTL_CONDRV_CLOSE_OUTPUT:
        if (in_size || *out_size) return STATUS_INVALID_PARAMETER;
        destroy_screen_buffer( screen_buffer );
        return STATUS_SUCCESS;

    case IOCTL_CONDRV_ACTIVATE:
        if (in_size || *out_size) return STATUS_INVALID_PARAMETER;
        return screen_buffer_activate( screen_buffer );

    case IOCTL_CONDRV_GET_MODE:
        {
            DWORD *mode;
            TRACE( "returning mode %x\n", screen_buffer->mode );
            if (in_size || *out_size != sizeof(*mode)) return STATUS_INVALID_PARAMETER;
            if (!(mode = alloc_ioctl_buffer( *out_size ))) return STATUS_NO_MEMORY;
            *mode = screen_buffer->mode;
            return STATUS_SUCCESS;
        }

    case IOCTL_CONDRV_SET_MODE:
        if (in_size != sizeof(unsigned int) || *out_size) return STATUS_INVALID_PARAMETER;
        screen_buffer->mode = *(unsigned int *)in_data;
        TRACE( "set %x mode\n", screen_buffer->mode );
        return STATUS_SUCCESS;

    case IOCTL_CONDRV_WRITE_OUTPUT:
        if ((*out_size != sizeof(DWORD) && *out_size != sizeof(SMALL_RECT)) ||
            in_size < sizeof(struct condrv_output_params))
            return STATUS_INVALID_PARAMETER;
        return write_output( screen_buffer, in_data, in_size, out_size );

    case IOCTL_CONDRV_READ_OUTPUT:
        if (in_size != sizeof(struct condrv_output_params)) return STATUS_INVALID_PARAMETER;
        return read_output( screen_buffer, in_data, out_size );

    case IOCTL_CONDRV_GET_OUTPUT_INFO:
        if (in_size || *out_size < sizeof(struct condrv_output_info)) return STATUS_INVALID_PARAMETER;
        return get_output_info( screen_buffer, out_size );

    case IOCTL_CONDRV_SET_OUTPUT_INFO:
        if (in_size < sizeof(struct condrv_output_info) || *out_size) return STATUS_INVALID_PARAMETER;
        return set_output_info( screen_buffer, in_data, in_size );

    case IOCTL_CONDRV_FILL_OUTPUT:
        if (in_size != sizeof(struct condrv_fill_output_params) || *out_size != sizeof(DWORD))
            return STATUS_INVALID_PARAMETER;
        return fill_output( screen_buffer, in_data );

    case IOCTL_CONDRV_SCROLL:
        if (in_size != sizeof(struct condrv_scroll_params) || *out_size)
            return STATUS_INVALID_PARAMETER;
        return scroll_output( screen_buffer, in_data );

    default:
        FIXME( "unsupported ioctl %x\n", code );
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS console_input_ioctl( struct console *console, unsigned int code, const void *in_data,
                                     size_t in_size, size_t *out_size )
{
    switch (code)
    {
    case IOCTL_CONDRV_GET_MODE:
        {
            DWORD *mode;
            TRACE( "returning mode %x\n", console->mode );
            if (in_size || *out_size != sizeof(*mode)) return STATUS_INVALID_PARAMETER;
            if (!(mode = alloc_ioctl_buffer( *out_size ))) return STATUS_NO_MEMORY;
            *mode = console->mode;
            return STATUS_SUCCESS;
        }

    case IOCTL_CONDRV_SET_MODE:
        if (in_size != sizeof(unsigned int) || *out_size) return STATUS_INVALID_PARAMETER;
        console->mode = *(unsigned int *)in_data;
        TRACE( "set %x mode\n", console->mode );
        return STATUS_SUCCESS;

    case IOCTL_CONDRV_READ_INPUT:
        {
            unsigned int blocking;
            NTSTATUS status;
            if (in_size && in_size != sizeof(blocking)) return STATUS_INVALID_PARAMETER;
            blocking = in_size && *(unsigned int *)in_data;
            if (blocking && !console->record_count && *out_size)
            {
                TRACE( "pending read" );
                console->pending_read = *out_size;
                return STATUS_PENDING;
            }
            status = read_console_input( console, *out_size );
            *out_size = 0;
            return status;
        }

    case IOCTL_CONDRV_WRITE_INPUT:
        if (in_size % sizeof(INPUT_RECORD) || *out_size) return STATUS_INVALID_PARAMETER;
        return write_console_input( console, in_data, in_size / sizeof(INPUT_RECORD) );

    case IOCTL_CONDRV_PEEK:
        {
            void *result;
            TRACE( "peek\n ");
            if (in_size) return STATUS_INVALID_PARAMETER;
            *out_size = min( *out_size, console->record_count * sizeof(INPUT_RECORD) );
            if (!(result = alloc_ioctl_buffer( *out_size ))) return STATUS_NO_MEMORY;
            if (*out_size) memcpy( result, console->records, *out_size );
            return STATUS_SUCCESS;
        }

    case IOCTL_CONDRV_GET_INPUT_INFO:
        {
            struct condrv_input_info *info;
            TRACE( "get info\n" );
            if (in_size || *out_size != sizeof(*info)) return STATUS_INVALID_PARAMETER;
            if (!(info = alloc_ioctl_buffer( sizeof(*info )))) return STATUS_NO_MEMORY;
            info->history_mode  = console->history_mode;
            info->history_size  = console->history_size;
            info->history_index = console->history_index;
            info->edition_mode  = console->edition_mode;
            info->input_cp      = console->input_cp;
            info->output_cp     = console->output_cp;
            info->win           = console->win;
            info->input_count   = console->record_count;
            return STATUS_SUCCESS;
        }

    case IOCTL_CONDRV_SET_INPUT_INFO:
        {
            const struct condrv_input_info_params *params = in_data;
            TRACE( "set info\n" );
            if (in_size != sizeof(*params) || *out_size) return STATUS_INVALID_PARAMETER;
            if (params->mask & SET_CONSOLE_INPUT_INFO_HISTORY_MODE)
            {
                console->history_mode = params->info.history_mode;
            }
            if ((params->mask & SET_CONSOLE_INPUT_INFO_HISTORY_SIZE) &&
                console->history_size != params->info.history_size)
            {
                struct history_line **mem = NULL;
                int i, delta;

                if (params->info.history_size)
                {
                    if (!(mem = malloc( params->info.history_size * sizeof(*mem) )))
                        return STATUS_NO_MEMORY;
                    memset( mem, 0, params->info.history_size * sizeof(*mem) );
                }

                delta = (console->history_index > params->info.history_size)
                    ? (console->history_index - params->info.history_size) : 0;

                for (i = delta; i < console->history_index; i++)
                {
                    mem[i - delta] = console->history[i];
                    console->history[i] = NULL;
                }
                console->history_index -= delta;

                for (i = 0; i < console->history_size; i++)
                    free( console->history[i] );
                free( console->history );
                console->history = mem;
                console->history_size = params->info.history_size;
            }
            if (params->mask & SET_CONSOLE_INPUT_INFO_EDITION_MODE)
            {
                console->edition_mode = params->info.edition_mode;
            }
            if (params->mask & SET_CONSOLE_INPUT_INFO_INPUT_CODEPAGE)
            {
                console->input_cp = params->info.input_cp;
            }
            if (params->mask & SET_CONSOLE_INPUT_INFO_OUTPUT_CODEPAGE)
            {
                console->output_cp = params->info.output_cp;
            }
            if (params->mask & SET_CONSOLE_INPUT_INFO_WIN)
            {
                console->win = params->info.win;
            }
            return STATUS_SUCCESS;
        }

    case IOCTL_CONDRV_GET_TITLE:
        {
            WCHAR *result;
            if (in_size) return STATUS_INVALID_PARAMETER;
            TRACE( "returning title %s\n", debugstr_wn(console->title,
                                                       console->title_len / sizeof(WCHAR)) );
            if (!(result = alloc_ioctl_buffer( *out_size = min( *out_size, console->title_len ))))
                return STATUS_NO_MEMORY;
            if (*out_size) memcpy( result, console->title, *out_size );
            return STATUS_SUCCESS;
        }

    case IOCTL_CONDRV_SET_TITLE:
        if (in_size % sizeof(WCHAR) || *out_size) return STATUS_INVALID_PARAMETER;
        return set_console_title( console, in_data, in_size );

    default:
        FIXME( "unsupported ioctl %x\n", code );
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS process_console_ioctls( struct console *console )
{
    size_t out_size = 0, in_size;
    unsigned int code;
    int output;
    NTSTATUS status = STATUS_SUCCESS;

    for (;;)
    {
        if (status) out_size = 0;

        SERVER_START_REQ( get_next_console_request )
        {
            req->handle = wine_server_obj_handle( console->server );
            req->status = status;
            req->signal = console->record_count != 0;
            wine_server_add_data( req, ioctl_buffer, out_size );
            wine_server_set_reply( req, ioctl_buffer, ioctl_buffer_size );
            status = wine_server_call( req );
            code     = reply->code;
            output   = reply->output;
            out_size = reply->out_size;
            in_size  = wine_server_reply_size( reply );
        }
        SERVER_END_REQ;

        if (status == STATUS_PENDING) return STATUS_SUCCESS;
        if (status == STATUS_BUFFER_OVERFLOW)
        {
            if (!alloc_ioctl_buffer( out_size )) return STATUS_NO_MEMORY;
            status = STATUS_SUCCESS;
            continue;
        }
        if (status)
        {
            TRACE( "failed to get next request: %#x\n", status );
            return status;
        }

        if (code == IOCTL_CONDRV_INIT_OUTPUT)
        {
            TRACE( "initializing output %x\n", output );
            if (console->active)
                create_screen_buffer( console, output, console->active->width, console->active->height );
            else
                create_screen_buffer( console, output, 80, 150 );
        }
        else if (!output)
        {
            status = console_input_ioctl( console, code, ioctl_buffer, in_size, &out_size );
        }
        else
        {
            struct wine_rb_entry *entry;
            if (!(entry = wine_rb_get( &screen_buffer_map, LongToPtr(output) )))
            {
                ERR( "invalid screen buffer id %x\n", output );
                status = STATUS_INVALID_HANDLE;
            }
            else
            {
                status = screen_buffer_ioctl( WINE_RB_ENTRY_VALUE( entry, struct screen_buffer, entry ), code,
                                              ioctl_buffer, in_size, &out_size );
            }
        }
    }
}

static int main_loop( struct console *console, HANDLE signal )
{
    HANDLE signal_event = NULL;
    HANDLE wait_handles[2];
    unsigned int wait_cnt = 0;
    unsigned short signal_id;
    IO_STATUS_BLOCK signal_io;
    NTSTATUS status;
    DWORD res;

    if (signal)
    {
        if (!(signal_event = CreateEventW( NULL, TRUE, FALSE, NULL ))) return 1;
        status = NtReadFile( signal, signal_event, NULL, NULL, &signal_io, &signal_id,
                             sizeof(signal_id), NULL, NULL );
        if (status && status != STATUS_PENDING) return 1;
    }

    if (!alloc_ioctl_buffer( 4096 )) return 1;

    wait_handles[wait_cnt++] = console->server;
    if (signal) wait_handles[wait_cnt++] = signal_event;

    for (;;)
    {
        res = WaitForMultipleObjects( wait_cnt, wait_handles, FALSE, INFINITE );

        switch (res)
        {
        case WAIT_OBJECT_0:
            if (process_console_ioctls( console )) return 0;
            break;

        case WAIT_OBJECT_0 + 1:
            if (signal_io.Status || signal_io.Information != sizeof(signal_id))
            {
                TRACE( "signaled quit\n" );
                return 0;
            }
            FIXME( "unimplemented signal %x\n", signal_id );
            status = NtReadFile( signal, signal_event, NULL, NULL, &signal_io, &signal_id,
                                 sizeof(signal_id), NULL, NULL );
            if (status && status != STATUS_PENDING) return 1;
            break;

        default:
            TRACE( "wait failed, quit\n");
            return 0;
        }
    }

    return 0;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    int headless = 0, i, width = 80, height = 150;
    HANDLE signal = NULL;
    WCHAR *end;

    static struct console console;

    for (i = 0; i < argc; i++) TRACE("%s ", wine_dbgstr_w(argv[i]));
    TRACE("\n");

    console.mode = ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT |
                   ENABLE_ECHO_INPUT | ENABLE_MOUSE_INPUT | ENABLE_INSERT_MODE |
                   ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS | ENABLE_AUTO_POSITION;
    console.input_cp = console.output_cp = GetOEMCP();
    console.history_size = 50;
    if (!(console.history = calloc( console.history_size, sizeof(*console.history) ))) return 1;

    for (i = 1; i < argc; i++)
    {
        if (!wcscmp( argv[i], L"--headless"))
        {
            headless = 1;
            continue;
        }
        if (!wcscmp( argv[i], L"--width" ))
        {
            if (++i == argc) return 1;
            width = wcstol( argv[i], &end, 0 );
            if (!width || width > 0xffff || *end) return 1;
            continue;
        }
        if (!wcscmp( argv[i], L"--height" ))
        {
            if (++i == argc) return 1;
            height = wcstol( argv[i], &end, 0 );
            if (!height || height > 0xffff || *end) return 1;
            continue;
        }
        if (!wcscmp( argv[i], L"--signal" ))
        {
            if (++i == argc) return 1;
            signal = ULongToHandle( wcstol( argv[i], &end, 0 ));
            if (*end) return 1;
            continue;
        }
        if (!wcscmp( argv[i], L"--server" ))
        {
            if (++i == argc) return 1;
            console.server = ULongToHandle( wcstol( argv[i], &end, 0 ));
            if (*end) return 1;
            continue;
        }
        FIXME( "unknown option %s\n", debugstr_w(argv[i]) );
        return 1;
    }

    if (!headless)
    {
        FIXME( "windowed mode not supported\n" );
        return 0;
    }

    if (!console.server)
    {
        ERR( "no server handle\n" );
        return 1;
    }

    if (!(console.active = create_screen_buffer( &console, 1, width, height ))) return 1;
    if (headless)
    {
        console.tty_output = GetStdHandle( STD_OUTPUT_HANDLE );
        init_tty_output( &console );
    }

    return main_loop( &console, signal );
}

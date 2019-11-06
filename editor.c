#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <time.h>

/* DEFINES */
#define PICO_VERSION "0.0.1"
#define PICO_TAB_STOP 8
#define Q_TIMES_TO_QUIT 3

#define CTRL_KEY(k) ((k)&0x1f)

enum
{
    BACKSPACE = 127,
    ARROW_UP = 1000,
    ARROW_DOWN,
    ARROW_LEFT,
    ARROW_RIGHT,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/* PROTOTYPES */
void editor_set_status_message(const char *s, ...);
void editor_refresh_screen();
char *editor_prompt(char *prompt, void (*callback) (char *, int));

/* DATA */

typedef struct erow
{
    int size;
    int rsize;
    char *data;
    char *render;
} erow;

struct editor_config
{
    int cx, cy;
    int ry;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    int dirty;
    erow *rows;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios original_set;
};

struct editor_config E;

/* TERMINAL */

void die(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disable_raw_mode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_set) == -1)
        die("tcsetattr");
}

void enable_raw_mode()
{
    if (tcgetattr(STDIN_FILENO, &E.original_set) == -1)
        die("tcgetattr");
    atexit(disable_raw_mode);

    struct termios raw = E.original_set;
    raw.c_iflag &= ~(IXON | ICRNL | ISTRIP | BRKINT | INPCK); // (Carriage Return New Line)
    raw.c_oflag &= ~(OPOST);                                  // disable '\r\n' on each of new lines
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cflag |= CS8;

    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 10;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editor_read_key()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
        if (nread == -1 && errno != EAGAIN)
            die("read");

    if (c == '\x1b')
    {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9') // \x1b[5~ - page down/up
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {

                    case '1':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            }

            switch (seq[1]) // \x1b[X  X belongs {'A', 'B', 'C', 'D'}
            {
            case 'A':
                return ARROW_UP;
            case 'B':
                return ARROW_DOWN;
            case 'C':
                return ARROW_RIGHT;
            case 'D':
                return ARROW_LEFT;
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }
        return '\x1b';
    }
    return c;
}

int get_cursor_position(int *c, int *r)
{
    char buf[32];
    int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) == -1)
        return -1;
    printf("\r\n");

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", c, r) != 2)
        return -1;

    return 0;
}

int get_window_size(int *r, int *c)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;

        return get_cursor_position(&E.screencols, &E.screenrows);
    }
    else
    {
        *c = ws.ws_col;
        *r = ws.ws_row;
        return 0;
    }
}

/* ROW OPERATIONS */
int editor_row_cy_to_ry(erow *row, int cy)
{
    int ry = 0;
    int j;
    for (j = 0; j < cy; j++)
    {
        if (row->data[j] == '\t')
            ry += (PICO_TAB_STOP - 1) - (ry % PICO_TAB_STOP);
        ry++;
    }
    return ry;
}

int editor_row_ry_to_cy(erow *row, int ry)
{
    int cur_ry = 0;
    int cy;

    for (cy = 0;  cy < row -> size; cy ++)
    {
        if (row -> data[cy] == '\t')
        {
            cur_ry += (PICO_TAB_STOP - 1) - (cur_ry % PICO_TAB_STOP);
        }  
        cur_ry++;

        if (cur_ry > ry) return cy;
    }
    return cy;
}

void editor_update_row(erow *row)
{
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
        if (row->data[j] == '\t')
            tabs++;

    if (row->render)
        free(row->render);

    row->render = malloc(row->size + tabs * (PICO_TAB_STOP - 1) + 1);
    int idx = 0;

    for (j = 0; j < row->size; j++)
    {
        if (row->data[j] == '\t')
        {
            row->render[idx++] = ' ';
            while (idx % PICO_TAB_STOP != 0)
                row->render[idx++] = ' ';
        }
        else
        {
            row->render[idx++] = row->data[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editor_insert_row(int at, char *s, size_t len)
{
    if (at < 0 || at > E.numrows)
        return;
    E.rows = realloc(E.rows, sizeof(erow) * (E.numrows + 1));
    memmove(E.rows + at + 1, E.rows + at, sizeof(erow) * (E.numrows - at));

    E.rows[at].size = len;
    E.rows[at].data = malloc(len + 1);
    memcpy(E.rows[at].data, s, len);
    E.rows[at].data[len] = '\0';

    E.rows[at].rsize = 0;
    E.rows[at].render = NULL;
    editor_update_row(E.rows + at);

    E.numrows++;
    E.dirty++;
}

void editor_free_row(erow *row)
{
    free(row->data);
    free(row->render);
}

void editor_delete_row(int at)
{
    if (at < 0 || at >= E.numrows)
        return;
    editor_free_row(E.rows + at);
    memmove(E.rows + at, E.rows + at + 1, sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editor_row_insert_char(erow *row, int at, int c)
{
    if (at < 0 || at > row->size)
        at = row->size;
    row->data = realloc(row->data, row->size + 2);
    memmove(row->data + at + 1, row->data + at, row->size - at + 1);
    row->size++;
    row->data[at] = c;
    editor_update_row(row);
    E.dirty++;
}

void editor_row_delete_char(erow *row, int at)
{
    if (at < 0 || at > row->size)
        return;

    memmove(row->data + at, row->data + at + 1, row->size - at);
    row->size--;
    editor_update_row(row);
    E.dirty++;
}

void editor_row_append_string(erow *row, char *s, size_t len)
{
    row->data = realloc(row->data, row->size + len + 1);
    memcpy(row->data + row->size, s, len);
    row->size = row->size + len;
    row->data[row->size] = '\0';
    editor_update_row(row);
    E.dirty++;
}

/* EDITOR OPERATIONS */
void editor_insert_char(int c)
{
    if (E.cx == E.numrows)
        editor_insert_row(E.cx, "", 0);
    editor_row_insert_char(E.rows + E.cx, E.cy, c);
    E.cy++;
}

void editor_delete_char()
{
    if (E.cx == E.numrows)
        return;
    if (E.cy == 0 && E.cx == 0)
        return;

    erow *row = E.rows + E.cx;
    if (E.cy > 0)
    {
        editor_row_delete_char(row, E.cy - 1);
        E.cy--;
    }
    else
    {
        E.cy = (E.rows + E.cx - 1)->size;
        editor_row_append_string(E.rows + E.cx - 1, row->data, row->size);
        editor_delete_row(E.cx);
        E.cx--;
        E.dirty++;
    }
}

void editor_insert_new_line()
{
    if (E.cy == 0)
    {
        editor_insert_row(E.cx, "", 0);
    }
    else
    {
        erow *row = E.rows + E.cx;
        editor_insert_row(E.cx + 1, row->data + E.cy, row->size - E.cy);
        row = E.rows + E.cx;
        row->size = E.cy;
        row->data = realloc(row->data, E.cy + 1);
        row->data[E.cy] = '\0';
        editor_update_row(row);
        E.dirty++;
    }
    E.cx++;
    E.cy = 0;
}

/* FILE i/o */

char *editor_rows_to_string(int *buflen)
{
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++)
        totlen += E.rows[j].size + 1;
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.numrows; j++)
    {
        memcpy(p, E.rows[j].data, E.rows[j].size);
        p += E.rows[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editor_open(const char *filename)
{
    if (E.filename)
        free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    char *line = NULL;
    ssize_t linecap;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1)
    {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editor_insert_row(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editor_save()
{
    if (E.filename == NULL)
    {
        E.filename = editor_prompt("Save file as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL)
        {
            editor_set_status_message("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editor_rows_to_string(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1)
    {
        if (ftruncate(fd, len) != -1)
        {
            if (write(fd, buf, len) == len)
            {
                close(fd);
                free(buf);
                E.dirty = 0;
                editor_set_status_message("%d bytes written to the disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editor_set_status_message("cant save! i/o error: ", strerror(errno));
}

/* FIND */

void editor_find_callback(char *query, int key)
{
    if (key == '\x1b' || key == '\r')
        return;
    int i;
    for (i = E.cx + 1; i < E.numrows; i++)
    {
        erow *row = E.rows + i;
        char *match = strstr(row->render, query);
        if (match)
        {
            E.cx = i;
            E.cy = editor_row_ry_to_cy(row, match - row -> render);
            E.rowoff = E.numrows;
            break;
        }
    }
}

void editor_find()
{
    char *query = editor_prompt("Search: %s (ESC to cancel)", editor_find_callback);
    if (query == NULL)
        return;
    
    free(query);
}

/* INPUT */

char *editor_prompt(char *prompt, void (*callback) (char *, int))
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1)
    {
        editor_set_status_message(prompt, buf);
        editor_refresh_screen();

        int c = editor_read_key();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if (buflen != 0)
                buf[--buflen] = '\0';
        }
        else if (c == '\x1b')
        {
            editor_set_status_message("");
            free(buf);
            if (callback) callback(buf, c);
            return NULL;
        }
        else if (c == '\r')
        {
            if (buflen != 0)
            {
                editor_set_status_message("");
                if (callback) callback(buf, c);
                return buf;
            }
        }
        else if (!iscntrl(c) && c < 128)
        {
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback) callback(buf, c);
    }
}

void editor_move_cursor(int key)
{
    erow *cur_row = (E.cx >= E.numrows ? NULL : E.rows + E.cx);

    switch (key)
    {
    case ARROW_LEFT:
        if (E.cy != 0)
            E.cy--;
        else if (E.cx > 0)
        {
            E.cx--;
            E.cy = E.rows[E.cx].size + 1;
        }
        break;
    case ARROW_RIGHT:
        if (cur_row && cur_row->size > E.cy)
            E.cy++;
        else if (cur_row && cur_row->size == E.cy)
        {
            E.cx++;
            E.cy = 0;
        }
        break;
    case ARROW_UP:
        if (E.cx > 0)
            E.cx--;
        break;
    case ARROW_DOWN:
        if (E.cx < E.numrows)
            E.cx++;
        break;
    default:
        break;
    }

    cur_row = (E.cx >= E.numrows ? NULL : E.rows + E.cx);
    int rowlen = cur_row == NULL ? 0 : cur_row->size;
    if (E.cy > rowlen)
        E.cy = rowlen;
}

void editor_process_keypress()
{
    static int to_quit = Q_TIMES_TO_QUIT;

    int c = editor_read_key();

    switch (c)
    {
    case '\r':
        editor_insert_new_line();
        break;
    case CTRL_KEY('q'):
        if (E.dirty && to_quit > 0)
        {
            editor_set_status_message("file has been modified, press Ctrl-Q %d more times to quit without saving", to_quit);
            to_quit--;
            return;
        }
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;

    case CTRL_KEY('s'):
        editor_save();
        break;
    
    case CTRL_KEY('f'):
        editor_find();
        break;

    case PAGE_UP:
    case PAGE_DOWN:
    {
        if (c == PAGE_UP)
            E.cx = E.rowoff;
        else if (c == PAGE_DOWN)
        {
            E.cx = E.rowoff + E.screenrows - 1;
            if (E.cx > E.numrows)
                E.cx = E.numrows;
        }

        int times = E.screenrows;
        while (times--)
            editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;

    case HOME_KEY:
        E.cy = 0;
        break;
    case END_KEY:
        if (E.cx < E.numrows)
            E.cy = E.rows[E.cx].size;
        break;

    case BACKSPACE:
    case DEL_KEY:
    case CTRL_KEY('h'):
        if (c == DEL_KEY)
            editor_move_cursor(ARROW_RIGHT);
        editor_delete_char();

        break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editor_move_cursor(c);
        break;

    case CTRL_KEY('l'):
    case '\x1b':
        break;
    default:
        editor_insert_char(c);
        break;
    }
    to_quit = Q_TIMES_TO_QUIT;
}

/* APPEND BUFFER */

typedef struct abuf
{
    char *data;
    int len;
} abuf;

#define ABUF_INIT \
    {             \
        NULL, 0   \
    }

void ab_append(abuf *buf, const char *s, int len)
{
    char *new = realloc(buf->data, buf->len + len);
    if (new == NULL)
        return;
    memcpy(&new[buf->len], s, len);
    buf->data = new;
    buf->len += len;
}

void ab_free(abuf *buf)
{
    free(buf->data);
}

/* OUTPUT */

void editor_scroll()
{
    E.ry = E.cy;
    if (E.cx < E.numrows)
        E.ry = editor_row_cy_to_ry(E.rows + E.cx, E.cy);

    if (E.cx < E.rowoff)
        E.rowoff = E.cx;
    if (E.cx >= E.rowoff + E.screenrows)
        E.rowoff = E.cx - E.screenrows + 1;

    if (E.ry < E.coloff)
        E.coloff = E.ry;
    if (E.cy >= E.coloff + E.screencols)
        E.coloff = E.cy - E.screencols + 1;
}

void editor_draw_raws(abuf *ab)
{
    int i = E.rowoff;
    while (i < E.numrows && i - E.rowoff < E.screenrows)
    {
        int len = E.rows[i].rsize;

        if (len > E.coloff)
        {
            len = len - E.coloff;
            if (len > E.screencols)
                len = E.screencols;
            ab_append(ab, E.rows[i].render + E.coloff, len);
        }
        ab_append(ab, "\r\n", 2);
        i++;
    }
    i -= E.rowoff;

    for (; i < E.screenrows; i++)
    {
        if (E.numrows == 0 && i == 2 * E.screenrows / 3) // printing welcome sign
        {
            char welcome[80];
            int wlen = snprintf(welcome, sizeof(welcome), "Pico Editor -- version %s", PICO_VERSION);
            if (wlen > E.screencols)
                wlen = E.screencols;
            int padding = (E.screencols - wlen) / 2;
            if (padding)
            {
                ab_append(ab, "~", 1);
                padding--;
            }
            while (padding--)
                ab_append(ab, " ", 1);
            ab_append(ab, welcome, wlen);
        }
        else
        {
            ab_append(ab, "~", 1);
        }

        ab_append(ab, "\x1b[K", 3); // clear line after coursor pos (default value is 0 so [K ~ [0K)
        ab_append(ab, "\r\n", 2);
    }
}

void editor_draw_status_bar(abuf *ab)
{
    ab_append(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename ? E.filename : "[No Name]",
                       E.numrows,
                       E.dirty ? "[modified]" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d [%d%%]", E.cx + 1, E.numrows, E.numrows == 0 ? 0 : 100 * (E.cx + 1) / E.numrows);
    if (len > E.screencols)
        len = E.screencols;
    ab_append(ab, status, len);

    while (len < E.screencols)
    {
        if (E.screencols - len == rlen)
        {
            ab_append(ab, rstatus, rlen);
            break;
        }
        ab_append(ab, " ", 1);
        len++;
    }
    ab_append(ab, "\x1b[m", 3);
    ab_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(abuf *ab)
{
    ab_append(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols)
        msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        ab_append(ab, E.statusmsg, msglen);
}

void editor_refresh_screen()
{
    editor_scroll();

    abuf ab = ABUF_INIT;

    ab_append(&ab, "\x1b[2J", 4);
    ab_append(&ab, "\x1b[?25l", 6);
    ab_append(&ab, "\x1b[0H", 4);

    editor_draw_raws(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    char buf[32];
    int len = snprintf(buf, 32, "\x1b[%d;%dH", E.cx - E.rowoff + 1, E.ry - E.coloff + 1);
    ab_append(&ab, buf, len);

    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.data, ab.len);
    ab_free(&ab);
}

void editor_set_status_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/* INIT */

void init_editor()
{
    E.cx = 0;
    E.cy = 0;
    E.ry = 0;
    E.numrows = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.rows = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.dirty = 0;

    if (get_window_size(&E.screenrows, &E.screencols) == -1)
        die("get_window_size");
    E.screenrows -= 2;
}

int main(int argc, char **argv)
{
    enable_raw_mode();
    init_editor();
    if (argc >= 2)
    {
        editor_open(argv[1]);
    }

    editor_set_status_message("HELP: Ctrl-Q = quit | Ctrl-S = save");

    while (1)
    {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}
#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include "config.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

/* Keep config.h files from older releases source-compatible. */
#ifndef COMMAND_KEY
#define COMMAND_KEY 'o'
#endif
#ifndef SHOW_STATUS
#define SHOW_STATUS 1
#endif
#ifndef MAX_WINDOWS
#define MAX_WINDOWS 32
#endif
#ifndef DEFAULT_SCROLLBACK
#define DEFAULT_SCROLLBACK 2000
#endif
#ifndef CHILD_TERM
#define CHILD_TERM "screen-256color"
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MWIN_CTRL(c) ((unsigned char)((c) & 0x1f))
#define CSI_MAX 192
#define OSC_MAX 1024
#define COMBINING_MAX 3
#define INPUT_CHUNK 8192

static const unsigned char paste_begin[] = "\033[200~";
static const unsigned char paste_end[] = "\033[201~";

enum {
	ATTR_BOLD      = 1u << 0,
	ATTR_DIM       = 1u << 1,
	ATTR_ITALIC    = 1u << 2,
	ATTR_UNDERLINE = 1u << 3,
	ATTR_BLINK     = 1u << 4,
	ATTR_REVERSE   = 1u << 5,
	ATTR_INVISIBLE = 1u << 6,
	ATTR_STRIKE    = 1u << 7
};

static const struct {
	int set;
	int reset;
	unsigned int flag;
} rendition_flags[] = {
	{1, 22, ATTR_BOLD},      {2, 22, ATTR_DIM},
	{3, 23, ATTR_ITALIC},    {4, 24, ATTR_UNDERLINE},
	{5, 25, ATTR_BLINK},     {7, 27, ATTR_REVERSE},
	{8, 28, ATTR_INVISIBLE}, {9, 29, ATTR_STRIKE}
};

enum ParserState {
	P_GROUND,
	P_ESCAPE,
	P_CSI,
	P_OSC,
	P_OSC_ESCAPE,
	P_STRING,
	P_STRING_ESCAPE,
	P_CHARSET
};

struct Attr {
	int fg;
	int bg;
	unsigned int flags;
};

struct Cell {
	uint32_t cp;
	uint32_t combining[COMBINING_MAX];
	unsigned char ncombining;
	unsigned char width;
	struct Attr attr;
};

struct StyleSpan {
	int column;
	struct Attr attr;
};

struct HistoryLine {
	char *text;
	size_t length;
	struct StyleSpan *spans;
	size_t span_count;
	bool wrapped;
};

struct History {
	struct HistoryLine *lines;
	size_t capacity;
	size_t count;
	size_t head;
};

struct ReflowBuffer {
	struct Cell *cells;
	unsigned char *wrapped;
	size_t count;
	size_t capacity;
	int cols;
	int col;
	bool need_row;
	bool have_last;
	size_t last_row;
	int last_col;
	bool cursor_set;
	size_t cursor_row;
	int cursor_col;
	bool cursor_wrap_pending;
	bool saved_set;
	size_t saved_row;
	int saved_col;
};

struct Terminal;

struct Screen {
	struct Cell *cells;
	unsigned char *dirty;
	unsigned char *wrapped;
	int rows;
	int cols;
	int row;
	int col;
	int saved_row;
	int saved_col;
	int scroll_top;
	int scroll_bottom;
	bool cursor_visible;
	bool origin;
	bool wrap;
	bool insert;
	bool wrap_pending;
	struct Terminal *terminal;
	struct Attr attr;
	struct Attr saved_attr;
};

struct Buffer {
	unsigned char *data;
	size_t offset;
	size_t length;
	size_t capacity;
};

struct Parser {
	enum ParserState state;
	char csi[CSI_MAX];
	size_t csi_length;
	char osc[OSC_MAX];
	size_t osc_length;
	uint32_t utf8_cp;
	uint32_t utf8_min;
	unsigned int utf8_need;
	unsigned int charset_slot;
};

struct Terminal {
	int fd;
	pid_t pid;
	struct Screen primary;
	struct Screen alternate;
	struct Screen *screen;
	struct Parser parser;
	struct Buffer input;
	struct History history;
	size_t scroll_offset;
	bool viewport_dirty;
	char title[64];
	bool unread;
	bool app_cursor;
	bool app_keypad;
	bool bracketed_paste;
	bool focus_events;
	int mouse_mode;
	bool mouse_sgr;
	bool g0_line_drawing;
	bool g1_line_drawing;
	bool use_g1;
};

static struct Terminal *windows[MAX_WINDOWS];
static size_t window_count;
static size_t active_window;
static struct Terminal *previous_window;
static struct termios original_termios;
static bool terminal_is_raw;
static bool running = true;
static bool prefix_pending;
static bool help_visible;
static bool input_is_paste;
static size_t paste_match_length;
static unsigned char scroll_escape_sequence[sizeof(paste_begin) - 1];
static size_t scroll_escape_length;
static bool status_enabled = SHOW_STATUS != 0;
static unsigned char command_prefix = MWIN_CTRL(COMMAND_KEY);
static size_t scrollback_limit = DEFAULT_SCROLLBACK;
static int host_rows = 24;
static int host_cols = 80;
static int signal_pipe[2] = {-1, -1};

static void fatal(const char *message);
static void render(bool full);
static void resize_all(void);
static void remove_window(size_t index, bool terminate);
static void normalize_row(struct Screen *screen, int row);
static void history_push(struct Terminal *terminal, const struct Cell *cells,
                         int columns, bool wrapped);
static void history_clear(struct Terminal *terminal);
static bool decode_utf8(const char *text, size_t length, size_t *offset,
                        uint32_t *codepoint);

static struct Attr
default_attr(void)
{
	struct Attr attr = {-1, -1, 0};
	return attr;
}

static bool
attr_equal(struct Attr a, struct Attr b)
{
	return a.fg == b.fg && a.bg == b.bg && a.flags == b.flags;
}

static struct Cell
blank_cell(struct Attr attr)
{
	struct Cell cell = {0};

	cell.cp = ' ';
	cell.width = 1;
	cell.attr = attr;
	return cell;
}

static int
clamp_int(int value, int minimum, int maximum)
{
	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;
	return value;
}

static void
fill_cells(struct Cell *cells, size_t count, struct Attr attr)
{
	struct Cell blank = blank_cell(attr);
	size_t i;

	for (i = 0; i < count; i++)
		cells[i] = blank;
}

static bool
allocate_screen(int rows, int cols, struct Cell **cells,
                unsigned char **dirty, unsigned char **wrapped)
{
	size_t count;

	*cells = NULL;
	*dirty = NULL;
	*wrapped = NULL;
	if (rows < 1 || cols < 1 ||
	    (size_t)rows > SIZE_MAX / (size_t)cols ||
	    (size_t)rows * (size_t)cols > SIZE_MAX / sizeof(**cells))
		return false;
	count = (size_t)rows * (size_t)cols;
	*cells = malloc(count * sizeof(**cells));
	*dirty = malloc((size_t)rows);
	*wrapped = calloc((size_t)rows, sizeof(**wrapped));
	if (*cells != NULL && *dirty != NULL && *wrapped != NULL)
		return true;
	free(*cells);
	free(*dirty);
	free(*wrapped);
	return false;
}

static void
replace_screen(struct Screen *screen, struct Cell *cells,
               unsigned char *dirty, unsigned char *wrapped,
               int rows, int cols)
{
	free(screen->cells);
	free(screen->dirty);
	free(screen->wrapped);
	screen->cells = cells;
	screen->dirty = dirty;
	screen->wrapped = wrapped;
	screen->rows = rows;
	screen->cols = cols;
}

static void
mark_all_dirty(struct Screen *screen)
{
	if (screen->dirty != NULL)
		memset(screen->dirty, 1, (size_t)screen->rows);
}

static void
screen_reset(struct Screen *screen)
{
	size_t count;

	screen->row = 0;
	screen->col = 0;
	screen->saved_row = 0;
	screen->saved_col = 0;
	screen->scroll_top = 0;
	screen->scroll_bottom = screen->rows - 1;
	screen->cursor_visible = true;
	screen->origin = false;
	screen->wrap = true;
	screen->insert = false;
	screen->wrap_pending = false;
	screen->attr = default_attr();
	screen->saved_attr = screen->attr;
	count = (size_t)screen->rows * (size_t)screen->cols;
	fill_cells(screen->cells, count, screen->attr);
	memset(screen->wrapped, 0, (size_t)screen->rows);
	mark_all_dirty(screen);
}

static bool
screen_init(struct Screen *screen, int rows, int cols)
{
	memset(screen, 0, sizeof(*screen));
	if (!allocate_screen(rows, cols, &screen->cells, &screen->dirty,
	                     &screen->wrapped))
		return false;
	screen->rows = rows;
	screen->cols = cols;
	screen_reset(screen);
	return true;
}

static void
screen_free(struct Screen *screen)
{
	free(screen->cells);
	free(screen->dirty);
	free(screen->wrapped);
	memset(screen, 0, sizeof(*screen));
}

static void
screen_resize(struct Screen *screen, int rows, int cols)
{
	struct Cell *new_cells;
	unsigned char *new_dirty;
	unsigned char *new_wrapped;
	int copy_rows = rows < screen->rows ? rows : screen->rows;
	int copy_cols = cols < screen->cols ? cols : screen->cols;
	int r;

	if (rows == screen->rows && cols == screen->cols)
		return;
	if (!allocate_screen(rows, cols, &new_cells, &new_dirty, &new_wrapped))
		return;
	fill_cells(new_cells, (size_t)rows * (size_t)cols, default_attr());
	for (r = 0; r < copy_rows; r++)
		memcpy(&new_cells[(size_t)r * (size_t)cols],
		       &screen->cells[(size_t)r * (size_t)screen->cols],
		       (size_t)copy_cols * sizeof(*new_cells));
	memcpy(new_wrapped, screen->wrapped, (size_t)copy_rows);
	memset(new_dirty, 1, (size_t)rows);
	replace_screen(screen, new_cells, new_dirty, new_wrapped, rows, cols);
	screen->row = clamp_int(screen->row, 0, rows - 1);
	screen->col = clamp_int(screen->col, 0, cols - 1);
	screen->saved_row = clamp_int(screen->saved_row, 0, rows - 1);
	screen->saved_col = clamp_int(screen->saved_col, 0, cols - 1);
	screen->scroll_top = 0;
	screen->scroll_bottom = rows - 1;
	screen->wrap_pending = false;
	for (r = 0; r < rows; r++)
		normalize_row(screen, r);
}

static struct Cell *
cell_at(struct Screen *screen, int row, int col)
{
	return &screen->cells[(size_t)row * (size_t)screen->cols + (size_t)col];
}

static void
blank_range(struct Screen *screen, int row, int first, int last)
{
	struct Cell blank = blank_cell(screen->attr);
	int col;

	if (row < 0 || row >= screen->rows)
		return;
	if (first < 0)
		first = 0;
	if (last >= screen->cols)
		last = screen->cols - 1;
	if (first > 0 && cell_at(screen, row, first)->width == 0 &&
	    cell_at(screen, row, first - 1)->width == 2)
		first--;
	if (last + 1 < screen->cols && cell_at(screen, row, last)->width == 2)
		last++;
	if (first == 0 && last == screen->cols - 1)
		screen->wrapped[row] = 0;
	for (col = first; col <= last; col++)
		*cell_at(screen, row, col) = blank;
	screen->dirty[row] = 1;
}

static void
normalize_row(struct Screen *screen, int row)
{
	struct Cell blank = blank_cell(screen->attr);
	int col;

	for (col = 0; col < screen->cols; col++) {
		struct Cell *cell = cell_at(screen, row, col);
		if (cell->width == 2) {
			if (col + 1 >= screen->cols) {
				*cell = blank;
			} else {
				struct Cell *next = cell_at(screen, row, col + 1);
				memset(next, 0, sizeof(*next));
				next->width = 0;
				next->attr = cell->attr;
				col++;
			}
		} else if (cell->width == 0) {
			*cell = blank;
		}
	}
	screen->dirty[row] = 1;
}

static void
scroll_region(struct Screen *screen, int count, bool down, bool record)
{
	int top = screen->scroll_top;
	int bottom = screen->scroll_bottom;
	int height = bottom - top + 1;
	int source;
	int destination;
	int first_blank;
	int row;

	count = clamp_int(count, 1, height);
	if (!down && record && screen->terminal != NULL &&
	    screen == &screen->terminal->primary &&
	    top == 0 && bottom == screen->rows - 1) {
		for (row = 0; row < count; row++)
			history_push(screen->terminal, cell_at(screen, row, 0), screen->cols,
			             screen->wrapped[row] != 0);
	}
	if (down) {
		source = top;
		destination = top + count;
		first_blank = top;
	} else {
		source = top + count;
		destination = top;
		first_blank = bottom - count + 1;
	}
	if (count < height) {
		memmove(cell_at(screen, destination, 0), cell_at(screen, source, 0),
		        (size_t)(height - count) * (size_t)screen->cols *
		        sizeof(struct Cell));
		memmove(&screen->wrapped[destination], &screen->wrapped[source],
		        (size_t)(height - count));
	}
	for (row = first_blank; row < first_blank + count; row++)
		blank_range(screen, row, 0, screen->cols - 1);
	for (row = top; row <= bottom; row++)
		screen->dirty[row] = 1;
}

static void
scroll_up(struct Screen *screen, int count, bool record)
{
	scroll_region(screen, count, false, record);
}

static void
scroll_down(struct Screen *screen, int count)
{
	scroll_region(screen, count, true, false);
}

static void
screen_index(struct Screen *screen, bool soft_wrap)
{
	unsigned char wrapped = soft_wrap ? 1 : 0;

	if (screen->wrapped[screen->row] != wrapped)
		screen->dirty[screen->row] = 1;
	screen->wrapped[screen->row] = wrapped;
	if (screen->row == screen->scroll_bottom)
		scroll_up(screen, 1, true);
	else if (screen->row < screen->rows - 1)
		screen->row++;
	screen->wrap_pending = false;
}

static void
screen_reverse_index(struct Screen *screen)
{
	if (screen->row == screen->scroll_top)
		scroll_down(screen, 1);
	else if (screen->row > 0)
		screen->row--;
	screen->wrap_pending = false;
}

static void
move_cursor(struct Screen *screen, int row, int col)
{
	screen->row = clamp_int(row, 0, screen->rows - 1);
	screen->col = clamp_int(col, 0, screen->cols - 1);
	screen->wrap_pending = false;
}

static void
save_cursor(struct Screen *screen)
{
	screen->saved_row = screen->row;
	screen->saved_col = screen->col;
	screen->saved_attr = screen->attr;
}

static void
restore_cursor(struct Screen *screen)
{
	move_cursor(screen, screen->saved_row, screen->saved_col);
	screen->attr = screen->saved_attr;
}

static void
erase_cell(struct Screen *screen, int row, int col)
{
	struct Cell *cell;
	struct Cell blank = blank_cell(screen->attr);

	if (row < 0 || row >= screen->rows || col < 0 || col >= screen->cols)
		return;
	cell = cell_at(screen, row, col);
	if (cell->width == 0 && col > 0) {
		*cell_at(screen, row, col - 1) = blank;
	} else if (cell->width == 2 && col + 1 < screen->cols) {
		*cell_at(screen, row, col + 1) = blank;
	}
	*cell = blank;
	screen->dirty[row] = 1;
}

static void
insert_cells(struct Screen *screen, int count)
{
	int available = screen->cols - screen->col;

	count = clamp_int(count, 1, available);
	if (count < available)
		memmove(cell_at(screen, screen->row, screen->col + count),
		        cell_at(screen, screen->row, screen->col),
		        (size_t)(available - count) * sizeof(struct Cell));
	blank_range(screen, screen->row, screen->col, screen->col + count - 1);
	normalize_row(screen, screen->row);
}

static void
delete_cells(struct Screen *screen, int count)
{
	int available = screen->cols - screen->col;

	count = clamp_int(count, 1, available);
	if (count < available)
		memmove(cell_at(screen, screen->row, screen->col),
		        cell_at(screen, screen->row, screen->col + count),
		        (size_t)(available - count) * sizeof(struct Cell));
	blank_range(screen, screen->row, screen->cols - count, screen->cols - 1);
	normalize_row(screen, screen->row);
}

static void
insert_lines(struct Screen *screen, int count)
{
	int old_top;

	if (screen->row < screen->scroll_top || screen->row > screen->scroll_bottom)
		return;
	old_top = screen->scroll_top;
	screen->scroll_top = screen->row;
	scroll_down(screen, count);
	screen->scroll_top = old_top;
}

static void
delete_lines(struct Screen *screen, int count)
{
	int old_top;

	if (screen->row < screen->scroll_top || screen->row > screen->scroll_bottom)
		return;
	old_top = screen->scroll_top;
	screen->scroll_top = screen->row;
	scroll_up(screen, count, false);
	screen->scroll_top = old_top;
}

static void
put_codepoint(struct Screen *screen, uint32_t cp)
{
	struct Cell *cell;
	int width = wcwidth((wchar_t)cp);
	int previous;

	if (width == 0) {
		previous = screen->col - 1;
		if (screen->wrap_pending)
			previous = screen->col;
		while (previous >= 0 && cell_at(screen, screen->row, previous)->width == 0)
			previous--;
		if (previous >= 0) {
			cell = cell_at(screen, screen->row, previous);
			if (cell->ncombining < COMBINING_MAX)
				cell->combining[cell->ncombining++] = cp;
			screen->dirty[screen->row] = 1;
		}
		return;
	}
	if (width < 0 || width > 2)
		width = 1;
	if (screen->wrap_pending) {
		if (screen->wrap) {
			screen->col = 0;
			screen_index(screen, true);
		}
		screen->wrap_pending = false;
	}
	if (width == 2 && screen->col == screen->cols - 1) {
		if (screen->wrap) {
			screen->col = 0;
			screen_index(screen, true);
		} else {
			width = 1;
		}
	}
	if (screen->insert)
		insert_cells(screen, width);
	erase_cell(screen, screen->row, screen->col);
	if (width == 2 && screen->col + 1 < screen->cols)
		erase_cell(screen, screen->row, screen->col + 1);
	cell = cell_at(screen, screen->row, screen->col);
	memset(cell, 0, sizeof(*cell));
	cell->cp = cp;
	cell->width = (unsigned char)width;
	cell->attr = screen->attr;
	if (width == 2 && screen->col + 1 < screen->cols) {
		cell = cell_at(screen, screen->row, screen->col + 1);
		memset(cell, 0, sizeof(*cell));
		cell->width = 0;
		cell->attr = screen->attr;
	}
	screen->dirty[screen->row] = 1;
	if (screen->col + width >= screen->cols) {
		screen->col = screen->cols - 1;
		screen->wrap_pending = true;
	} else {
		screen->col += width;
	}
}

static bool
buffer_append(struct Buffer *buffer, const void *data, size_t length)
{
	size_t used = buffer->length - buffer->offset;
	size_t wanted;
	unsigned char *new_data;

	if (length == 0)
		return true;
	if (buffer->offset != 0 && used != 0)
		memmove(buffer->data, buffer->data + buffer->offset, used);
	buffer->offset = 0;
	buffer->length = used;
	if (length > SIZE_MAX - buffer->length)
		return false;
	wanted = buffer->length + length;
	if (wanted > buffer->capacity) {
		size_t capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
		while (capacity < wanted) {
			if (capacity > SIZE_MAX / 2) {
				capacity = wanted;
				break;
			}
			capacity *= 2;
		}
		new_data = realloc(buffer->data, capacity);
		if (new_data == NULL)
			return false;
		buffer->data = new_data;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->length, data, length);
	buffer->length += length;
	return true;
}

static void
buffer_free(struct Buffer *buffer)
{
	free(buffer->data);
	memset(buffer, 0, sizeof(*buffer));
}

static void
queue_reply(struct Terminal *terminal, const char *reply)
{
	(void)buffer_append(&terminal->input, reply, strlen(reply));
}

static void
erase_display(struct Screen *screen, int mode)
{
	int row;

	if (mode == 2 || mode == 3) {
		for (row = 0; row < screen->rows; row++)
			blank_range(screen, row, 0, screen->cols - 1);
	} else if (mode == 1) {
		for (row = 0; row < screen->row; row++)
			blank_range(screen, row, 0, screen->cols - 1);
		blank_range(screen, screen->row, 0, screen->col);
	} else {
		blank_range(screen, screen->row, screen->col, screen->cols - 1);
		for (row = screen->row + 1; row < screen->rows; row++)
			blank_range(screen, row, 0, screen->cols - 1);
	}
}

static void
erase_line(struct Screen *screen, int mode)
{
	if (mode == 1)
		blank_range(screen, screen->row, 0, screen->col);
	else if (mode == 2)
		blank_range(screen, screen->row, 0, screen->cols - 1);
	else
		blank_range(screen, screen->row, screen->col, screen->cols - 1);
}

static int
parse_parameters(const char *text, int *parameters, size_t capacity, char *private_marker)
{
	size_t index = 0;
	const char *cursor = text;

	*private_marker = '\0';
	if (*cursor == '?' || *cursor == '>' || *cursor == '!' || *cursor == '=')
		*private_marker = *cursor++;
	if (*cursor == '\0') {
		parameters[0] = -1;
		return 1;
	}
	while (*cursor != '\0' && index < capacity) {
		if (*cursor == ';' || *cursor == ':') {
			parameters[index++] = -1;
			cursor++;
			continue;
		}
		if (isdigit((unsigned char)*cursor)) {
			long value = 0;
			while (isdigit((unsigned char)*cursor)) {
				value = value * 10 + (*cursor - '0');
				if (value > 100000)
					value = 100000;
				cursor++;
			}
			parameters[index++] = (int)value;
		} else {
			cursor++;
		}
		if (*cursor == ';' || *cursor == ':') {
			cursor++;
			if (*cursor == '\0' && index < capacity)
				parameters[index++] = -1;
		}
	}
	return (int)index;
}

static int
parameter(const int *parameters, int count, int index, int fallback)
{
	if (index >= count || parameters[index] < 0)
		return fallback;
	return parameters[index];
}

static void
set_rendition(struct Screen *screen, const int *p, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		int code = p[i] < 0 ? 0 : p[i];
		size_t flag;
		bool handled = false;

		if (code == 0)
			screen->attr = default_attr();
		else if (code == 21)
			screen->attr.flags |= ATTR_UNDERLINE;
		else if (code == 6)
			screen->attr.flags |= ATTR_BLINK;
		for (flag = 0; flag < LEN(rendition_flags); flag++) {
			if (code == rendition_flags[flag].set) {
				screen->attr.flags |= rendition_flags[flag].flag;
				handled = true;
			} else if (code == rendition_flags[flag].reset) {
				screen->attr.flags &= ~rendition_flags[flag].flag;
				handled = true;
			}
		}
		if (code == 0 || code == 6 || code == 21 || handled)
			continue;
		if (code >= 30 && code <= 37)
			screen->attr.fg = code - 30;
		else if (code >= 90 && code <= 97)
			screen->attr.fg = code - 90 + 8;
		else if (code == 39)
			screen->attr.fg = -1;
		else if (code >= 40 && code <= 47)
			screen->attr.bg = code - 40;
		else if (code >= 100 && code <= 107)
			screen->attr.bg = code - 100 + 8;
		else if (code == 49)
			screen->attr.bg = -1;
		else if ((code == 38 || code == 48) && i + 2 < count && p[i + 1] == 5) {
			int value = p[i + 2];
			int *color = code == 38 ? &screen->attr.fg : &screen->attr.bg;
			if (value >= 0 && value <= 255)
				*color = value;
			i += 2;
		} else if ((code == 38 || code == 48) && i + 4 < count && p[i + 1] == 2) {
			int red = p[i + 2];
			int green = p[i + 3];
			int blue = p[i + 4];
			int *color = code == 38 ? &screen->attr.fg : &screen->attr.bg;
			if (red >= 0 && red <= 255 && green >= 0 && green <= 255 &&
			    blue >= 0 && blue <= 255)
				*color = 0x1000000 | (red << 16) | (green << 8) | blue;
			i += 4;
		}
	}
}

static void
use_alternate(struct Terminal *terminal, bool enabled, bool clear)
{
	if (enabled) {
		if (clear)
			screen_reset(&terminal->alternate);
		terminal->screen = &terminal->alternate;
	} else {
		terminal->screen = &terminal->primary;
	}
	mark_all_dirty(terminal->screen);
}

static void
set_private_mode(struct Terminal *terminal, int mode, bool enabled)
{
	struct Screen *screen = terminal->screen;

	switch (mode) {
	case 1:
		terminal->app_cursor = enabled;
		break;
	case 6:
		screen->origin = enabled;
		screen->row = enabled ? screen->scroll_top : 0;
		screen->col = 0;
		screen->wrap_pending = false;
		break;
	case 7:
		screen->wrap = enabled;
		break;
	case 25:
		screen->cursor_visible = enabled;
		break;
	case 47:
	case 1047:
		use_alternate(terminal, enabled, enabled);
		break;
	case 1048:
		if (enabled)
			save_cursor(screen);
		else
			restore_cursor(screen);
		break;
	case 1049:
		if (enabled) {
			save_cursor(&terminal->primary);
			use_alternate(terminal, true, true);
		} else {
			use_alternate(terminal, false, false);
			restore_cursor(&terminal->primary);
		}
		break;
	case 1000:
	case 1002:
	case 1003:
		terminal->mouse_mode = enabled ? mode : 0;
		break;
	case 1004:
		terminal->focus_events = enabled;
		break;
	case 1006:
		terminal->mouse_sgr = enabled;
		break;
	case 2004:
		terminal->bracketed_paste = enabled;
		break;
	default:
		break;
	}
}

static void
handle_csi(struct Terminal *terminal, char final)
{
	struct Screen *screen = terminal->screen;
	int p[32];
	char private_marker;
	int count;
	int amount;
	int row;
	int col;
	int i;
	char reply[64];

	terminal->parser.csi[terminal->parser.csi_length] = '\0';
	count = parse_parameters(terminal->parser.csi, p, LEN(p), &private_marker);
	amount = parameter(p, count, 0, 1);
	if (amount < 1)
		amount = 1;

	switch (final) {
	case 'A':
		move_cursor(screen,
		            clamp_int(screen->row - amount,
		                      screen->origin ? screen->scroll_top : 0,
		                      screen->rows - 1), screen->col);
		break;
	case 'B':
	case 'e':
		move_cursor(screen,
		            clamp_int(screen->row + amount, 0,
		                      screen->origin ? screen->scroll_bottom :
		                                       screen->rows - 1), screen->col);
		break;
	case 'C':
	case 'a':
		move_cursor(screen, screen->row, screen->col + amount);
		break;
	case 'D':
		move_cursor(screen, screen->row, screen->col - amount);
		break;
	case 'E':
		move_cursor(screen, screen->row + amount, 0);
		break;
	case 'F':
		move_cursor(screen, screen->row - amount, 0);
		break;
	case 'G':
	case '`':
		move_cursor(screen, screen->row, amount - 1);
		break;
	case 'H':
	case 'f':
		row = parameter(p, count, 0, 1) - 1;
		col = parameter(p, count, 1, 1) - 1;
		if (screen->origin)
			row += screen->scroll_top;
		row = clamp_int(row, screen->origin ? screen->scroll_top : 0,
		                screen->origin ? screen->scroll_bottom : screen->rows - 1);
		move_cursor(screen, row, col);
		break;
	case 'd':
		row = amount - 1 + (screen->origin ? screen->scroll_top : 0);
		move_cursor(screen, row, screen->col);
		break;
	case 'J':
		if (parameter(p, count, 0, 0) == 3)
			history_clear(terminal);
		else
			erase_display(screen, parameter(p, count, 0, 0));
		break;
	case 'K':
		erase_line(screen, parameter(p, count, 0, 0));
		break;
	case '@':
		insert_cells(screen, amount);
		break;
	case 'P':
		delete_cells(screen, amount);
		break;
	case 'X':
		blank_range(screen, screen->row, screen->col,
		            screen->col + amount - 1);
		break;
	case 'L':
		insert_lines(screen, amount);
		break;
	case 'M':
		delete_lines(screen, amount);
		break;
	case 'S':
		scroll_up(screen, amount, true);
		break;
	case 'T':
		scroll_down(screen, amount);
		break;
	case 'm':
		set_rendition(screen, p, count);
		break;
	case 'r':
		if (private_marker == '\0') {
			int top = parameter(p, count, 0, 1) - 1;
			int bottom = parameter(p, count, 1, screen->rows) - 1;
			if (top >= 0 && bottom < screen->rows && top < bottom) {
				screen->scroll_top = top;
				screen->scroll_bottom = bottom;
				screen->row = screen->origin ? top : 0;
				screen->col = 0;
				screen->wrap_pending = false;
			}
		}
		break;
	case 's':
		save_cursor(screen);
		break;
	case 'u':
		restore_cursor(screen);
		break;
	case 'h':
	case 'l':
		if (private_marker == '?') {
			for (i = 0; i < count; i++)
				set_private_mode(terminal, p[i], final == 'h');
		} else if (parameter(p, count, 0, 0) == 4) {
			screen->insert = final == 'h';
		}
		break;
	case 'n':
		if (private_marker == '\0' && parameter(p, count, 0, 0) == 5)
			queue_reply(terminal, "\033[0n");
		else if (private_marker == '\0' && parameter(p, count, 0, 0) == 6) {
			(void)snprintf(reply, sizeof(reply), "\033[%d;%dR",
			               screen->row + 1, screen->col + 1);
			queue_reply(terminal, reply);
		}
		break;
	case 'c':
		if (private_marker == '>')
			queue_reply(terminal, "\033[>1;0;0c");
		else
			queue_reply(terminal, "\033[?1;2c");
		break;
	default:
		break;
	}
}

static void
finish_osc(struct Terminal *terminal)
{
	char *separator;
	long command;
	char *end;
	size_t i;

	terminal->parser.osc[terminal->parser.osc_length] = '\0';
	separator = strchr(terminal->parser.osc, ';');
	if (separator == NULL)
		return;
	*separator++ = '\0';
	errno = 0;
	command = strtol(terminal->parser.osc, &end, 10);
	if (errno != 0 || *end != '\0' || (command != 0 && command != 2))
		return;
	for (i = 0; i + 1 < sizeof(terminal->title) && separator[i] != '\0'; i++) {
		unsigned char ch = (unsigned char)separator[i];
		terminal->title[i] = (char)(isprint(ch) ? ch : ' ');
	}
	terminal->title[i] = '\0';
}

static void
terminal_reset(struct Terminal *terminal)
{
	history_clear(terminal);
	screen_reset(&terminal->primary);
	screen_reset(&terminal->alternate);
	terminal->screen = &terminal->primary;
	terminal->app_cursor = false;
	terminal->app_keypad = false;
	terminal->bracketed_paste = false;
	terminal->focus_events = false;
	terminal->mouse_mode = 0;
	terminal->mouse_sgr = false;
	terminal->g0_line_drawing = false;
	terminal->g1_line_drawing = false;
	terminal->use_g1 = false;
}

static uint32_t
dec_graphic(unsigned char byte)
{
	static const uint32_t graphic[] = {
		0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,
		0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
		0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
		0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7
	};
	if (byte >= '`' && byte <= '~')
		return graphic[byte - '`'];
	return byte;
}

static void
handle_control(struct Terminal *terminal, unsigned char byte)
{
	struct Screen *screen = terminal->screen;

	switch (byte) {
	case '\a':
		if (window_count != 0 && windows[active_window] == terminal) {
			ssize_t ignored = write(STDOUT_FILENO, "\a", 1);
			(void)ignored;
		}
		break;
	case '\b':
		if (screen->col > 0)
			screen->col--;
		screen->wrap_pending = false;
		break;
	case '\t':
		screen->col = ((screen->col / 8) + 1) * 8;
		if (screen->col >= screen->cols)
			screen->col = screen->cols - 1;
		screen->wrap_pending = false;
		break;
	case '\n':
	case '\v':
	case '\f':
		screen_index(screen, false);
		break;
	case '\r':
		screen->col = 0;
		screen->wrap_pending = false;
		break;
	case 0x0e:
		terminal->use_g1 = true;
		break;
	case 0x0f:
		terminal->use_g1 = false;
		break;
	default:
		break;
	}
}

static void
feed_ground_byte(struct Terminal *terminal, unsigned char byte)
{
	struct Parser *parser = &terminal->parser;

	if (parser->utf8_need != 0) {
		if ((byte & 0xc0u) == 0x80u) {
			parser->utf8_cp = (parser->utf8_cp << 6) | (uint32_t)(byte & 0x3fu);
			parser->utf8_need--;
			if (parser->utf8_need == 0) {
				uint32_t cp = parser->utf8_cp;
				if (cp < parser->utf8_min || cp > 0x10ffffu ||
				    (cp >= 0xd800u && cp <= 0xdfffu))
					cp = 0xfffdu;
				put_codepoint(terminal->screen, cp);
			}
			return;
		}
		parser->utf8_need = 0;
		put_codepoint(terminal->screen, 0xfffdu);
	}
	if (byte == 0x1b) {
		parser->state = P_ESCAPE;
	} else if (byte < 0x20 || byte == 0x7f) {
		handle_control(terminal, byte);
	} else if (byte < 0x80) {
		bool line_drawing = terminal->use_g1 ? terminal->g1_line_drawing :
		                                             terminal->g0_line_drawing;
		put_codepoint(terminal->screen, line_drawing ? dec_graphic(byte) : byte);
	} else if ((byte & 0xe0u) == 0xc0u) {
		parser->utf8_cp = (uint32_t)(byte & 0x1fu);
		parser->utf8_min = 0x80u;
		parser->utf8_need = 1;
	} else if ((byte & 0xf0u) == 0xe0u) {
		parser->utf8_cp = (uint32_t)(byte & 0x0fu);
		parser->utf8_min = 0x800u;
		parser->utf8_need = 2;
	} else if ((byte & 0xf8u) == 0xf0u) {
		parser->utf8_cp = (uint32_t)(byte & 0x07u);
		parser->utf8_min = 0x10000u;
		parser->utf8_need = 3;
	} else {
		put_codepoint(terminal->screen, 0xfffdu);
	}
}

static void
feed_byte(struct Terminal *terminal, unsigned char byte)
{
	struct Parser *parser = &terminal->parser;
	struct Screen *screen = terminal->screen;

	switch (parser->state) {
	case P_GROUND:
		feed_ground_byte(terminal, byte);
		break;
	case P_ESCAPE:
		parser->state = P_GROUND;
		switch (byte) {
		case '[':
			parser->csi_length = 0;
			parser->state = P_CSI;
			break;
		case ']':
			parser->osc_length = 0;
			parser->state = P_OSC;
			break;
		case 'P':
		case '^':
		case '_':
			parser->state = P_STRING;
			break;
		case '(':
			parser->charset_slot = 0;
			parser->state = P_CHARSET;
			break;
		case ')':
			parser->charset_slot = 1;
			parser->state = P_CHARSET;
			break;
		case '*':
		case '+':
			parser->charset_slot = 2;
			parser->state = P_CHARSET;
			break;
		case '7':
			save_cursor(screen);
			break;
		case '8':
			restore_cursor(screen);
			break;
		case 'D':
			screen_index(screen, false);
			break;
		case 'E':
			screen->col = 0;
			screen_index(screen, false);
			break;
		case 'M':
			screen_reverse_index(screen);
			break;
		case 'c':
			terminal_reset(terminal);
			break;
		case '=':
			terminal->app_keypad = true;
			break;
		case '>':
			terminal->app_keypad = false;
			break;
		default:
			break;
		}
		break;
	case P_CSI:
		if (byte == 0x1b) {
			parser->state = P_ESCAPE;
		} else if (byte >= 0x40 && byte <= 0x7e) {
			handle_csi(terminal, (char)byte);
			parser->state = P_GROUND;
		} else if (parser->csi_length + 1 < sizeof(parser->csi)) {
			parser->csi[parser->csi_length++] = (char)byte;
		}
		break;
	case P_OSC:
		if (byte == '\a') {
			finish_osc(terminal);
			parser->state = P_GROUND;
		} else if (byte == 0x1b) {
			parser->state = P_OSC_ESCAPE;
		} else if (parser->osc_length + 1 < sizeof(parser->osc)) {
			parser->osc[parser->osc_length++] = (char)byte;
		}
		break;
	case P_OSC_ESCAPE:
		if (byte == '\\') {
			finish_osc(terminal);
			parser->state = P_GROUND;
		} else {
			if (parser->osc_length + 2 < sizeof(parser->osc)) {
				parser->osc[parser->osc_length++] = '\033';
				parser->osc[parser->osc_length++] = (char)byte;
			}
			parser->state = P_OSC;
		}
		break;
	case P_STRING:
		if (byte == 0x1b)
			parser->state = P_STRING_ESCAPE;
		break;
	case P_STRING_ESCAPE:
		parser->state = byte == '\\' ? P_GROUND : P_STRING;
		break;
	case P_CHARSET:
		if (parser->charset_slot == 0)
			terminal->g0_line_drawing = byte == '0';
		else if (parser->charset_slot == 1)
			terminal->g1_line_drawing = byte == '0';
		parser->state = P_GROUND;
		break;
	}
}

static void
feed_bytes(struct Terminal *terminal, const unsigned char *data, size_t length)
{
	size_t i;
	for (i = 0; i < length; i++)
		feed_byte(terminal, data[i]);
}

static void
output_append(struct Buffer *output, const char *text)
{
	(void)buffer_append(output, text, strlen(text));
}

static void
output_printf(struct Buffer *output, const char *format, ...)
{
	va_list arguments;
	int length;
	char text[192];

	va_start(arguments, format);
	length = vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	if (length > 0)
		(void)buffer_append(output, text,
		                    (size_t)length < sizeof(text) ?
		                    (size_t)length : sizeof(text) - 1);
}

static void
append_codepoint(struct Buffer *output, uint32_t cp)
{
	unsigned char encoded[4];
	size_t length;

	if (cp <= 0x7fu) {
		encoded[0] = (unsigned char)cp;
		length = 1;
	} else if (cp <= 0x7ffu) {
		encoded[0] = (unsigned char)(0xc0u | (cp >> 6));
		encoded[1] = (unsigned char)(0x80u | (cp & 0x3fu));
		length = 2;
	} else if (cp <= 0xffffu) {
		encoded[0] = (unsigned char)(0xe0u | (cp >> 12));
		encoded[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3fu));
		encoded[2] = (unsigned char)(0x80u | (cp & 0x3fu));
		length = 3;
	} else {
		encoded[0] = (unsigned char)(0xf0u | (cp >> 18));
		encoded[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3fu));
		encoded[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3fu));
		encoded[3] = (unsigned char)(0x80u | (cp & 0x3fu));
		length = 4;
	}
	(void)buffer_append(output, encoded, length);
}

static bool
cell_is_empty(const struct Cell *cell)
{
	return cell->cp == ' ' && cell->ncombining == 0 && cell->width == 1 &&
	       attr_equal(cell->attr, default_attr());
}

static void
history_line_free(struct HistoryLine *line)
{
	free(line->text);
	free(line->spans);
	memset(line, 0, sizeof(*line));
}

static void
history_push(struct Terminal *terminal, const struct Cell *cells, int columns,
             bool wrapped)
{
	struct History *history = &terminal->history;
	struct HistoryLine line = {0};
	struct Buffer text = {0};
	struct StyleSpan *spans = NULL;
	struct Attr previous = default_attr();
	size_t span_count = 0;
	size_t index;
	int last = 0;
	int col;
	bool overwriting;
	bool viewing_oldest;

	if (history->capacity == 0)
		return;
	for (col = 0; col < columns; col++) {
		const struct Cell *cell = &cells[col];
		if (cell->width != 0 && !cell_is_empty(cell))
			last = col + cell->width;
	}
	if (last > 0) {
		spans = calloc((size_t)last, sizeof(*spans));
		if (spans == NULL)
			return;
	}
	for (col = 0; col < last; col++) {
		const struct Cell *cell = &cells[col];
		unsigned int combining;
		if (cell->width == 0)
			continue;
		if (!attr_equal(previous, cell->attr)) {
			spans[span_count].column = col;
			spans[span_count].attr = cell->attr;
			span_count++;
			previous = cell->attr;
		}
		append_codepoint(&text, cell->cp == 0 ? ' ' : cell->cp);
		for (combining = 0; combining < cell->ncombining; combining++)
			append_codepoint(&text, cell->combining[combining]);
	}
	if (span_count == 0) {
		free(spans);
		spans = NULL;
	} else {
		struct StyleSpan *smaller = realloc(spans, span_count * sizeof(*spans));
		if (smaller != NULL)
			spans = smaller;
	}
	line.text = (char *)text.data;
	line.length = text.length;
	line.spans = spans;
	line.span_count = span_count;
	line.wrapped = wrapped;
	if (history->lines == NULL) {
		history->lines = calloc(history->capacity, sizeof(*history->lines));
		if (history->lines == NULL) {
			history->capacity = 0;
			history_line_free(&line);
			return;
		}
	}
	overwriting = history->count == history->capacity;
	viewing_oldest = terminal->scroll_offset != 0 &&
	                 terminal->scroll_offset == history->count;
	if (overwriting) {
		index = history->head;
		history_line_free(&history->lines[index]);
		history->head = (history->head + 1) % history->capacity;
	} else {
		index = (history->head + history->count) % history->capacity;
		history->count++;
	}
	history->lines[index] = line;
	if (terminal->scroll_offset != 0) {
		if (terminal->scroll_offset < history->count)
			terminal->scroll_offset++;
		if (overwriting && viewing_oldest)
			terminal->viewport_dirty = true;
	}
}

static void
history_clear(struct Terminal *terminal)
{
	struct History *history = &terminal->history;
	size_t i;

	if (history->lines != NULL) {
		for (i = 0; i < history->capacity; i++)
			history_line_free(&history->lines[i]);
	}
	history->count = 0;
	history->head = 0;
	terminal->scroll_offset = 0;
	terminal->viewport_dirty = true;
	if (terminal->screen != NULL)
		mark_all_dirty(terminal->screen);
}

static void
history_free(struct Terminal *terminal)
{
	history_clear(terminal);
	free(terminal->history.lines);
	memset(&terminal->history, 0, sizeof(terminal->history));
}

static const struct HistoryLine *
history_line(const struct History *history, size_t index)
{
	if (history->lines == NULL || index >= history->count)
		return NULL;
	return &history->lines[(history->head + index) % history->capacity];
}

static void
reflow_buffer_free(struct ReflowBuffer *buffer)
{
	free(buffer->cells);
	free(buffer->wrapped);
	memset(buffer, 0, sizeof(*buffer));
}

static bool
reflow_start_row(struct ReflowBuffer *buffer)
{
	size_t cells;

	if (!buffer->need_row)
		return true;
	if (buffer->count == buffer->capacity) {
		size_t capacity = buffer->capacity == 0 ? 64 : buffer->capacity * 2;
		struct Cell *new_cells;
		unsigned char *new_wrapped;

		if (capacity < buffer->capacity ||
		    capacity > SIZE_MAX / (size_t)buffer->cols / sizeof(*new_cells))
			return false;
		cells = capacity * (size_t)buffer->cols;
		new_cells = malloc(cells * sizeof(*new_cells));
		new_wrapped = malloc(capacity * sizeof(*new_wrapped));
		if (new_cells == NULL || new_wrapped == NULL) {
			free(new_cells);
			free(new_wrapped);
			return false;
		}
		if (buffer->count != 0) {
			memcpy(new_cells, buffer->cells,
			       buffer->count * (size_t)buffer->cols * sizeof(*new_cells));
			memcpy(new_wrapped, buffer->wrapped,
			       buffer->count * sizeof(*new_wrapped));
		}
		free(buffer->cells);
		free(buffer->wrapped);
		buffer->cells = new_cells;
		buffer->wrapped = new_wrapped;
		buffer->capacity = capacity;
	}
	cells = buffer->count * (size_t)buffer->cols;
	fill_cells(&buffer->cells[cells], (size_t)buffer->cols, default_attr());
	buffer->wrapped[buffer->count] = 0;
	buffer->count++;
	buffer->col = 0;
	buffer->need_row = false;
	return true;
}

static bool
reflow_wrap_row(struct ReflowBuffer *buffer)
{
	if (!reflow_start_row(buffer))
		return false;
	buffer->wrapped[buffer->count - 1] = 1;
	buffer->need_row = true;
	return reflow_start_row(buffer);
}

static bool
reflow_put_cell(struct ReflowBuffer *buffer, const struct Cell *source)
{
	struct Cell *cell;
	int width = source->width == 2 ? 2 : 1;

	if (!reflow_start_row(buffer))
		return false;
	if (buffer->col >= buffer->cols || buffer->col + width > buffer->cols)
		if (!reflow_wrap_row(buffer))
			return false;
	cell = &buffer->cells[(buffer->count - 1) * (size_t)buffer->cols +
	                      (size_t)buffer->col];
	*cell = *source;
	cell->width = (unsigned char)width;
	buffer->last_row = buffer->count - 1;
	buffer->last_col = buffer->col;
	buffer->have_last = true;
	if (width == 2) {
		struct Cell *next = cell + 1;
		memset(next, 0, sizeof(*next));
		next->width = 0;
		next->attr = cell->attr;
	}
	buffer->col += width;
	return true;
}

static void
reflow_add_combining(struct ReflowBuffer *buffer, uint32_t cp)
{
	struct Cell *cell;

	if (!buffer->have_last)
		return;
	cell = &buffer->cells[buffer->last_row * (size_t)buffer->cols +
	                      (size_t)buffer->last_col];
	if (cell->ncombining < COMBINING_MAX)
		cell->combining[cell->ncombining++] = cp;
}

static bool
reflow_hard_break(struct ReflowBuffer *buffer)
{
	if (!reflow_start_row(buffer))
		return false;
	buffer->wrapped[buffer->count - 1] = 0;
	buffer->need_row = true;
	buffer->have_last = false;
	return true;
}

static bool
reflow_mark_position(struct ReflowBuffer *buffer, bool wrap_pending,
                     size_t *row, int *col, bool *new_wrap_pending)
{
	if (!reflow_start_row(buffer))
		return false;
	if (buffer->col >= buffer->cols) {
		if (wrap_pending) {
			*row = buffer->count - 1;
			*col = buffer->cols - 1;
			*new_wrap_pending = true;
			return true;
		}
		if (!reflow_wrap_row(buffer))
			return false;
	}
	*row = buffer->count - 1;
	*col = buffer->col;
	*new_wrap_pending = false;
	return true;
}

static bool
reflow_capture_position(struct ReflowBuffer *buffer, bool cursor,
                        bool wrap_pending)
{
	bool ignored;
	bool *new_wrap = cursor ? &buffer->cursor_wrap_pending : &ignored;
	size_t *row = cursor ? &buffer->cursor_row : &buffer->saved_row;
	int *col = cursor ? &buffer->cursor_col : &buffer->saved_col;

	if (!reflow_mark_position(buffer, wrap_pending, row, col, new_wrap))
		return false;
	if (cursor)
		buffer->cursor_set = true;
	else
		buffer->saved_set = true;
	return true;
}

static bool
reflow_feed_history(struct ReflowBuffer *buffer, const struct HistoryLine *line)
{
	struct Attr attr = default_attr();
	size_t offset = 0;
	size_t span = 0;
	int column = 0;

	while (offset < line->length) {
		struct Cell cell;
		uint32_t cp;
		int width;

		if (!decode_utf8(line->text, line->length, &offset, &cp))
			break;
		width = wcwidth((wchar_t)cp);
		if (width == 0) {
			reflow_add_combining(buffer, cp);
			continue;
		}
		if (width < 0 || width > 2)
			width = 1;
		while (span < line->span_count &&
		       line->spans[span].column <= column) {
			attr = line->spans[span].attr;
			span++;
		}
		memset(&cell, 0, sizeof(cell));
		cell.cp = cp;
		cell.width = (unsigned char)width;
		cell.attr = attr;
		if (!reflow_put_cell(buffer, &cell))
			return false;
		column += width;
	}
	return line->wrapped || reflow_hard_break(buffer);
}

static int
screen_row_last(const struct Screen *screen, int row)
{
	int col;
	int last = 0;

	for (col = 0; col < screen->cols; col++) {
		const struct Cell *cell = &screen->cells[(size_t)row *
		                                                (size_t)screen->cols +
		                                                (size_t)col];
		if (cell->width != 0 && !cell_is_empty(cell))
			last = col + cell->width;
	}
	return last;
}

static bool
reflow_feed_screen_row(struct ReflowBuffer *buffer, const struct Screen *screen,
                       int row)
{
	int cursor_mark = -1;
	int saved_mark = -1;
	int used = screen_row_last(screen, row);
	int col = 0;

	if (row == screen->row) {
		cursor_mark = screen->col + (screen->wrap_pending ? 1 : 0);
		if (cursor_mark < screen->cols && cursor_mark > 0 &&
		    screen->cells[(size_t)row * (size_t)screen->cols +
		                  (size_t)cursor_mark].width == 0)
			cursor_mark--;
		if (cursor_mark > used)
			used = cursor_mark;
	}
	if (row == screen->saved_row) {
		saved_mark = screen->saved_col;
		if (saved_mark > 0 &&
		    screen->cells[(size_t)row * (size_t)screen->cols +
		                  (size_t)saved_mark].width == 0)
			saved_mark--;
		if (saved_mark > used)
			used = saved_mark;
	}
	while (col < used) {
		const struct Cell *cell;
		int width;

		if (col == cursor_mark &&
		    !reflow_capture_position(buffer, true, screen->wrap_pending))
			return false;
		if (col == saved_mark &&
		    !reflow_capture_position(buffer, false, false))
			return false;
		cell = &screen->cells[(size_t)row * (size_t)screen->cols +
		                      (size_t)col];
		if (cell->width == 0) {
			col++;
			continue;
		}
		width = cell->width == 2 ? 2 : 1;
		if (!reflow_put_cell(buffer, cell))
			return false;
		col += width;
	}
	if (cursor_mark == used &&
	    !reflow_capture_position(buffer, true, screen->wrap_pending))
		return false;
	if (saved_mark == used &&
	    !reflow_capture_position(buffer, false, false))
		return false;
	return screen->wrapped[row] != 0 || reflow_hard_break(buffer);
}

static bool
reflow_primary(struct Terminal *terminal, int rows, int cols)
{
	struct Screen *screen = &terminal->primary;
	struct ReflowBuffer buffer = {0};
	struct Cell *new_cells;
	unsigned char *new_dirty;
	unsigned char *new_wrapped;
	size_t old_scroll_offset = terminal->scroll_offset;
	size_t start;
	size_t i;
	int r;

	if (rows == screen->rows && cols == screen->cols)
		return true;
	buffer.cols = cols;
	buffer.need_row = true;
	for (i = 0; i < terminal->history.count; i++)
		if (!reflow_feed_history(&buffer, history_line(&terminal->history, i)))
			goto fail;
	for (r = 0; r < screen->rows; r++)
		if (!reflow_feed_screen_row(&buffer, screen, r))
			goto fail;
	if (!buffer.cursor_set || !buffer.saved_set || buffer.count == 0)
		goto fail;
	start = buffer.count > (size_t)rows ? buffer.count - (size_t)rows : 0;
	if (buffer.cursor_row < start)
		start = buffer.cursor_row;
	else if (buffer.cursor_row >= start + (size_t)rows)
		start = buffer.cursor_row - (size_t)rows + 1;
	if (!allocate_screen(rows, cols, &new_cells, &new_dirty, &new_wrapped))
		goto fail;
	fill_cells(new_cells, (size_t)rows * (size_t)cols, default_attr());
	for (r = 0; r < rows && start + (size_t)r < buffer.count; r++) {
		memcpy(&new_cells[(size_t)r * (size_t)cols],
		       &buffer.cells[(start + (size_t)r) * (size_t)cols],
		       (size_t)cols * sizeof(*new_cells));
		new_wrapped[r] = buffer.wrapped[start + (size_t)r];
	}
	memset(new_dirty, 1, (size_t)rows);
	replace_screen(screen, new_cells, new_dirty, new_wrapped, rows, cols);
	screen->row = (int)(buffer.cursor_row - start);
	screen->col = buffer.cursor_col;
	screen->wrap_pending = buffer.cursor_wrap_pending;
	if (buffer.saved_row < start) {
		screen->saved_row = 0;
		screen->saved_col = 0;
	} else if (buffer.saved_row >= start + (size_t)rows) {
		screen->saved_row = rows - 1;
		screen->saved_col = cols - 1;
	} else {
		screen->saved_row = (int)(buffer.saved_row - start);
		screen->saved_col = buffer.saved_col;
	}
	screen->scroll_top = 0;
	screen->scroll_bottom = rows - 1;
	history_clear(terminal);
	for (i = 0; i < start; i++)
		history_push(terminal, &buffer.cells[i * (size_t)cols], cols,
		             buffer.wrapped[i] != 0);
	terminal->scroll_offset = old_scroll_offset < terminal->history.count ?
	                          old_scroll_offset : terminal->history.count;
	terminal->viewport_dirty = true;
	reflow_buffer_free(&buffer);
	return true;

fail:
	reflow_buffer_free(&buffer);
	return false;
}

static void
append_attr(struct Buffer *output, struct Attr attr)
{
	const int colors[] = {attr.fg, attr.bg};
	const int selectors[] = {38, 48};
	size_t i;

	(void)output_append(output, "\033[0");
	for (i = 0; i < LEN(rendition_flags); i++)
		if ((attr.flags & rendition_flags[i].flag) != 0)
			(void)output_printf(output, ";%d", rendition_flags[i].set);
	for (i = 0; i < LEN(colors); i++)
		if (colors[i] >= 0) {
			if ((colors[i] & 0x1000000) != 0)
				(void)output_printf(output, ";%d;2;%d;%d;%d", selectors[i],
				                    (colors[i] >> 16) & 255,
				                    (colors[i] >> 8) & 255, colors[i] & 255);
			else
				(void)output_printf(output, ";%d;5;%d", selectors[i], colors[i]);
		}
	(void)output_append(output, "m");
}

static void
append_screen_cells(struct Buffer *output, struct Screen *screen, int source_row,
                    struct Attr *previous)
{
	int col;

	for (col = 0; col < screen->cols; col++) {
		struct Cell *cell = cell_at(screen, source_row, col);
		unsigned int i;
		if (cell->width == 0)
			continue;
		if (!attr_equal(*previous, cell->attr)) {
			append_attr(output, cell->attr);
			*previous = cell->attr;
		}
		append_codepoint(output, cell->cp == 0 ? ' ' : cell->cp);
		for (i = 0; i < cell->ncombining; i++)
			append_codepoint(output, cell->combining[i]);
	}
}

static bool
decode_utf8(const char *text, size_t length, size_t *offset, uint32_t *codepoint)
{
	const unsigned char *bytes = (const unsigned char *)text;
	unsigned char first;
	uint32_t cp;
	size_t need;
	size_t i;

	if (*offset >= length)
		return false;
	first = bytes[*offset];
	if (first < 0x80) {
		*codepoint = first;
		(*offset)++;
		return true;
	}
	if ((first & 0xe0u) == 0xc0u) {
		cp = first & 0x1fu;
		need = 1;
	} else if ((first & 0xf0u) == 0xe0u) {
		cp = first & 0x0fu;
		need = 2;
	} else if ((first & 0xf8u) == 0xf0u) {
		cp = first & 0x07u;
		need = 3;
	} else {
		*codepoint = 0xfffdu;
		(*offset)++;
		return true;
	}
	if (*offset + need >= length) {
		*codepoint = 0xfffdu;
		(*offset)++;
		return true;
	}
	for (i = 1; i <= need; i++) {
		unsigned char byte = bytes[*offset + i];
		if ((byte & 0xc0u) != 0x80u) {
			*codepoint = 0xfffdu;
			(*offset)++;
			return true;
		}
		cp = (cp << 6) | (uint32_t)(byte & 0x3fu);
	}
	*offset += need + 1;
	*codepoint = cp;
	return true;
}

static void
append_history_cells(struct Buffer *output, const struct HistoryLine *line,
                     int columns, struct Attr *previous)
{
	struct Attr current = default_attr();
	size_t offset = 0;
	size_t span = 0;
	int column = 0;

	if (line != NULL) {
		while (offset < line->length && column < columns) {
			size_t start = offset;
			uint32_t cp;
			int width;
			if (!decode_utf8(line->text, line->length, &offset, &cp))
				break;
			width = wcwidth((wchar_t)cp);
			if (width < 0)
				width = 1;
			if (width > 0)
				while (span < line->span_count &&
				       line->spans[span].column <= column) {
					current = line->spans[span].attr;
					span++;
				}
			if (width > 0 && column + width > columns)
				break;
			if (!attr_equal(*previous, current)) {
				append_attr(output, current);
				*previous = current;
			}
			(void)buffer_append(output, line->text + start, offset - start);
			column += width;
		}
	}
	current = default_attr();
	if (column < columns && !attr_equal(*previous, current)) {
		append_attr(output, current);
		*previous = current;
	}
	while (column < columns) {
		(void)buffer_append(output, " ", 1);
		column++;
	}
}

static const struct HistoryLine *
viewport_history_line(struct Terminal *terminal, size_t first, int display_row,
                      int *screen_row)
{
	struct Screen *screen = terminal->screen;
	size_t line = first + (size_t)display_row;

	*screen_row = -1;
	if (line < terminal->history.count)
		return history_line(&terminal->history, line);
	line -= terminal->history.count;
	if (line < (size_t)screen->rows)
		*screen_row = (int)line;
	return NULL;
}

static void
append_screen_rows(struct Buffer *output, struct Screen *screen, int first,
                   int last)
{
	struct Attr previous = {0, 0, UINT32_MAX};
	int row;

	(void)output_printf(output, "\033[%d;1H", first + 1);
	for (row = first; row <= last; row++) {
		append_screen_cells(output, screen, row, &previous);
		if (row < last && screen->wrapped[row] == 0)
			(void)output_append(output, "\r\n");
	}
	(void)output_append(output, "\033[0m");
	if (last + 1 < host_rows && screen->wrapped[last] == 0)
		(void)output_append(output, "\r\n");
}

static void
append_viewport(struct Buffer *output, struct Terminal *terminal)
{
	struct Screen *screen = terminal->screen;
	struct Attr previous = {0, 0, UINT32_MAX};
	size_t first = terminal->history.count - terminal->scroll_offset;
	bool wrapped = false;
	int display_row;

	(void)output_append(output, "\033[1;1H");
	for (display_row = 0; display_row < screen->rows; display_row++) {
		int screen_row;
		const struct HistoryLine *line = viewport_history_line(terminal, first,
		                                                        display_row,
		                                                        &screen_row);
		if (screen_row >= 0) {
			append_screen_cells(output, screen, screen_row, &previous);
			wrapped = screen->wrapped[screen_row] != 0;
		} else {
			append_history_cells(output, line, screen->cols, &previous);
			wrapped = line != NULL && line->wrapped;
		}
		if (display_row + 1 < screen->rows && !wrapped)
			(void)output_append(output, "\r\n");
	}
	(void)output_append(output, "\033[0m");
	if (screen->rows < host_rows && !wrapped)
		(void)output_append(output, "\r\n");
}

static const char *
key_text(unsigned char key, char text[4])
{
	if (key < 32) {
		text[0] = '^';
		text[1] = (char)(key + '@');
		text[2] = '\0';
		return text;
	}
	if (key == 127)
		return "^?";
	if (isprint(key)) {
		text[0] = (char)key;
		text[1] = '\0';
		return text;
	}
	return "?";
}

static void
append_bar(struct Buffer *output, const char *text, size_t length)
{
	if ((int)length > host_cols)
		length = (size_t)host_cols;
	(void)output_printf(output, "\033[%d;1H\033[0;7m", host_rows);
	(void)buffer_append(output, text, length);
	while ((int)length < host_cols) {
		(void)buffer_append(output, " ", 1);
		length++;
	}
	(void)output_append(output, "\033[0m");
}

static void
append_status(struct Buffer *output)
{
	struct Buffer status = {0};
	size_t i;

	if (!status_enabled || host_rows < 2)
		return;
	if (prefix_pending) {
		char key[4];
		output_printf(&status, " prefix %s: waiting for key",
		              key_text(command_prefix, key));
	} else if (windows[active_window]->scroll_offset != 0) {
		const char *title = windows[active_window]->title[0] == '\0' ?
		                    "shell" : windows[active_window]->title;
		output_printf(&status,
		              "[%zu:%s] scroll %zu/%zu  ^U/^D page  k/j line  g/G oldest/live  Esc exit",
		              active_window + 1, title,
		              windows[active_window]->scroll_offset,
		              windows[active_window]->history.count);
	} else for (i = 0; i < window_count; i++) {
		const char *title = windows[i]->title[0] == '\0' ? "shell" : windows[i]->title;
		output_printf(&status, "%s%s%zu:%s%s",
		              i == 0 ? "" : "  ", i == active_window ? "[" : "",
		              i + 1, title, i == active_window ? "]" :
		              (windows[i]->unread ? "*" : ""));
		if (status.length >= (size_t)host_cols)
			break;
	}
	append_bar(output, (const char *)status.data, status.length);
	buffer_free(&status);
}

static void
append_help(struct Buffer *output)
{
	char help[256];
	char key[4];
	const char *prefix_text;
	size_t length;

	prefix_text = key_text(command_prefix, key);
	(void)snprintf(help, sizeof(help),
	               " %s m:new  o:last  ^N/^P:next/prev  1-0:select  ^X:close  ^L:redraw  ^U:scroll  %s:send ",
	               prefix_text, prefix_text);
	length = strlen(help);

	append_bar(output, help, length);
}

static void
append_host_modes(struct Buffer *output, struct Terminal *terminal)
{
	static struct Terminal *previous;
	static bool app_cursor;
	static bool app_keypad;
	static bool paste;
	static bool focus;
	static int mouse;
	static bool mouse_sgr;
	bool force = terminal != previous;

	if (force || app_cursor != terminal->app_cursor)
		(void)output_append(output, terminal->app_cursor ? "\033[?1h" : "\033[?1l");
	if (force || app_keypad != terminal->app_keypad)
		(void)output_append(output, terminal->app_keypad ? "\033=" : "\033>");
	if (force || paste != terminal->bracketed_paste)
		(void)output_append(output, terminal->bracketed_paste ? "\033[?2004h" : "\033[?2004l");
	if (force || focus != terminal->focus_events)
		(void)output_append(output, terminal->focus_events ? "\033[?1004h" : "\033[?1004l");
	if (force || mouse != terminal->mouse_mode || mouse_sgr != terminal->mouse_sgr) {
		(void)output_append(output, "\033[?1000l\033[?1002l\033[?1003l\033[?1006l");
		if (terminal->mouse_mode != 0)
			(void)output_printf(output, "\033[?%dh", terminal->mouse_mode);
		if (terminal->mouse_sgr)
			(void)output_append(output, "\033[?1006h");
	}
	previous = terminal;
	app_cursor = terminal->app_cursor;
	app_keypad = terminal->app_keypad;
	paste = terminal->bracketed_paste;
	focus = terminal->focus_events;
	mouse = terminal->mouse_mode;
	mouse_sgr = terminal->mouse_sgr;
}

static void
write_all(int fd, const unsigned char *data, size_t length)
{
	while (length != 0) {
		ssize_t written = write(fd, data, length);
		if (written > 0) {
			data += (size_t)written;
			length -= (size_t)written;
		} else if (written < 0 && errno == EINTR) {
			continue;
		} else {
			break;
		}
	}
}

static void
render(bool full)
{
	struct Terminal *terminal;
	struct Screen *screen;
	struct Buffer output = {0};
	int row;

	if (window_count == 0)
		return;
	terminal = windows[active_window];
	screen = terminal->screen;
	if (full) {
		(void)output_append(&output, "\033[?25l\033[0m\033[H\033[2J");
		mark_all_dirty(screen);
	}
	append_host_modes(&output, terminal);
	if (terminal->scroll_offset != 0) {
		if (full || terminal->viewport_dirty) {
			append_viewport(&output, terminal);
			terminal->viewport_dirty = false;
		}
	} else {
		for (row = 0; row < screen->rows;) {
			int first;
			int last;
			int current;

			if (!screen->dirty[row]) {
				row++;
				continue;
			}
			first = row;
			while (first > 0 && screen->wrapped[first - 1] != 0)
				first--;
			last = row;
			while (last + 1 < screen->rows && screen->wrapped[last] != 0)
				last++;
			append_screen_rows(&output, screen, first, last);
			for (current = first; current <= last; current++)
				screen->dirty[current] = 0;
			row = last + 1;
		}
	}
	append_status(&output);
	if (help_visible)
		append_help(&output);
	if (screen->cursor_visible && !help_visible && terminal->scroll_offset == 0) {
		(void)output_printf(&output, "\033[%d;%dH\033[?25h",
		                    screen->row + 1, screen->col + 1);
	} else {
		(void)output_append(&output, "\033[?25l");
	}
	write_all(STDOUT_FILENO, output.data, output.length);
	buffer_free(&output);
}

static void
make_raw(struct termios *termios)
{
	termios->c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	termios->c_oflag &= (tcflag_t)~OPOST;
	termios->c_cflag |= CS8;
	termios->c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
	termios->c_cc[VMIN] = 1;
	termios->c_cc[VTIME] = 0;
}

static void
enter_terminal(void)
{
	struct termios raw;

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
		fatal("standard input and output must be terminals");
	if (tcgetattr(STDIN_FILENO, &original_termios) < 0)
		fatal("tcgetattr");
	raw = original_termios;
	make_raw(&raw);
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
		fatal("tcsetattr");
	terminal_is_raw = true;
	write_all(STDOUT_FILENO,
	          (const unsigned char *)"\033[?1049h\033[?7h\033[?25l\033[0m\033[H\033[2J",
	          strlen("\033[?1049h\033[?7h\033[?25l\033[0m\033[H\033[2J"));
}

static void
leave_terminal(void)
{
	const char *reset = "\033[?1000l\033[?1002l\033[?1003l\033[?1004l\033[?1006l"
	                    "\033[?2004l\033[?1l\033>\033[0m\033[?25h\033[?1049l";
	write_all(STDOUT_FILENO, (const unsigned char *)reset, strlen(reset));
	if (terminal_is_raw) {
		(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
		terminal_is_raw = false;
	}
}

static int
set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	flags = fcntl(fd, F_GETFD);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
	return 0;
}

static const char *
shell_path(void)
{
	const char *shell = getenv("SHELL");
	struct passwd *password;

	if (shell != NULL && *shell != '\0')
		return shell;
	password = getpwuid(getuid());
	if (password != NULL && password->pw_shell != NULL && *password->pw_shell != '\0')
		return password->pw_shell;
	return "/bin/sh";
}

static const char *
base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash == NULL ? path : slash + 1;
}

static struct Terminal *
spawn_terminal(char *const argv[])
{
	struct Terminal *terminal;
	struct winsize size;
	char *slave_name;
	int master;
	int slave;
	pid_t pid;

	master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
		if (master >= 0)
			close(master);
		return NULL;
	}
	slave_name = ptsname(master);
	if (slave_name == NULL) {
		close(master);
		return NULL;
	}
	slave = open(slave_name, O_RDWR | O_NOCTTY);
	if (slave < 0) {
		close(master);
		return NULL;
	}
	size.ws_row = (unsigned short)(host_rows - (status_enabled && host_rows > 1 ? 1 : 0));
	size.ws_col = (unsigned short)host_cols;
	size.ws_xpixel = 0;
	size.ws_ypixel = 0;
	(void)tcsetattr(slave, TCSANOW, &original_termios);
	(void)ioctl(slave, TIOCSWINSZ, &size);
	pid = fork();
	if (pid < 0) {
		close(slave);
		close(master);
		return NULL;
	}
	if (pid == 0) {
		struct sigaction action;
		const int signals[] = {SIGCHLD, SIGWINCH, SIGINT,
		                       SIGTERM, SIGHUP, SIGQUIT};
		size_t i;

		close(master);
		if (setsid() < 0)
			_exit(126);
		(void)ioctl(slave, TIOCSCTTY, 0);
		if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
		    dup2(slave, STDERR_FILENO) < 0)
			_exit(126);
		if (slave > STDERR_FILENO)
			close(slave);
		(void)tcsetpgrp(STDIN_FILENO, getpid());
		memset(&action, 0, sizeof(action));
		action.sa_handler = SIG_DFL;
		(void)sigemptyset(&action.sa_mask);
		for (i = 0; i < LEN(signals); i++)
			(void)sigaction(signals[i], &action, NULL);
		(void)setenv("TERM", CHILD_TERM, 1);
		(void)setenv("MWIN", "1", 1);
		execvp(argv[0], argv);
		(void)dprintf(STDERR_FILENO, "mwin: cannot execute %s: %s\r\n",
		              argv[0], strerror(errno));
		_exit(127);
	}
	close(slave);
	if (set_nonblocking(master) < 0) {
		close(master);
		(void)kill(pid, SIGHUP);
		return NULL;
	}
	terminal = calloc(1, sizeof(*terminal));
	if (terminal == NULL) {
		close(master);
		(void)kill(pid, SIGHUP);
		return NULL;
	}
	terminal->fd = master;
	terminal->pid = pid;
	terminal->parser.state = P_GROUND;
	terminal->history.capacity = scrollback_limit;
	(void)snprintf(terminal->title, sizeof(terminal->title), "%s", base_name(argv[0]));
	if (!screen_init(&terminal->primary, size.ws_row, size.ws_col) ||
	    !screen_init(&terminal->alternate, size.ws_row, size.ws_col)) {
		screen_free(&terminal->primary);
		screen_free(&terminal->alternate);
		buffer_free(&terminal->input);
		free(terminal);
		close(master);
		(void)kill(pid, SIGHUP);
		return NULL;
	}
	terminal->primary.terminal = terminal;
	terminal->screen = &terminal->primary;
	return terminal;
}

static bool
add_window(char *const argv[])
{
	struct Terminal *terminal;

	if (window_count >= MAX_WINDOWS)
		return false;
	terminal = spawn_terminal(argv);
	if (terminal == NULL)
		return false;
	if (window_count != 0)
		previous_window = windows[active_window];
	windows[window_count++] = terminal;
	active_window = window_count - 1;
	terminal->unread = false;
	help_visible = false;
	render(true);
	return true;
}

static void
free_terminal(struct Terminal *terminal, bool terminate)
{
	if (terminate && terminal->pid > 0) {
		if (kill(-terminal->pid, SIGHUP) < 0)
			(void)kill(terminal->pid, SIGHUP);
	}
	close(terminal->fd);
	screen_free(&terminal->primary);
	screen_free(&terminal->alternate);
	history_free(terminal);
	buffer_free(&terminal->input);
	free(terminal);
}

static void
remove_window(size_t index, bool terminate)
{
	struct Terminal *removed;
	size_t i;

	if (index >= window_count)
		return;
	removed = windows[index];
	if (previous_window == removed)
		previous_window = NULL;
	free_terminal(removed, terminate);
	for (i = index; i + 1 < window_count; i++)
		windows[i] = windows[i + 1];
	window_count--;
	if (window_count == 0) {
		running = false;
		return;
	}
	if (active_window > index)
		active_window--;
	else if (active_window >= window_count)
		active_window = window_count - 1;
	if (previous_window == windows[active_window])
		previous_window = NULL;
	windows[active_window]->unread = false;
	help_visible = false;
	render(true);
}

static ssize_t
find_window(struct Terminal *terminal)
{
	size_t i;

	for (i = 0; i < window_count; i++)
		if (windows[i] == terminal)
			return (ssize_t)i;
	return -1;
}

static bool
activate_window(size_t index)
{
	if (index >= window_count || index == active_window)
		return false;
	previous_window = windows[active_window];
	active_window = index;
	windows[index]->unread = false;
	help_visible = false;
	return true;
}

static void
select_window(size_t index)
{
	if (activate_window(index))
		render(true);
}

static bool
activate_previous_window(void)
{
	ssize_t index;

	if (previous_window == NULL)
		return false;
	index = find_window(previous_window);
	if (index < 0) {
		previous_window = NULL;
		return false;
	}
	return activate_window((size_t)index);
}

static void
select_previous_window(void)
{
	if (activate_previous_window())
		render(true);
}

static void
new_shell(void)
{
	char *arguments[] = {(char *)shell_path(), NULL};

	if (!add_window(arguments)) {
		ssize_t ignored = write(STDOUT_FILENO, "\a", 1);
		(void)ignored;
	}
}

static size_t
scroll_page(const struct Terminal *terminal)
{
	return terminal->screen->rows > 1 ? (size_t)(terminal->screen->rows - 1) : 1;
}

static void
scroll_backward(struct Terminal *terminal, size_t amount)
{
	if (terminal->screen != &terminal->primary || terminal->history.count == 0)
		return;
	if (amount > terminal->history.count - terminal->scroll_offset)
		terminal->scroll_offset = terminal->history.count;
	else
		terminal->scroll_offset += amount;
	terminal->viewport_dirty = true;
	render(true);
}

static void
scroll_to_live(struct Terminal *terminal)
{
	bool changed = terminal->scroll_offset != 0;

	terminal->scroll_offset = 0;
	terminal->viewport_dirty = false;
	if (changed)
		render(true);
}

static void
scroll_forward(struct Terminal *terminal, size_t amount)
{
	if (terminal->scroll_offset == 0)
		return;
	if (amount >= terminal->scroll_offset) {
		scroll_to_live(terminal);
		return;
	}
	terminal->scroll_offset -= amount;
	terminal->viewport_dirty = true;
	render(true);
}

static void
handle_scrollback_key(struct Terminal *terminal, unsigned char byte)
{
	switch (byte) {
	case MWIN_CTRL('u'):
		scroll_backward(terminal, scroll_page(terminal));
		break;
	case MWIN_CTRL('d'):
		scroll_forward(terminal, scroll_page(terminal));
		break;
	case 'k':
		scroll_backward(terminal, 1);
		break;
	case 'j':
		scroll_forward(terminal, 1);
		break;
	case 'g':
		scroll_backward(terminal, terminal->history.count);
		break;
	case 'G':
	case '\033':
		scroll_to_live(terminal);
		break;
	default:
		break;
	}
}

static void
handle_command(unsigned char byte)
{
	size_t target;

	if (byte == command_prefix) {
		(void)buffer_append(&windows[active_window]->input, &byte, 1);
		return;
	}
	switch (byte) {
	case 'm':
		new_shell();
		break;
	case 'o':
		select_previous_window();
		break;
	case MWIN_CTRL('n'):
		select_window((active_window + 1) % window_count);
		break;
	case MWIN_CTRL('p'):
		select_window((active_window + window_count - 1) % window_count);
		break;
	case MWIN_CTRL('x'):
		remove_window(active_window, true);
		break;
	case MWIN_CTRL('l'):
		help_visible = false;
		render(true);
		break;
	case MWIN_CTRL('u'):
		scroll_backward(windows[active_window], scroll_page(windows[active_window]));
		break;
	case '?':
		help_visible = true;
		render(false);
		break;
	case '0':
		target = 9;
		select_window(target);
		break;
	default:
		if (byte >= '1' && byte <= '9') {
			target = (size_t)(byte - '1');
			select_window(target);
		}
		break;
	}
}

static void
handle_regular_input(unsigned char byte)
{
	if (help_visible) {
		help_visible = false;
		render(true);
		return;
	}
	if (windows[active_window]->scroll_offset != 0) {
		handle_scrollback_key(windows[active_window], byte);
		return;
	}
	if (prefix_pending) {
		prefix_pending = false;
		handle_command(byte);
		if (status_enabled)
			render(false);
	} else if (byte == command_prefix) {
		prefix_pending = true;
		if (status_enabled)
			render(false);
	} else {
		(void)buffer_append(&windows[active_window]->input, &byte, 1);
	}
}

static void
observe_paste_byte(unsigned char byte)
{
	const unsigned char *wanted = input_is_paste ? paste_end : paste_begin;
	const size_t wanted_length = sizeof(paste_begin) - 1;

	if (byte == wanted[paste_match_length])
		paste_match_length++;
	else
		paste_match_length = byte == wanted[0] ? 1 : 0;
	if (paste_match_length == wanted_length) {
		input_is_paste = !input_is_paste;
		paste_match_length = 0;
	}
}

static void
handle_live_input(unsigned char byte)
{
	if (input_is_paste)
		(void)buffer_append(&windows[active_window]->input, &byte, 1);
	else
		handle_regular_input(byte);
	observe_paste_byte(byte);
}

static void
flush_scroll_escape(void)
{
	size_t i;

	/* The first Escape already served as the scrollback-exit command. */
	for (i = 1; i < scroll_escape_length && running; i++)
		handle_live_input(scroll_escape_sequence[i]);
	scroll_escape_length = 0;
}

static void
handle_input(const unsigned char *data, size_t length)
{
	size_t i;

	for (i = 0; i < length && running; i++) {
		unsigned char byte = data[i];

		if (scroll_escape_length != 0) {
			scroll_escape_sequence[scroll_escape_length++] = byte;
			if (memcmp(scroll_escape_sequence, paste_begin,
			           scroll_escape_length) != 0) {
				flush_scroll_escape();
			} else if (scroll_escape_length == sizeof(paste_begin) - 1) {
				(void)buffer_append(&windows[active_window]->input,
				                    scroll_escape_sequence, scroll_escape_length);
				scroll_escape_length = 0;
				paste_match_length = 0;
				input_is_paste = true;
			}
			continue;
		}
		if (!input_is_paste && byte == '\033' &&
		    windows[active_window]->scroll_offset != 0) {
			handle_regular_input(byte);
			scroll_escape_sequence[0] = byte;
			scroll_escape_length = 1;
			paste_match_length = 0;
			continue;
		}
		handle_live_input(byte);
	}
}

static void
flush_input(struct Terminal *terminal)
{
	while (terminal->input.offset < terminal->input.length) {
		ssize_t written = write(terminal->fd,
		                        terminal->input.data + terminal->input.offset,
		                        terminal->input.length - terminal->input.offset);
		if (written > 0) {
			terminal->input.offset += (size_t)written;
		} else if (written < 0 && errno == EINTR) {
			continue;
		} else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return;
		} else {
			return;
		}
	}
	terminal->input.offset = 0;
	terminal->input.length = 0;
}

static bool
read_terminal(struct Terminal *terminal)
{
	unsigned char data[INPUT_CHUNK];
	bool received = false;

	for (;;) {
		ssize_t length = read(terminal->fd, data, sizeof(data));
		if (length > 0) {
			feed_bytes(terminal, data, (size_t)length);
			received = true;
		} else if (length == 0) {
			return false;
		} else if (errno == EINTR) {
			continue;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			break;
		} else if (errno == EIO) {
			return false;
		} else {
			return false;
		}
	}
	if (received) {
		if (window_count == 0 || windows[active_window] != terminal)
			terminal->unread = true;
		render(false);
	}
	return true;
}

static void
resize_all(void)
{
	struct winsize size;
	int rows;
	int cols;
	size_t i;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
		if (size.ws_row != 0)
			host_rows = size.ws_row;
		if (size.ws_col != 0)
			host_cols = size.ws_col;
	}
	rows = host_rows - (status_enabled && host_rows > 1 ? 1 : 0);
	cols = host_cols;
	if (rows < 1)
		rows = 1;
	if (cols < 1)
		cols = 1;
	size.ws_row = (unsigned short)rows;
	size.ws_col = (unsigned short)cols;
	size.ws_xpixel = 0;
	size.ws_ypixel = 0;
	for (i = 0; i < window_count; i++) {
		if (!reflow_primary(windows[i], rows, cols))
			screen_resize(&windows[i]->primary, rows, cols);
		screen_resize(&windows[i]->alternate, rows, cols);
		(void)ioctl(windows[i]->fd, TIOCSWINSZ, &size);
	}
	render(true);
}

static void
signal_handler(int signal_number)
{
	unsigned char byte = (unsigned char)signal_number;
	ssize_t ignored;
	if (signal_pipe[1] >= 0) {
		ignored = write(signal_pipe[1], &byte, 1);
		(void)ignored;
	}
}

static void
install_signals(void)
{
	struct sigaction action;
	int signals[] = {SIGWINCH, SIGCHLD, SIGINT, SIGTERM, SIGHUP, SIGQUIT};
	size_t i;

	if (pipe(signal_pipe) < 0)
		fatal("pipe");
	if (set_nonblocking(signal_pipe[0]) < 0 || set_nonblocking(signal_pipe[1]) < 0)
		fatal("fcntl");
	memset(&action, 0, sizeof(action));
	action.sa_handler = signal_handler;
	(void)sigemptyset(&action.sa_mask);
	for (i = 0; i < LEN(signals); i++)
		if (sigaction(signals[i], &action, NULL) < 0)
			fatal("sigaction");
}

static void
reap_children(void)
{
	pid_t pid;
	int status;
	size_t i;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
		for (i = 0; i < window_count; i++)
			if (windows[i]->pid == pid) {
				windows[i]->pid = -1;
				break;
			}
}

static void
handle_signals(void)
{
	unsigned char data[64];
	ssize_t length;
	bool resized = false;
	bool child = false;
	bool stop = false;
	ssize_t i;

	do {
		length = read(signal_pipe[0], data, sizeof(data));
		if (length > 0) {
			for (i = 0; i < length; i++) {
				if (data[i] == SIGWINCH)
					resized = true;
				else if (data[i] == SIGCHLD)
					child = true;
				else
					stop = true;
			}
		}
	} while (length > 0);
	if (child)
		reap_children();
	if (resized)
		resize_all();
	if (stop)
		running = false;
}

static void
event_loop(void)
{
	struct pollfd descriptors[MAX_WINDOWS + 2];
	struct Terminal *polled[MAX_WINDOWS];
	unsigned char input[INPUT_CHUNK];

	while (running && window_count != 0) {
		nfds_t count = 0;
		size_t i;
		int result;

		descriptors[count].fd = STDIN_FILENO;
		descriptors[count].events = POLLIN;
		descriptors[count++].revents = 0;
		descriptors[count].fd = signal_pipe[0];
		descriptors[count].events = POLLIN;
		descriptors[count++].revents = 0;
		for (i = 0; i < window_count; i++) {
			polled[i] = windows[i];
			descriptors[count].fd = windows[i]->fd;
			descriptors[count].events = POLLIN;
			if (windows[i]->input.offset < windows[i]->input.length)
				descriptors[count].events |= POLLOUT;
			descriptors[count++].revents = 0;
		}
		result = poll(descriptors, count, scroll_escape_length == 0 ? -1 : 30);
		if (result < 0) {
			if (errno == EINTR) {
				handle_signals();
				continue;
			}
			break;
		}
		if (result == 0) {
			flush_scroll_escape();
			continue;
		}
		if ((descriptors[1].revents & POLLIN) != 0)
			handle_signals();
		if (!running || window_count == 0)
			break;
		if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
			ssize_t length = read(STDIN_FILENO, input, sizeof(input));
			if (length > 0)
				handle_input(input, (size_t)length);
			else if (length == 0)
				running = false;
		}
		for (i = 2; i < (size_t)count && running && window_count != 0; i++) {
			struct Terminal *terminal = polled[i - 2];
			ssize_t index = find_window(terminal);
			if (index < 0)
				continue;
			if ((descriptors[i].revents & POLLOUT) != 0)
				flush_input(terminal);
			if ((descriptors[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0 &&
			    !read_terminal(terminal)) {
				index = find_window(terminal);
				if (index >= 0)
					remove_window((size_t)index, false);
			}
		}
	}
}

static void
cleanup(void)
{
	while (window_count != 0)
		remove_window(window_count - 1, true);
	if (signal_pipe[0] >= 0)
		close(signal_pipe[0]);
	if (signal_pipe[1] >= 0)
		close(signal_pipe[1]);
	leave_terminal();
}

static void
fatal(const char *message)
{
	int saved_errno = errno;
	leave_terminal();
	errno = saved_errno;
	perror(message);
	exit(1);
}

static unsigned char
parse_key(const char *text)
{
	char *end;
	long value;

	if (text[0] == '^' && text[1] != '\0' && text[2] == '\0')
		return MWIN_CTRL((unsigned char)toupper((unsigned char)text[1]));
	if (text[0] != '\0' && text[1] == '\0')
		return (unsigned char)text[0];
	errno = 0;
	value = strtol(text, &end, 10);
	if (errno == 0 && *end == '\0' && value >= 0 && value <= 255)
		return (unsigned char)value;
	fprintf(stderr, "mwin: invalid command key: %s\n", text);
	exit(2);
}

static size_t
scrollback_from_environment(void)
{
	const char *text = getenv("MWIN_SCROLLBACK");
	const char *cursor;
	char *end;
	unsigned long long value;

	if (text == NULL)
		return DEFAULT_SCROLLBACK;
	if (text[0] == '\0')
		goto invalid;
	for (cursor = text; *cursor != '\0'; cursor++)
		if (!isdigit((unsigned char)*cursor))
			goto invalid;
	errno = 0;
	value = strtoull(text, &end, 10);
	if (*end != '\0' || errno == ERANGE || value > SIZE_MAX)
		goto invalid;
	return (size_t)value;

invalid:
	fprintf(stderr, "mwin: invalid MWIN_SCROLLBACK value: %s\n", text);
	exit(2);
}


static void
usage(FILE *stream)
{
	fprintf(stream,
	        "usage: mwin [-s] [-c key] [command [argument ...]]\n"
	        "       mwin -h | -v\n");
}

int
main(int argc, char **argv)
{
	char *initial_shell[2];
	char **initial_command;
	int option;

	(void)setlocale(LC_CTYPE, "");
	if (argc == 2 && strcmp(argv[1], "--help") == 0) {
		usage(stdout);
		return 0;
	}
	while ((option = getopt(argc, argv, "+c:shv")) != -1) {
		switch (option) {
		case 'c':
			command_prefix = parse_key(optarg);
			break;
		case 's':
			status_enabled = false;
			break;
		case 'h':
			usage(stdout);
			return 0;
		case 'v':
			printf("mwin %s\n", VERSION);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (command_prefix == 0) {
		fprintf(stderr, "mwin: NUL cannot be used as the command key\n");
		return 2;
	}
	scrollback_limit = scrollback_from_environment();
	enter_terminal();
	install_signals();
	resize_all();
	if (optind < argc) {
		initial_command = &argv[optind];
	} else {
		initial_shell[0] = (char *)shell_path();
		initial_shell[1] = NULL;
		initial_command = initial_shell;
	}
	if (!add_window(initial_command))
		fatal("cannot create terminal");
	event_loop();
	cleanup();
	reap_children();
	return 0;
}

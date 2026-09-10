#define main mwin_program_main
#include "../mwin.c"
#undef main

#define TEST_CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "model-test: %s:%d: %s\n", \
		        __func__, __LINE__, #condition); \
		failed = 1; \
	} \
} while (0)

static bool
row_starts_with(const struct Screen *screen, int row, const char *text)
{
	int col;

	for (col = 0; text[col] != '\0'; col++) {
		if (col >= screen->cols ||
		    screen->cells[(size_t)row * (size_t)screen->cols +
		                  (size_t)col].cp != (unsigned char)text[col])
			return false;
	}
	return true;
}

static bool
init_test_terminal(struct Terminal *terminal, int rows, int cols,
                   size_t history_capacity)
{
	memset(terminal, 0, sizeof(*terminal));
	terminal->history.capacity = history_capacity;
	if (!screen_init(&terminal->primary, rows, cols) ||
	    !screen_init(&terminal->alternate, rows, cols)) {
		screen_free(&terminal->primary);
		screen_free(&terminal->alternate);
		return false;
	}
	terminal->primary.terminal = terminal;
	terminal->screen = &terminal->primary;
	terminal->parser.state = P_GROUND;
	return true;
}

static void
free_test_terminal(struct Terminal *terminal)
{
	history_free(terminal);
	buffer_free(&terminal->input);
	screen_free(&terminal->primary);
	screen_free(&terminal->alternate);
}

static int
test_vt_commands(void)
{
	struct Terminal terminal;
	struct Screen *screen;
	struct Cell *cell;
	const unsigned char edits[] = "abcd\033[2D\033[@X\033[2G\033[P";
	const unsigned char lines[] = "111\r\n222\r\n333\033[2;1H\033[L\033[M";
	const unsigned char movements[] =
		"\033[3;4H\033[2A\033[B\033[2C\033[D\033[E\033[F"
		"\033[5G\033[4d\033[2;3f";
	const unsigned char modes_on[] =
		"\033[?1;7;25;1002;1004;1006;2004h\033[4h\033=";
	const unsigned char modes_off[] =
		"\033[?1;7;25;1002;1004;1006;2004l\033[4l\033>";
	const unsigned char styled[] =
		"\033[1;2;3;4;5;7;8;9;38;5;123;48;2;1;2;3mX";
	const unsigned char style_reset[] =
		"\033[22;23;24;25;27;28;29;39;49mY";
	const unsigned char queries[] = "\033[5n\033[6n\033[c\033[>c";
	const char expected_replies[] =
		"\033[0n\033[2;3R\033[?1;2c\033[>1;0;0c";
	const unsigned char unicode[] = {'A', 0xcc, 0x81, 0xe7, 0x95, 0x8c,
	                                 0xc0, 'Z'};
	int failed = 0;
	int world_width;

	if (!init_test_terminal(&terminal, 4, 8, 0)) {
		fprintf(stderr, "model-test: test_vt_commands: allocation failed\n");
		return 1;
	}
	screen = terminal.screen;

	feed_bytes(&terminal, edits, sizeof(edits) - 1);
	TEST_CHECK(row_starts_with(screen, 0, "aXcd"));
	feed_bytes(&terminal, (const unsigned char *)"\033[2G\033[2X",
	           strlen("\033[2G\033[2X"));
	TEST_CHECK(cell_at(screen, 0, 0)->cp == 'a');
	TEST_CHECK(cell_at(screen, 0, 1)->cp == ' ');
	TEST_CHECK(cell_at(screen, 0, 2)->cp == ' ');
	TEST_CHECK(cell_at(screen, 0, 3)->cp == 'd');

	terminal_reset(&terminal);
	feed_bytes(&terminal, lines, sizeof(lines) - 1);
	TEST_CHECK(row_starts_with(screen, 0, "111"));
	TEST_CHECK(row_starts_with(screen, 1, "222"));
	TEST_CHECK(row_starts_with(screen, 2, "333"));
	TEST_CHECK(cell_at(screen, 3, 0)->cp == ' ');

	terminal_reset(&terminal);
	feed_bytes(&terminal, (const unsigned char *)"abcdef\033[3G\033[1K",
	           strlen("abcdef\033[3G\033[1K"));
	TEST_CHECK(cell_at(screen, 0, 0)->cp == ' ' &&
	           cell_at(screen, 0, 2)->cp == ' ' &&
	           cell_at(screen, 0, 3)->cp == 'd');
	feed_bytes(&terminal, (const unsigned char *)"\033[4G\033[K",
	           strlen("\033[4G\033[K"));
	TEST_CHECK(cell_at(screen, 0, 3)->cp == ' ' &&
	           cell_at(screen, 0, 5)->cp == ' ');
	feed_bytes(&terminal, (const unsigned char *)"xy\r\nz\033[1J",
	           strlen("xy\r\nz\033[1J"));
	TEST_CHECK(cell_at(screen, 0, 0)->cp == ' ' &&
	           cell_at(screen, 1, 0)->cp == ' ');

	terminal_reset(&terminal);
	feed_bytes(&terminal, movements, sizeof(movements) - 1);
	TEST_CHECK(screen->row == 1 && screen->col == 2);
	feed_bytes(&terminal, (const unsigned char *)"\033[2;4r\033[?6h\033[1;1H",
	           strlen("\033[2;4r\033[?6h\033[1;1H"));
	TEST_CHECK(screen->origin && screen->row == 1 && screen->col == 0);
	feed_bytes(&terminal, (const unsigned char *)"\033M\033D\033E\033[?6l",
	           strlen("\033M\033D\033E\033[?6l"));
	TEST_CHECK(!screen->origin && screen->row == 0 && screen->col == 0);

	feed_bytes(&terminal, modes_on, sizeof(modes_on) - 1);
	TEST_CHECK(terminal.app_cursor && terminal.app_keypad &&
	           terminal.mouse_mode == 1002 && terminal.mouse_sgr &&
	           terminal.focus_events && terminal.bracketed_paste &&
	           screen->wrap && screen->cursor_visible && screen->insert);
	feed_bytes(&terminal, modes_off, sizeof(modes_off) - 1);
	TEST_CHECK(!terminal.app_cursor && !terminal.app_keypad &&
	           terminal.mouse_mode == 0 && !terminal.mouse_sgr &&
	           !terminal.focus_events && !terminal.bracketed_paste &&
	           !screen->wrap && !screen->cursor_visible && !screen->insert);
	feed_bytes(&terminal, (const unsigned char *)"\033[?7;25h",
	           strlen("\033[?7;25h"));

	terminal_reset(&terminal);
	feed_bytes(&terminal, styled, sizeof(styled) - 1);
	cell = cell_at(screen, 0, 0);
	TEST_CHECK(cell->attr.flags == (ATTR_BOLD | ATTR_DIM | ATTR_ITALIC |
	                                ATTR_UNDERLINE | ATTR_BLINK | ATTR_REVERSE |
	                                ATTR_INVISIBLE | ATTR_STRIKE));
	TEST_CHECK(cell->attr.fg == 123);
	TEST_CHECK(cell->attr.bg == (0x1000000 | (1 << 16) | (2 << 8) | 3));
	feed_bytes(&terminal, style_reset, sizeof(style_reset) - 1);
	cell = cell_at(screen, 0, 1);
	TEST_CHECK(cell->attr.flags == 0 && cell->attr.fg == -1 && cell->attr.bg == -1);

	terminal_reset(&terminal);
	feed_bytes(&terminal, (const unsigned char *)"\033[2;3H",
	           strlen("\033[2;3H"));
	feed_bytes(&terminal, queries, sizeof(queries) - 1);
	TEST_CHECK(terminal.input.length == sizeof(expected_replies) - 1);
	TEST_CHECK(memcmp(terminal.input.data, expected_replies,
	                  sizeof(expected_replies) - 1) == 0);
	buffer_free(&terminal.input);

	feed_bytes(&terminal, (const unsigned char *)"\033]2;hello\001world\a", 16);
	TEST_CHECK(strcmp(terminal.title, "hello world") == 0);
	feed_bytes(&terminal, (const unsigned char *)"\033]0;again\033\\", 11);
	TEST_CHECK(strcmp(terminal.title, "again") == 0);
	feed_bytes(&terminal, (const unsigned char *)"\033Pignored\033\\Q", 12);
	TEST_CHECK(cell_at(screen, 1, 2)->cp == 'Q');

	terminal_reset(&terminal);
	feed_bytes(&terminal, unicode, sizeof(unicode));
	world_width = wcwidth((wchar_t)0x754c) == 2 ? 2 : 1;
	TEST_CHECK(cell_at(screen, 0, 0)->cp == 'A');
	TEST_CHECK(cell_at(screen, 0, 0)->ncombining == 1 &&
	           cell_at(screen, 0, 0)->combining[0] == 0x301);
	TEST_CHECK(cell_at(screen, 0, 1)->cp == 0x754c &&
	           cell_at(screen, 0, 1)->width == world_width);
	TEST_CHECK(cell_at(screen, 0, 1 + world_width)->cp == 0xfffd);
	TEST_CHECK(cell_at(screen, 0, 2 + world_width)->cp == 'Z');

	terminal_reset(&terminal);
	feed_bytes(&terminal, (const unsigned char *)"ab\bX\tY\033)0\016l\017l\vV\fF",
	           strlen("ab\bX\tY\033)0\016l\017l\vV\fF"));
	TEST_CHECK(cell_at(screen, 0, 0)->cp == 'a' &&
	           cell_at(screen, 0, 1)->cp == 'X' &&
	           cell_at(screen, 0, 7)->cp == 'Y');
	TEST_CHECK(cell_at(screen, 1, 0)->cp == 0x250c &&
	           cell_at(screen, 1, 1)->cp == 'l');
	TEST_CHECK(cell_at(screen, 2, 2)->cp == 'V' &&
	           cell_at(screen, 3, 3)->cp == 'F');

	terminal_reset(&terminal);
	feed_bytes(&terminal, (const unsigned char *)"save\0337\033[4;8H\0338",
	           strlen("save\0337\033[4;8H\0338"));
	TEST_CHECK(screen->row == 0 && screen->col == 4);
	feed_bytes(&terminal, (const unsigned char *)"\033[s\033[3;3H\033[u",
	           strlen("\033[s\033[3;3H\033[u"));
	TEST_CHECK(screen->row == 0 && screen->col == 4);
	feed_bytes(&terminal, (const unsigned char *)"\033[?47hA\033[?47l",
	           strlen("\033[?47hA\033[?47l"));
	TEST_CHECK(terminal.screen == &terminal.primary &&
	           cell_at(&terminal.alternate, 0, 0)->cp == 'A');

	free_test_terminal(&terminal);
	return failed;
}

static int
test_model_and_reflow(void)
{
	struct Terminal terminal;
	struct Terminal history_terminal;
	struct Terminal reflow_terminal;
	struct ReflowBuffer combining_buffer;
	struct Screen *screen;
	const struct HistoryLine *line;
	const unsigned char basic[] = "abc\033[2;2HZ\033[31mR\033[0m";
	const unsigned char alternate[] = "\033[?1049hALT\033[?1049l";
	const unsigned char erase[] = "\033[2J\033[Hok";
	const unsigned char line_drawing[] = "\033(0lqk\033(B";
	const unsigned char scrolling[] = "a\r\n\033[31mb\033[0m\r\nc\r\nd\r\n";
	const unsigned char alternate_scrolling[] = "\033[?1049h1\r\n2\r\n3\r\n\033[?1049l";
	const unsigned char clear_history[] = "\033[3J";
	const unsigned char long_line[] = "\033[31mabcdefghij\033[0m";
	const unsigned char combined[] = {'A', 0xcc, 0x81};
	int failed = 0;

	if (!init_test_terminal(&terminal, 3, 8, 0)) {
		fprintf(stderr, "model-test: allocation failed\n");
		return 1;
	}
	feed_bytes(&terminal, basic, sizeof(basic) - 1);
	screen = &terminal.primary;
	TEST_CHECK(cell_at(screen, 0, 0)->cp == 'a' &&
	           cell_at(screen, 0, 2)->cp == 'c' &&
	           cell_at(screen, 1, 1)->cp == 'Z' &&
	           cell_at(screen, 1, 2)->cp == 'R' &&
	           cell_at(screen, 1, 2)->attr.fg == 1);
	feed_bytes(&terminal, alternate, sizeof(alternate) - 1);
	TEST_CHECK(terminal.screen == &terminal.primary &&
	           cell_at(&terminal.alternate, 0, 0)->cp == 'A' &&
	           cell_at(&terminal.primary, 0, 0)->cp == 'a');
	feed_bytes(&terminal, erase, sizeof(erase) - 1);
	TEST_CHECK(cell_at(&terminal.primary, 0, 0)->cp == 'o' &&
	           cell_at(&terminal.primary, 0, 1)->cp == 'k' &&
	           cell_at(&terminal.primary, 1, 1)->cp == ' ');
	feed_bytes(&terminal, line_drawing, sizeof(line_drawing) - 1);
	TEST_CHECK(cell_at(&terminal.primary, 0, 2)->cp == 0x250c &&
	           cell_at(&terminal.primary, 0, 3)->cp == 0x2500 &&
	           cell_at(&terminal.primary, 0, 4)->cp == 0x2510);
	free_test_terminal(&terminal);
	if (!init_test_terminal(&history_terminal, 2, 4, 2)) {
		fprintf(stderr, "model-test: history allocation failed\n");
		return 1;
	}
	feed_bytes(&history_terminal, scrolling, sizeof(scrolling) - 1);
	line = history_line(&history_terminal.history, 0);
	TEST_CHECK(history_terminal.history.count == 2 && line != NULL &&
	           line->length == 1 && line->text[0] == 'b' &&
	           line->span_count == 1 && line->spans[0].attr.fg == 1);
	line = history_line(&history_terminal.history, 1);
	TEST_CHECK(line != NULL && line->length == 1 && line->text[0] == 'c' &&
	           line->span_count == 0);
	feed_bytes(&history_terminal, alternate_scrolling, sizeof(alternate_scrolling) - 1);
	TEST_CHECK(history_terminal.history.count == 2);
	feed_bytes(&history_terminal, clear_history, sizeof(clear_history) - 1);
	TEST_CHECK(history_terminal.history.count == 0);
	terminal_reset(&history_terminal);
	feed_bytes(&history_terminal, combined, sizeof(combined));
	history_push(&history_terminal, cell_at(&history_terminal.primary, 0, 0),
	             history_terminal.primary.cols, false);
	line = history_line(&history_terminal.history, 0);
	memset(&combining_buffer, 0, sizeof(combining_buffer));
	combining_buffer.cols = 2;
	combining_buffer.need_row = true;
	TEST_CHECK(line != NULL && reflow_feed_history(&combining_buffer, line) &&
	           combining_buffer.count != 0 &&
	           combining_buffer.cells[0].cp == 'A' &&
	           combining_buffer.cells[0].ncombining == 1 &&
	           combining_buffer.cells[0].combining[0] == 0x301);
	reflow_buffer_free(&combining_buffer);
	free_test_terminal(&history_terminal);
	if (!init_test_terminal(&reflow_terminal, 3, 6, 16)) {
		fprintf(stderr, "model-test: reflow allocation failed\n");
		return 1;
	}
	feed_bytes(&reflow_terminal, long_line, sizeof(long_line) - 1);
	TEST_CHECK(reflow_terminal.primary.wrapped[0] != 0 &&
	           reflow_primary(&reflow_terminal, 3, 4));
	line = history_line(&reflow_terminal.history, 0);
	TEST_CHECK(reflow_terminal.history.count == 1 && line != NULL &&
	           line->wrapped && line->length == 4 &&
	           memcmp(line->text, "abcd", 4) == 0 && line->span_count == 1 &&
	           line->spans[0].attr.fg == 1 &&
	           cell_at(&reflow_terminal.primary, 0, 0)->cp == 'e' &&
	           cell_at(&reflow_terminal.primary, 0, 3)->cp == 'h' &&
	           reflow_terminal.primary.wrapped[0] != 0 &&
	           cell_at(&reflow_terminal.primary, 1, 0)->cp == 'i' &&
	           cell_at(&reflow_terminal.primary, 1, 1)->cp == 'j' &&
	           reflow_terminal.primary.row == 1 &&
	           reflow_terminal.primary.col == 2);
	TEST_CHECK(reflow_primary(&reflow_terminal, 3, 6) &&
	           reflow_terminal.history.count == 0 &&
	           cell_at(&reflow_terminal.primary, 0, 0)->cp == 'a' &&
	           cell_at(&reflow_terminal.primary, 0, 0)->attr.fg == 1 &&
	           cell_at(&reflow_terminal.primary, 0, 5)->cp == 'f' &&
	           reflow_terminal.primary.wrapped[0] != 0 &&
	           cell_at(&reflow_terminal.primary, 1, 0)->cp == 'g' &&
	           cell_at(&reflow_terminal.primary, 1, 3)->cp == 'j' &&
	           reflow_terminal.primary.row == 1 &&
	           reflow_terminal.primary.col == 4);
	terminal_reset(&reflow_terminal);
	feed_bytes(&reflow_terminal, (const unsigned char *)"one\r\ntwo\r\nthree", 15);
	TEST_CHECK(reflow_primary(&reflow_terminal, 2, 6));
	line = history_line(&reflow_terminal.history, 0);
	TEST_CHECK(reflow_terminal.history.count == 1 && line != NULL &&
	           !line->wrapped && line->length == 3 &&
	           memcmp(line->text, "one", 3) == 0 &&
	           cell_at(&reflow_terminal.primary, 0, 0)->cp == 't' &&
	           cell_at(&reflow_terminal.primary, 1, 0)->cp == 't' &&
	           reflow_terminal.primary.row == 1 &&
	           reflow_terminal.primary.col == 5);
	TEST_CHECK(reflow_primary(&reflow_terminal, 3, 6) &&
	           reflow_terminal.history.count == 0 &&
	           cell_at(&reflow_terminal.primary, 0, 0)->cp == 'o' &&
	           cell_at(&reflow_terminal.primary, 1, 0)->cp == 't' &&
	           cell_at(&reflow_terminal.primary, 2, 0)->cp == 't' &&
	           reflow_terminal.primary.row == 2 &&
	           reflow_terminal.primary.col == 5);
	free_test_terminal(&reflow_terminal);
	return failed;
}

static int
model_test(void)
{
	int failed = 0;

	failed |= test_vt_commands();
	failed |= test_model_and_reflow();
	if (failed) {
		fprintf(stderr, "model-test: failed\n");
		return 1;
	}
	puts("model-test: ok");
	return 0;
}

#undef TEST_CHECK

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--model-test") == 0) {
		(void)setlocale(LC_CTYPE, "");
		return model_test();
	}
	return mwin_program_main(argc, argv);
}

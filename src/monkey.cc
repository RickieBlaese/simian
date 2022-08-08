#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <ncurses.h>

#include "/home/stole/rapidjson/include/rapidjson/document.h"
#include "/home/stole/rapidjson/include/rapidjson/filereadstream.h"


using JSONArray = rapidjson::GenericValue<rapidjson::UTF8<>>::Array;

/* const std::array<char, 4> block{"■"}; */
constexpr double chars_per_word = 5.0;

WINDOW* init_ncurses() {
	WINDOW* wind = initscr();
	noecho();
	keypad(wind, true);
	cbreak();
	wclear(wind);
	wrefresh(wind);
	return wind;
}

chtype wait_for_char(chtype chr) {
	chtype cur = getch();
	while (cur != chr) {
		if (chr != '\t' && cur != KEY_DL) { break; }
		cur = getch();
	}
	return cur;
}

void addmonkeystr() {
	attron(A_STANDOUT);
	addstr("monkey");
	attroff(A_STANDOUT);
}

std::uint64_t current_time() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/* stark overflow */
std::ifstream::pos_type filesize(const char *filename) {
    std::ifstream instream(filename, std::ifstream::ate | std::ifstream::binary);
    return instream.tellg(); 
}

void deinit_ncurses() {
	clear();
	endwin();
}

enum State : unsigned int {
	cont, done, switch_mode
};


State ask_again(bool broken, const long double& wpm) {
	if (!broken) {
		addstr(std::string("\nwpm: ").append(std::to_string(wpm)).append("\nagain [").c_str());
		attron(A_UNDERLINE);
		addch('y');
		attroff(A_UNDERLINE);
		addstr("es, ");
		attron(A_UNDERLINE);
		addch('n');
		attroff(A_UNDERLINE);
		addstr("o]?  ");
		chtype chin = 'q';
		bool cont = false;
		while (true) {
			refresh();
			chin = getch();
			if (chin == '\n' || chin == 'y' || chin == 'Y' || chin == '\t') {
				cont = true;
				break;
			}
			if (chin == 'n' || chin == 'N' || chin == KEY_BACKSPACE || chin == KEY_DL) {
				cont = false;
				break;
			}
		}
		if (!cont) { return State::switch_mode; }
	}
	return State::cont;
}

enum Mode : unsigned int {
	words, timed, zen, help, end
};

Mode ask_mode() {
	char chin = 'w';
	do {
		clear();
		addstr("mode [");
		attron(A_UNDERLINE);
		addch('w');
		attroff(A_UNDERLINE);
		addstr("ords, ");
		attron(A_UNDERLINE);
		addch('t');
		attroff(A_UNDERLINE);
		addstr("imed, ");
		attron(A_UNDERLINE);
		addch('z');
		attroff(A_UNDERLINE);
		addstr("en, ");
		attron(A_UNDERLINE);
		addch('h');
		attroff(A_UNDERLINE);
		addstr("elp, ");
		attron(A_UNDERLINE);
		addch('q');
		attroff(A_UNDERLINE);
		addstr("uit]? ");
		refresh();
		chin = getch();
	} while (chin != 'w' && chin != 't' && chin != 'z' && chin != 'h' && chin != 'q');
	switch (chin) {
		case 'w':
			return Mode::words;
			break;
		case 't':
			return Mode::timed;
			break;
		case 'z':
			return Mode::zen;
			break;
		case 'h':
			return Mode::help;
			break;
		case 'q':
			return Mode::end;
			break;
	}
	return Mode::words;
}


namespace modes {

	State timed(JSONArray& jwords, std::default_random_engine& engine) {
		clear();
		std::shuffle(jwords.begin(), jwords.end(), engine);

		constexpr int viewable = 100;
		constexpr double time_given = 15.0; /* seconds */

		std::string out;
		for (int i = 0; i < viewable; i++) {
			out.append(std::string(jwords[i].GetString())).append(" ");
		}

		/* take out the final space */ 
		out = out.substr(0, out.size() - 1);

		if (out.empty()) {
			mvaddstr(0, 0, "error: mode timed: wordstring was empty");
			refresh();
			getch();
			return State::switch_mode;
		}

		mvaddstr(0, 0, out.c_str());
		move(0, 0);
		refresh();

		std::uint64_t start = 0;
		bool started = false;

		timeout(0);
		int chars_done = 0;
		chtype chin = 0;
		for (const char chout : out) {
			do {
				chin = getch();
				if (chin != 0 && !started) {
					started = true;
					start = current_time();
				}
				if (current_time() - start > static_cast<std::uint64_t>(std::nano::den * time_given)) {
					break;
				}
			} while (chin == 0 || chin != chout); /* to allow checking at the same time we are expecting input: this is instead of threading */
			if (chin == chout) { chars_done++; }
			if (chin == '\t' || chin == KEY_DL) {
				break;
			}
			addch(chout);
			refresh();
		}
		timeout(-1); /* reset to what it was previously */

		clear();
		const long double wpm = static_cast<long double>(chars_done) * (60.0 / (time_given * chars_per_word));
		addstr(std::to_string(chars_done).c_str());
		refresh();

		return ask_again(false, wpm);
	}

	State words(JSONArray& jwords, std::default_random_engine& engine) {
		clear();
		std::shuffle(jwords.begin(), jwords.end(), engine);

		constexpr int words_limit = 15;

		std::string out;
		for (int i = 0; i < words_limit; i++) {
			out.append(std::string(jwords[i].GetString())).append(" ");
		}
		
		out = out.substr(0, out.size() - 1);

		if (out.empty()) {
			mvaddstr(0, 0, "error: mode words: wordstring was empty");
			refresh();
			getch();
			return State::cont;
		}

		addstr(out.c_str());
		move(0, 0);
		refresh();

		std::uint64_t start = 0;
		bool started = false;
		bool broken = false;
		std::int_fast32_t char_count = 0;

		for (const char chout : out) {
			char_count++;
			chtype chin = 0;
			while ((chin = wait_for_char(chout)) != chout) {
				if (chin == KEY_DL || chout == '\t') {
					broken = true;
					break;
				}
			}
			if (!started) {
				started = true;
				start = current_time();
			}
			addch(chout);
			refresh();
		}

		/* bad conversions but idc */
		const long double wpm = static_cast<long double>(char_count) * (static_cast<long double>(std::nano::den * 60) / ((current_time() - start) * chars_per_word));

		return ask_again(broken, wpm);
	}

	State zen(WINDOW *pwin) {
		clear();
		std::uint_fast32_t char_count = 0;
		std::uint64_t start = 0;
		std::uint16_t curx = 0;
		std::uint16_t cury = 0;

		chtype chin = getch();
		start = current_time();

		while (chin != '\t') {
			if (chin == KEY_BACKSPACE) {
				getyx(pwin, cury, curx);
				if (curx > 0) {
					mvaddch(cury, curx - 1, ' ');
					move(cury, curx - 1);
				}
				continue;
			}

			addch(chin);
			char_count++;
			refresh();
			chin = getch();
		}
		
		/* bad conversions but idc */
		const long double wpm = static_cast<long double>(char_count) * (static_cast<long double>(std::nano::den * 60) / ((current_time() - start) * chars_per_word));

		return ask_again(false, wpm);
	}

	State help() {
		clear();
		addstr("need some help?\n");
		addmonkeystr();
		addstr(" was designed to be as close to monkeytype as possible, with some caveats due to the terminal interface.\n"
		"this means that to exit most typing interfaces, [tab] can be used.\nas an alternate key, [del] may also be used in some circumstances.\n"
		"unfortunately, due to terminal resolution, ");
		addmonkeystr();
		addstr(" is unable to use a smoothed cursor when typing.\n"
		"we realize this is hard to type with and are working on a solution.\n\n"
		"press any key to continue... ");
		refresh();
		getch();

		return State::switch_mode;
	}

} /* namespace modes */


int main() {
	WINDOW* full_win = init_ncurses();

	std::FILE *pfile = std::fopen("words/english.json", "r");
	if (pfile == nullptr) {
		std::cerr << "error: fatal: could not open \"english.json\"\n";
		return 1;
	}
	rapidjson::Document doc;
	const std::uintmax_t english_size = std::filesystem::file_size("english.json");
	char *contents = new char[english_size];
	rapidjson::FileReadStream frs(pfile, contents, english_size);
	doc.ParseStream(frs);
	std::fclose(pfile);
	delete[] contents;

	move(0, 0);
	JSONArray jwords = doc.GetArray();

	std::random_device device{};
	std::default_random_engine engine{device()};

	Mode mode = ask_mode();
	State res = State::cont;
	bool done = false;
	while (true) {
		switch (mode) {
			case Mode::words:
				res = modes::words(jwords, engine);
				break;
			case Mode::timed:
				res = modes::timed(jwords, engine);
				break;
			case Mode::zen:
				res = modes::zen(full_win);
				break;
			case Mode::help:
				res = modes::help();
				break;
			case Mode::end:
				done = true;
				break;
		}
		if (done) { break; }
		switch (res) {
			case State::done:
				done = true;
				break;
			case State::cont:
				continue;
				break;
			case State::switch_mode:
				clear();
				mode = ask_mode();
				break;
		}
		if (done) { break; }
	}
	deinit_ncurses();

}

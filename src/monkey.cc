#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <ncurses.h>
#include <menu.h>

#include <sys/stat.h>

#include "/home/stole/rapidjson/include/rapidjson/document.h"
#include "/home/stole/rapidjson/include/rapidjson/filereadstream.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "/home/stole/cpp-httplib/httplib.h"

#include "/home/stole/fmt/include/fmt/core.h"

#include "/home/stole/color/color.h"


bool has_color = false;

const std::string CONFIG_FILENAME = "main.conf";


/* but remember to move cursor to output if index !! */
wchar_t get_unicode_cursor(std::uint32_t index) {
    const static std::wstring cs = L"█▉▊▋▌▍▎▏";
    return cs[7 - (index % 8)];
}


struct Theme {
    /* main is used for correct letters, caret is for caret color, sub is used for other text color, bg is background color, and cful_error is generally error color */
    ssto::color::rgb main, caret, sub, sub_alt, bg,
        text, error, error_extra, cful_error, cful_error_extra; /* cful = colorful */

    /* color pairs */
    std::int16_t main_pair, caret_pair, sub_pair, sub_alt_pair, bg_pair,
        text_pair, error_pair, error_extra_pair, cful_error_pair, cful_error_extra_pair;
};

ssto::color::rgb hex_to_rgb(std::uint32_t hexv) {
    return ssto::color::rgb{static_cast<std::uint16_t>(((hexv >> 16) & 0xFF) / 255.0), static_cast<std::uint16_t>(((hexv >> 8) & 0xFF) / 255.0), static_cast<std::uint16_t>((hexv & 0xFF) / 255.0)};
}

std::unordered_map<std::string, std::string> config = {
    {"theme", ""}, {"name", ""}, {"lang", ""}
};

ssto::color::rgb strhex_to_rgb(const std::string& hexv) {
    std::uint16_t r = 0, g = 0, b = 0;
    if (hexv.length() == 3) {
        std::uint8_t res = std::sscanf(hexv.c_str(), "%hx%hx%hx", &r, &g, &b);
        if (res != 3) { return ssto::color::rgb{0, 0, 0}; }
        return ssto::color::rgb{r, g, b};
    }
    std::uint8_t res = std::sscanf(hexv.c_str(), "%2hx%2hx%2hx", &r, &g, &b);
    if (res != 3) { return ssto::color::rgb{0, 0, 0}; }
    return ssto::color::rgb{r, g, b};
}

enum State : unsigned int {
    cont, done, switch_mode
};

enum Quote : unsigned int {
    szshort, szmedium, szlong, szthicc
};

bool file_exists(const std::string& filename) {
    struct stat buffer{};
    return !stat(filename.c_str(), &buffer);
}


WINDOW* init_ncurses() {
    WINDOW* wind = initscr();
    noecho();
    keypad(wind, true);
    cbreak();
    wclear(wind);
    wrefresh(wind);
    return wind;
}

void deinit_ncurses() {
    clear();
    endwin();
}


/* const std::array<char, 4> block{"■"}; */
static constexpr long double chars_per_word = 5.0;


void split(const std::string& s, const std::string& delim, std::vector<std::string>& outs) {
    std::size_t last = 0, next = 0;
    while ((next = s.find(delim, last)) != std::string::npos) {
        outs.push_back(s.substr(last, next - last));
        last = next + 1;
    }
    if (last != s.size()) { outs.push_back(s.substr(last, s.size())); }
}

void pair_init(std::int16_t pairid, std::int16_t cid2, std::int16_t cid3, const ssto::color::rgb& fg, const ssto::color::rgb& bg) {
    static constexpr double m = 125.0 / 32.0; /* init_color accepts rgb from 0 to 1000 */
    init_color(cid2, static_cast<std::int16_t>(fg.r * m), static_cast<std::int16_t>(fg.g * m), static_cast<std::int16_t>(fg.b * m));
    init_color(cid3, static_cast<std::int16_t>(bg.r * m), static_cast<std::int16_t>(bg.g * m), static_cast<std::int16_t>(bg.b * m));
    init_pair(pairid, cid2, cid3); 
}

std::vector<std::int16_t> ncc_past_colors; /* TODO: */

void nccon(std::int16_t pairid) {
    if (!ncc_past_colors.empty()) {
        attroff(*(ncc_past_colors.end() - 1));
    }
    ncc_past_colors.push_back(pairid);
    attron(COLOR_PAIR(pairid));
}

void nccoff(std::int16_t pairid) {
    auto it = std::find(ncc_past_colors.begin(), ncc_past_colors.end(), pairid);
    if (it != ncc_past_colors.end()) {
        ncc_past_colors.erase(it);
    }
    attroff(COLOR_PAIR(pairid));
    if (!ncc_past_colors.empty()) {
        attron(COLOR_PAIR(*(ncc_past_colors.end() - 1)));
    }
}

/* will fetch from monkeytype if not exist locally */
/* filename should not have beginning */
/* origin is where it is called from for errors */
void fetch_file(const std::string& filename, const std::string& origin) {
    if (file_exists(filename)) {
        return;
    }

    httplib::SSLClient cli("monkeytype.com");
    cli.enable_server_certificate_verification(false);
    auto resp = cli.Get("/" + filename);
    if (!resp) {
        std::stringstream s;
        s << resp.error();
        deinit_ncurses();
        std::cerr << "\nerror: " << origin << " fetch for " << filename << " failed with httplib error " << s.str() << '\n';
        exit(1);
    }

    if (resp->status != 200) {
        deinit_ncurses();
        std::cerr << "\nerror: " << origin << " fetch for " << filename << " failed with status " << resp->status << '\n';
        exit(1);
    }
}

std::string get_file_content(const std::string& filename) {
    std::ifstream file(filename);
    std::stringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    file.close();
    return text;
}

void get_quotes(std::vector<std::string>& outs, Quote size) {
    /* "quotes": [ */
    /*     { */
    /*         "text": "You can't use the fire exit because you're not made of fire.", */
    /*         "source": "Undertale", */
    /*         "id": 12, */
    /*         "length": 60 */
    /*     } */
    /* ] */

    const std::string quotes_filename = fmt::format("quotes/{}.json", config["lang"]);
    fetch_file(quotes_filename, "get_quotes");
    std::FILE *pfile = std::fopen(quotes_filename.c_str(), "r");
    rapidjson::Document doc;
    const std::uintmax_t fsize = std::filesystem::file_size(quotes_filename);
    char *contents = new char[fsize];
    rapidjson::FileReadStream frs(pfile, contents, fsize);
    doc.ParseStream(frs);

    std::fclose(pfile);
    delete[] contents;
    std::int32_t group_begin = doc["groups"][size][0].GetInt(), group_end = doc["groups"][size][1].GetInt();
    for (std::int_fast32_t i = group_begin; i <= group_end; i++) {
        outs.emplace_back(doc["quotes"][i]["text"].GetString());
    }
}

void get_words(std::vector<std::string>& outs) {
    const std::string words_filename = fmt::format("languages/{}.json", config["lang"]);
    fetch_file(words_filename, "get_words");
    std::FILE *pfile = std::fopen(words_filename.c_str(), "r");
    rapidjson::Document doc;
    const std::uintmax_t fsize = std::filesystem::file_size(words_filename);
    char *contents = new char[fsize];
    rapidjson::FileReadStream frs(pfile, contents, fsize);
    doc.ParseStream(frs);
    for (const rapidjson::GenericValue<rapidjson::UTF8<>>& word : doc.GetArray()) {
        outs.emplace_back(word.GetString());
    }
    std::fclose(pfile);
    delete[] contents;
}


void get_themes_list(std::vector<std::string>& outs) {
    /* [ */
    /*     { */
    /*         "name": "oblivion", */
    /*         "bgColor": "#313231", */
    /*         "mainColor": "#a5a096", */
    /*         "subColor": "#5d6263", */
    /*         "textColor": "#f7f5f1" */
    /*     } */
    /* ] */
    
    const std::string list_filename = fmt::format("themes/_list.json");
    fetch_file(list_filename, "get_themes_list");
    std::FILE *pfile = std::fopen(list_filename.c_str(), "r");
    rapidjson::Document doc;
    const std::uintmax_t size = std::filesystem::file_size(list_filename);
    char *contents = new char[size];
    rapidjson::FileReadStream frs(pfile, contents, size);
    doc.ParseStream(frs);
    for (const auto& theme : doc.GetArray()) {
        outs.emplace_back(theme["name"].GetString());
    }
    std::fclose(pfile);
    delete[] contents;
}

/* will assign color ids up to base + 29, color pairs up to base + 27 */
void assign_theme(const std::int16_t& base, Theme& theme) {
    /* NOLINTBEGIN */
    pair_init(base, base + 1, base + 2, theme.main, theme.bg);
    theme.main_pair = base;
    pair_init(base + 3, base + 4, base + 5, theme.caret, theme.bg);
    theme.caret_pair = base + 3;
    pair_init(base + 6, base + 7, base + 8, theme.sub, theme.bg);
    theme.sub_pair = base + 6;
    pair_init(base + 9, base + 10, base + 11, theme.sub_alt, theme.bg);
    theme.sub_alt_pair = base + 9;
    pair_init(base + 12, base + 13, base + 14, theme.text, theme.bg);
    theme.text_pair = base + 12;
    pair_init(base + 15, base + 16, base + 17, theme.error, theme.bg);
    theme.error_pair = base + 15;
    pair_init(base + 18, base + 19, base + 20, theme.error_extra, theme.bg);
    theme.error_extra_pair = base + 18;
    pair_init(base + 21, base + 22, base + 23, theme.cful_error, theme.bg);
    theme.cful_error_pair = base + 21;
    pair_init(base + 24, base + 25, base + 26, theme.cful_error_extra, theme.bg);
    theme.cful_error_extra_pair = base + 24;

    pair_init(base + 27, base + 28, base + 29, theme.bg, theme.bg);
    theme.bg_pair = base + 27;
    /* NOLINTEND */
}


void get_theme(const std::string& name, Theme& theme) {

    const std::string theme_filename = fmt::format("themes/{}.css", name);
    fetch_file(theme_filename, "get_theme");
    std::string text = get_file_content(theme_filename);

    std::vector<std::string> fcolors, tcolors, colors;
    std::size_t brace = text.find('}');
    text = text.substr(0, brace);
    split(text, ";", fcolors);
    for (const std::string& f : fcolors) {
        split(f, "#", tcolors);
        colors.push_back(tcolors[tcolors.size() - 1]);
    }
    
    if (colors.size() < 10) { return; }
    theme.bg = strhex_to_rgb(colors[0]);
    theme.main = strhex_to_rgb(colors[1]);
    theme.caret = strhex_to_rgb(colors[2]);
    theme.sub = strhex_to_rgb(colors[3]);
    theme.sub_alt = strhex_to_rgb(colors[4]);
    theme.text = strhex_to_rgb(colors[5]);
    theme.error = strhex_to_rgb(colors[6]);
    theme.error_extra = strhex_to_rgb(colors[7]);
    theme.cful_error = strhex_to_rgb(colors[8]);
    theme.cful_error_extra = strhex_to_rgb(colors[9]);


    /* just hope that the following color ids don't conflict with current terminal */
    static constexpr std::uint32_t base = 100;
    assign_theme(base, theme);
}

void cleart(const Theme& theme) {
    nccon(theme.bg_pair);
    for (std::int_fast32_t r = 0; r < LINES; r++) {
        for (std::int_fast32_t c = 0; c < COLS; c++) {
            mvaddch(r, c, ' ');
        }
    }
    move(0, 0);
    refresh();
    nccoff(theme.bg_pair);
}


chtype wait_for_char(chtype chr) {
    chtype cur = getch();
    while (cur != chr) {
        if (chr != '\t' && cur != KEY_DL) { break; }
        cur = getch();
    }
    return cur;
}

/* haha */
void addmonkeystr(const Theme& theme) {
    nccon(theme.text_pair);
    addstr("monkey");
    nccoff(theme.text_pair);
}

/* \n seems to cause issues with color */
void addnewline(WINDOW *pwin) {
    std::uint16_t cury = 0, curx = 0;
    getyx(pwin, cury, curx);
    wmove(pwin, cury + 1, 0);
}


std::uint64_t current_time() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/* stark overflow */
std::ifstream::pos_type filesize(const char *filename) {
    std::ifstream instream(filename, std::ifstream::ate | std::ifstream::binary);
    std::ifstream::pos_type r = instream.tellg();
    instream.close();
    return r; 
}



State ask_again(WINDOW *pwin, bool broken, const long double& wpm, const Theme& theme) {
    if (!broken) {
        nccon(theme.sub_pair);
        addnewline(pwin);
        wprintw(pwin, "wpm: %Lf", wpm);
        addnewline(pwin);
        addstr("again [");
        attron(A_UNDERLINE);
        addch('y');
        attroff(A_UNDERLINE);
        addstr("es, ");
        attron(A_UNDERLINE);
        addch('n');
        attroff(A_UNDERLINE);
        addstr("o]? ");
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

Mode ask_mode(WINDOW *pwin, const Theme& theme) {
    char chin = 'w';

    do {
        cleart(theme);
        nccon(theme.sub_pair);
        addstr("monkey see");
        addnewline(pwin);
        attron(A_BOLD);
        addstr("monkeytype");
        attroff(A_BOLD);
        addnewline(pwin);
        addstr("mode [");
        attron(A_UNDERLINE);
        addch('t');
        attroff(A_UNDERLINE);
        addstr("imed, ");
        attron(A_UNDERLINE);
        addch('w');
        attroff(A_UNDERLINE);
        addstr("ords, ");
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

    State timed(WINDOW *pwin, std::vector<std::string>& words, std::default_random_engine& engine, const Theme& theme) {
        cleart(theme);
        std::shuffle(words.begin(), words.end(), engine);

        constexpr int viewable = 100;
        constexpr double time_given = 15.0; /* seconds */

        std::string out;
        for (int i = 0; i < viewable; i++) {
            out.append(words[i]).append(" ");
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
        int i = 0;
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
                if (chin != 0 && chin != chout) {}
            } while ((out.size() == i || chout == ' ') && (chin == 0 || chin != chout)); /* to allow checking at the same time we are expecting input: this is instead of threading */
            if (chin == chout) { chars_done++; }
            if (chin == '\t' || chin == KEY_DL) {
                break;
            }
            if (chin != chout && has_color) { /* got it wrong */
                attron(A_UNDERLINE);
                nccon(theme.cful_error_pair);
                addch(chout);
                nccoff(theme.cful_error_pair);
                attroff(A_UNDERLINE);
            } else {
                attron(A_BOLD);
                addch(chout);
                attroff(A_BOLD);
            }
            refresh();
            i++;
        }
        timeout(-1); /* reset to what it was previously */

        cleart(theme);
        const long double wpm = static_cast<long double>(chars_done) * (60.0 / (time_given * chars_per_word));

        addstr(std::to_string(chars_done).c_str());
        refresh();

        return ask_again(pwin, false, wpm, theme);
    }

    State words(WINDOW *pwin, std::vector<std::string>& words, std::default_random_engine& engine, const Theme& theme) {
        cleart(theme);
        std::shuffle(words.begin(), words.end(), engine);

        constexpr int words_limit = 10;

        std::string out;
        for (int i = 0; i < words_limit; i++) {
            out.append(words[i]).append(" ");
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
        std::vector<std::pair<char, bool /* is error */>> buf;
        buf.reserve(out.size());
        for (char c : out) {
            buf.emplace_back(c, false);
        }
        std::uint32_t p = 0;

        std::uint64_t start = 0;
        bool started = false;
        bool broken = false;
        std::int_fast32_t char_count = 0;

        nccon(theme.main_pair);

        while (p < buf.size()) {
            char_count++;
            chtype chin = 0;
            while ((chin = wait_for_char(buf[p].first))) {
                if (chin == KEY_DL || chin == '\t') {
                    broken = true;
                    break;
                }
                if (chin == buf[p].first) { break; }
                auto newbuf = buf;
                newbuf.assign(buf.begin(), buf.begin() + p);
                newbuf.emplace_back(static_cast<char>(chin), true); /* true error */
                for (auto it = buf.begin() + p; it != buf.end(); it++) {
                    newbuf.push_back(*it);
                }
                buf = newbuf;
            }

            p++;
            if (broken) { break; }
            if (!started) {
                started = true;
                start = current_time();
            }

            cleart(theme);
            for (auto [ch, err] : buf) {
                if (err) {
                    nccon(theme.cful_error_pair);
                    addch(ch);
                    nccoff(theme.cful_error_pair);
                } else {
                    nccon(theme.main_pair);
                    addch(ch);
                    nccoff(theme.main_pair);
                }
            }

            refresh();
        }

        nccoff(theme.main_pair);

        const long double wpm = static_cast<long double>(char_count) * (static_cast<long double>(std::nano::den * 60) / ((current_time() - start) * chars_per_word));

        return ask_again(pwin, broken, wpm, theme);
    }

    State zen(WINDOW *pwin, const Theme& theme) {
        cleart(theme);
        nccon(theme.sub_pair);
        std::uint_fast32_t char_count = 0;
        std::uint64_t start = 0;
        std::uint16_t curx = 0;
        std::uint16_t cury = 0;

        chtype chin = '\0';
        bool started = false;

        while (chin != '\t') {
            chin = getch();
            if (!started) {
                started = true;
                start = current_time();
            }
            if (chin == KEY_BACKSPACE) {
                getyx(pwin, cury, curx);
                if (curx > 0) {
                    mvaddch(cury, curx - 1, ' ');
                    move(cury, curx - 1);
                }
            } else if (chin == '\n') {
                addnewline(pwin);
            } else {
                addch(chin);
                char_count++;
            }
            
            refresh();
        }
        nccoff(theme.sub_pair);
        
        const long double wpm = static_cast<long double>(char_count) * (static_cast<long double>(std::nano::den * 60) / ((current_time() - start) * chars_per_word));

        return ask_again(pwin, false, wpm, theme);
    }

    State help(WINDOW *pwin, const Theme& theme) {
        cleart(theme);
        nccon(theme.sub_pair);
        addstr("need some help?");
        addnewline(pwin);
        addmonkeystr(theme);
        addstr(" was designed to be as close to monkeytype as possible, with some caveats due to the terminal interface.");
        addnewline(pwin);
        addstr("this means that to exit most typing interfaces, [tab] can be used.");
        addnewline(pwin);
        addstr("as an alternate key, [del] may also be used in some circumstances.");
        addnewline(pwin);
        addstr("unfortunately, due to terminal resolution, ");
        addmonkeystr(theme);
        addstr(" is unable to use a smoothed cursor when typing.");
        addnewline(pwin);
        addnewline(pwin);
        addstr("press any key to continue... ");
        nccoff(theme.sub_pair);
        refresh();
        getch();

        return State::switch_mode;
    }

} /* namespace modes */


int main() {
    WINDOW* full_win = init_ncurses();

    start_color();

    std::ifstream config_file(CONFIG_FILENAME);
    if (!config_file.is_open()) {
        deinit_ncurses();
        std::cerr << "fatal: failed to open config file " << CONFIG_FILENAME << '\n';
        return 1;
    }

    std::uint32_t ocount = 0;
    while (config_file) {
        ocount++;
        std::string line;
        config_file >> line;
        if (line.empty()) { continue; }
        std::vector<std::string> opts;
        opts.reserve(2);
        split(line, "=", opts);
        if (opts.size() < 2) {
            wprintw(full_win, "warning: %s config option %i had %lu tokens ... skipping\n", CONFIG_FILENAME.c_str(), ocount, opts.size());
            getch();
            continue;
        }
        if (opts.size() > 2) {
            wprintw(full_win, "warning: %s config option %i had %lu tokens ... using first two\n", CONFIG_FILENAME.c_str(), ocount, opts.size());
            getch();
        }
        std::string name = opts[0], value = opts[1];
        /* wprintw(full_win, "name: %s, value: %s\n", name.c_str(), value.c_str()); */
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });

        config[name] = value;
    }

    for (const std::string& option_name : {"theme", "lang"}) {
        if (config[option_name].empty()) {
            deinit_ncurses();
            std::cerr << "fatal: " << CONFIG_FILENAME << " config " << option_name << " not set (\"" << option_name << "=...\")\n";
            return 1;
        }
    }

    Theme theme{};
    get_theme(config["theme"], theme);

    std::random_device device{};
    std::default_random_engine engine{device()};

    std::vector<std::string> words, quotes;
    get_words(words);
    get_quotes(quotes, Quote::szshort);

    nccon(theme.sub_pair);
    move(0, 0);
    Mode mode = ask_mode(full_win, theme);
    State res = State::cont;
    bool done = false;
    while (true) {
        switch (mode) {
            case Mode::words:
                res = modes::words(full_win, words, engine, theme);
                break;
            case Mode::timed:
                res = modes::timed(full_win, words, engine, theme);
                break;
            case Mode::zen:
                res = modes::zen(full_win, theme);
                break;
            case Mode::help:
                res = modes::help(full_win, theme);
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
                cleart(theme);
                mode = ask_mode(full_win, theme);
                break;
        }
        if (done) { break; }
    }
    nccoff(theme.main_pair);
    deinit_ncurses();

    return 0;

}

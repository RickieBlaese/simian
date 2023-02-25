#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <iostream>
#include <sstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <ncurses.h>
#include <menu.h>

#include <sys/stat.h>
#include <clocale>

#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <cpp-httplib/httplib.h>

#include <color/color.h>

#include <rapidfuzz/fuzz.hpp>


bool has_color = false;

const std::string CONFIG_FILENAME = "main.conf";


/* but remember to move cursor to output if animating 2nd half !! */
wchar_t get_unicode_caret(std::uint32_t index) {
    const static std::wstring cs = L"▏▎▍▌▋▊▉█▕"; /* 100 ms for one char ? 6.25 ms per cursor */
    return cs[index];
}


struct Theme {
    /* main is used for correct letters, caret is for caret color, text is used for slightly standout text, sub is used for other text color, bg is background color, colorful_error is general error color, and colorful_error_extra is used for incorrect letters typed outside of a word */
    ssto::color::rgb main, caret, sub, sub_alt, bg,
        text, error, error_extra, colorful_error, colorful_error_extra; /* colorful = colorful */

    /* color pairs */
    std::int16_t main_pair, caret_pair, caret_inverse_pair, sub_pair, sub_alt_pair, bg_pair,
        text_pair, error_pair, error_extra_pair, colorful_error_pair, colorful_error_extra_pair;
};

ssto::color::rgb hex_to_rgb(std::uint32_t hexv) {
    return ssto::color::rgb{static_cast<std::uint16_t>(std::round(((hexv >> 16) & 0xFF) / 255.0)), static_cast<std::uint16_t>(std::round(((hexv >> 8) & 0xFF) / 255.0)), static_cast<std::uint16_t>(std::round((hexv & 0xFF) / 255.0))};
}


/* ----- */
std::unordered_map<std::string, std::string> config = {
    {"theme", ""}, {"name", ""}, {"language", ""},
    {"base_color_id", "200"},
    {"caret_wait", "6250"}, {"hide_caret", "false"}, {"smooth_caret", "true"},
    {"show_decimal_places", "false"}
};
/* ----- */

/* copied from rapidfuzz github */
template <typename Sentence1, typename Iterable, typename Sentence2 = typename Iterable::value_type>
std::optional<std::pair<Sentence2, double>> extractOne(const Sentence1& query, const Iterable& choices, const double score_cutoff = 0.0) {
    bool match_found = false;
    double best_score = score_cutoff;
    Sentence2 best_match;

    rapidfuzz::fuzz::CachedPartialRatio<typename Sentence1::value_type> scorer(query);

    for (const auto& choice : choices) {
        double score = scorer.similarity(choice, best_score);

        if (score >= best_score) {
            match_found = true;
            best_score = score;
            best_match = choice;
        }
    }

    if (!match_found) {
          return std::nullopt;
    }

    return std::make_pair(best_match, best_score);
}

bool str_startswith(const std::string& src, const std::string& match) {
    return src.length() >= match.length() ? src.substr(0, match.length()) == match : false;
}



ssto::color::rgb strhex_to_rgb(const std::string& hexv) {
    std::uint16_t r = 0, g = 0, b = 0;
    ssto::color::rgb ret{};
    std::uint8_t res = 0;

    /* no idea why but r, g, b are all one lower than they should be here after reading */
    if (hexv.length() == 3) {
        res = std::sscanf(hexv.c_str(), "%1hx%1hx%1hx", &r, &g, &b);
        ret.r = r * 17 + 1;
        ret.g = g * 17 + 1;
        ret.b = b * 17 + 1;
    } else if (hexv.length() == 6) {
        res = std::sscanf(hexv.c_str(), "%2hx%2hx%2hx", &r, &g, &b);
        ret.r = r + 1;
        ret.g = g + 1;
        ret.b = b + 1;
    }
    if (res != 3) { return {0, 0, 0}; }

    return ret;
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
    /* TODO: maybe check if pair already in use through pair_content ? and adjust if necessary */
    static constexpr double m = 125.0 / 32.0; /* init_color accepts rgb from 0 to 1000 */
    init_color(cid2, static_cast<std::int16_t>(std::round(fg.r * m)), static_cast<std::int16_t>(std::round(fg.g * m)), static_cast<std::int16_t>(std::round(fg.b * m)));
    init_color(cid3, static_cast<std::int16_t>(std::round(bg.r * m)), static_cast<std::int16_t>(std::round(bg.g * m)), static_cast<std::int16_t>(std::round(bg.b * m)));
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

/* origin is for error msgs, report where it was called from and what was read */
bool str_rdb(const std::string& name, const std::string& origin) {
    std::string confs = config[name];
    std::transform(confs.begin(), confs.end(), confs.begin(), [](unsigned char c) { return std::tolower(c); });

    if (str_startswith(confs, "true") || str_startswith(confs, "y")) {
        return true;
    }

    if (str_startswith(confs, "false") || str_startswith(confs, "no")) {
        return false;
    }

    deinit_ncurses();
    std::cerr << "fatal: " << origin << ": failed to convert option " << name << " value \"" << confs << "\" to bool\n";
    exit(1);
}

/* origin is for error msgs, report where it was called from and what was read */
std::int64_t str_rdll(const std::string& name, const std::string& origin) {
    std::string confs = config[name];
    std::int64_t r = 0;
    try {
        r = std::stoll(confs);
    } catch (std::exception& e) {
        deinit_ncurses();
        std::cerr << "fatal: " << origin << ": failed to convert option " << name << " value \"" << confs << "\" to long long\n";
        exit(1);
    }

    return r;
}

/* will fetch from monkeytype if not exist locally */
/* filename should not have beginning */
/* origin is where it is called from for errors */
void fetch_file(const std::string& filename, const std::string& origin) {
    if (file_exists(filename)) {
        return;
    }

    printw("info: %s: fetching monkeytype.com/%s\n", origin.c_str(), filename.c_str());
    getch();
    refresh();

    httplib::SSLClient cli("monkeytype.com");
    cli.enable_server_certificate_verification(false);
    auto resp = cli.Get("/" + filename);
    if (!resp) {
        std::stringstream s;
        s << resp.error();
        deinit_ncurses();
        std::cerr << "fatal: " << origin << ": fetch for " << filename << " failed with httplib error " << s.str() << '\n';
        exit(1);
    }

    if (resp->status != 200) {
        deinit_ncurses();
        std::cerr << "fatal: " << origin << ": fetch for " << filename << " failed with status " << resp->status << '\n';
        exit(1);
    }

    std::string text = resp->body;
    if (text.substr(0, 9) == "<!doctype") { /* returned nice looking html 404 page */
        deinit_ncurses();
        std::cerr << "fatal: " << origin << ": fetch for " << filename << " failed with status 400\n";
        exit(1);
    }

    std::ofstream outfile(filename);
    outfile << text;
    outfile.close();
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

    const std::string quotes_filename = "quotes/" + config["language"] + ".json";
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
    for (const auto& quote : doc["quotes"].GetArray()) {
        const rapidjson::SizeType len = quote["text"].GetStringLength();
        if (len >= group_begin || len < group_end) {
            outs.emplace_back(quote["text"].GetString());
        }
    }
}

void get_words(std::vector<std::string>& outs) {
    const std::string words_filename = "languages/" + config["language"] + ".json";
    fetch_file(words_filename, "get_words");
    std::FILE *pfile = std::fopen(words_filename.c_str(), "r");
    rapidjson::Document doc;
    const std::uintmax_t fsize = std::filesystem::file_size(words_filename);
    char *contents = new char[fsize];
    rapidjson::FileReadStream frs(pfile, contents, fsize);
    doc.ParseStream(frs);
    for (const auto& word : doc["words"].GetArray()) {
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
    
    const std::string list_filename = "themes/_list.json";
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

/* will assign color ids up to base + 30, color pairs up to base + 32 */
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
    pair_init(base + 21, base + 22, base + 23, theme.colorful_error, theme.bg);
    theme.colorful_error_pair = base + 21;
    pair_init(base + 24, base + 25, base + 26, theme.colorful_error_extra, theme.bg);
    theme.colorful_error_extra_pair = base + 24;

    pair_init(base + 27, base + 28, base + 29, theme.bg, theme.bg);
    theme.bg_pair = base + 27;

    pair_init(base + 30, base + 31, base + 32, theme.bg, theme.caret);
    theme.caret_inverse_pair = base + 30;
    /* NOLINTEND */
}


void get_theme(const std::string& name, Theme& theme) {
    const std::string theme_filename = "themes/" + name + ".css";
    std::string text;

    if (name == "custom") { /* we do not want to fetch */
        if (!file_exists(theme_filename)) {
            deinit_ncurses();
            std::cerr << "fatal: get_theme: custom theme custom.css file does not exist\n";
            exit(1);
        }
    } else {
        fetch_file(theme_filename, "get_theme");
    }

    text = get_file_content(theme_filename);

    /* parse this better, not all css define in the same order */
    std::vector<std::string> fcolors, tcolors, colors;
    std::size_t beginb = text.find(":root{") + 5, endb = text.find('}', beginb);
    text = text.substr(beginb + 1, endb - beginb);
    split(text, ";", fcolors);
    for (const std::string& f : fcolors) {
        std::size_t beginh = f.find('#') + 1, beginn = f.find("--") + 2;
        std::string cname = f.substr(beginn, f.find(':') - 6 - beginn); /* 6 is width of "-color" suffix */
        ssto::color::rgb color = strhex_to_rgb(f.substr(beginh));


        if (cname == "bg") { theme.bg = color; }
        else if (cname == "main") { theme.main = color; }
        else if (cname == "caret") { theme.caret = color; }
        else if (cname == "sub") { theme.sub = color; }
        else if (cname == "sub-alt") { theme.sub_alt = color; }
        else if (cname == "text") { theme.text = color; }
        else if (cname == "error") { theme.error = color; }
        else if (cname == "error-extra") { theme.error_extra = color; }
        else if (cname == "colorful-error") { theme.colorful_error = color; }
        else if (cname == "colorful-error-extra") { theme.colorful_error_extra = color; }
        else {
            deinit_ncurses();
            std::cerr << "fatal: get_theme: parsing theme " << name << " failed near \"" << f << "\"\n";
            exit(1);
        }

    }

    /* just hope that the color ids don't conflict with terminal */
    assign_theme(static_cast<std::int16_t>(str_rdll("base_color_id", "get_theme")), theme);
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
        if (chr == '\t' || cur == KEY_DL) { break; }
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

long double rounddouble(const long double& num, std::int32_t places) {
    const long double coeff = std::pow(10, places);
    return std::round(num * coeff) / coeff;
}

inline std::int64_t roundlong(const long double& num) {
    return static_cast<std::int64_t>(std::round(num));
}


State ask_again(WINDOW *pwin, bool broken, const long double& wpm, const Theme& theme) {
    if (!broken) {
        nccon(theme.sub_pair);
        addnewline(pwin);
        waddstr(pwin, "wpm: ");
        nccon(theme.main_pair);
        if (str_rdb("show_decimal_places", "ask_again")) {
            wprintw(pwin, "%.2Lf", rounddouble(wpm, 2));
        } else {
            wprintw(pwin, "%li", roundlong(wpm));
        }
        nccoff(theme.main_pair);
        addnewline(pwin);
        addstr("again [");
        nccon(theme.main_pair);
        attron(A_UNDERLINE);
        addch('y');
        attroff(A_UNDERLINE);
        addstr("es");
        nccoff(theme.main_pair);
        addstr(", ");
        nccon(theme.main_pair);
        attron(A_UNDERLINE);
        addch('n');
        attroff(A_UNDERLINE);
        addstr("o");
        nccoff(theme.main_pair);
        addstr("]? ");
        nccoff(theme.sub_pair);
        chtype chin = 'q';

        curs_set(2);
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
        nccon(theme.text_pair);
        attron(A_BOLD);
        addstr("monkeytype");
        attroff(A_BOLD);
        nccoff(theme.text_pair);
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

        const std::size_t viewable = std::min(200UL, words.size());
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
                nccon(theme.colorful_error_pair);
                addch(chout);
                nccoff(theme.colorful_error_pair);
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
        nccon(theme.sub_pair);
        std::shuffle(words.begin(), words.end(), engine);

        constexpr int words_limit = 10;

        std::string out;
        for (int i = 0; i < words_limit; i++) {
            out.append(words[i]).append(" ");
        }
        
        if (out.empty()) {
            mvaddstr(0, 0, "error: mode words: wordstring was empty");
            refresh();
            getch();
            return State::cont;
        }

        out.erase(out.end() - 1);

        addstr(out.c_str());
        move(0, 0);
        refresh();
        std::vector<std::pair<char, bool /* is error */>> buf;
        buf.reserve(out.size());
        for (char c : out) {
            buf.emplace_back(c, false);
        }

        std::int32_t p = 0;
        std::uint64_t start = 0;
        bool started = false;
        bool broken = false;
        std::int_fast32_t char_count = 0;

        curs_set(0);

        /* putting off total rewrite with nonblocking getch */
        while (p < buf.size()) {
            chtype chin = getch();

            if (chin == KEY_DL || chin == '\t') {
                broken = true;
                break;
            }
            
            if (chin == '\n') {
                continue;
            }

            int x = 0, y = 0;
            getyx(pwin, y, x);
            bool forwards = true;
            if (chin == KEY_BACKSPACE) {
                if (p <= 0) { continue; }
                if (buf[p - 1].second) {
                    buf.erase(buf.begin() + p - 1);
                }
                p--;
                forwards = false;
            } else {
                if (chin != buf[p].first) {
                    buf.insert(buf.begin() + p, std::make_pair(static_cast<char>(chin), true));
                } else {
                    char_count++;
                }

                if (!started) {
                    started = true;
                    start = current_time();
                }

                p++;
            }


            auto outch = [&theme](const std::pair<char, bool>& bchar, bool correct) {
                char ch = bchar.first;
                bool err = bchar.second;

                if (err) {
                    nccon(theme.colorful_error_pair);
                    addch(ch);
                    nccoff(theme.colorful_error_pair);
                } else if (correct) {
                    nccon(theme.main_pair);
                    addch(ch);
                    nccoff(theme.main_pair);
                } else {
                    nccon(theme.sub_pair);
                    addch(ch);
                    nccoff(theme.sub_pair);
                }
            };

            
            if (str_rdb("smooth_caret", "mode words")) {
                std::int64_t caret_wait = str_rdll("caret_wait", "mode words");
                if (p > 0 && forwards) {
                    /* to erase artifacts from last dangling thin cursor */
                    if (p > 1) {
                        move(y, p - 2);
                        outch(buf[p - 2], true);
                    }

                    nccon(theme.caret_pair);
                    for (int i = 0; i < 8; i++) {
                        move(y, p - 1);
                        printw("%lc", get_unicode_caret(i));
                        move(y, p - 1);
                        refresh();
                        std::this_thread::sleep_for(std::chrono::microseconds(caret_wait));
                    }
                    nccoff(theme.caret_pair);

                    nccon(theme.caret_inverse_pair);
                    for (int i = 0; i < 7; i++) {
                        move(y, p - 1);
                        printw("%lc", get_unicode_caret(i));
                        move(y, p - 1);
                        refresh();
                        std::this_thread::sleep_for(std::chrono::microseconds(caret_wait));
                    }
                    nccoff(theme.caret_inverse_pair);
                } else if (!forwards) {
                    /* to erase artifacts from last dangling thin cursor */
                    move(y, p + 1);
                    outch(buf[p + 1], true);

                    nccon(theme.caret_inverse_pair);
                    for (int i = 7; i >= 0; i--) {
                        move(y, p);
                        printw("%lc", get_unicode_caret(i));
                        move(y, p);
                        refresh();
                        std::this_thread::sleep_for(std::chrono::microseconds(caret_wait));
                    }
                    nccoff(theme.caret_inverse_pair);

                    nccon(theme.caret_pair);
                    for (int i = 7; i >= 1; i--) {
                        move(y, p);
                        printw("%lc", get_unicode_caret(i));
                        move(y, p);
                        refresh();
                        std::this_thread::sleep_for(std::chrono::microseconds(caret_wait));
                    }
                    nccoff(theme.caret_pair);
                }
            }


            cleart(theme);
            std::int_fast32_t i = 0;
            for (const std::pair<char, bool>& bchar : buf) {
                outch(bchar, i < p);
                i++;
            }

            nccon(theme.caret_pair);
            move(y, p - 1);
            printw("%lc", get_unicode_caret(8));
            move(y, p - 1);
            nccoff(theme.caret_pair);

            refresh();
        }

        nccoff(theme.sub_pair);

        const long double wpm = static_cast<long double>(char_count) * (static_cast<long double>(std::nano::den * 60) / ((current_time() - start) * chars_per_word));

        return ask_again(pwin, broken, wpm, theme);
    }

    State zen(WINDOW *pwin, const Theme& theme) {
        cleart(theme);
        nccon(theme.main_pair);
        std::uint_fast32_t char_count = 0;
        std::uint64_t start = 0;
        std::uint16_t curx = 0;
        std::uint16_t cury = 0;

        chtype chin = '\0';
        bool started = false;

        curs_set(1);
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
        nccoff(theme.main_pair);
        
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
        addstr(" is unable to replicate the same exact experience when typing.");
        addnewline(pwin);
        addnewline(pwin);
        addstr("press any key to continue... ");
        nccoff(theme.sub_pair);
        refresh();
        getch();

        return State::switch_mode;
    }

} /* namespace modes */


int main(int argc, char** argv) {
    /* TODO: maybe option to output template .conf? or theme list or similar */
    /* if (argc > 1) { */

    
    std::setlocale(LC_ALL, "");

    WINDOW* full_win = init_ncurses();
    start_color();

    std::ifstream config_file(CONFIG_FILENAME);
    if (!config_file.is_open()) {
        deinit_ncurses();
        std::cerr << "fatal: main: failed to open config file " << CONFIG_FILENAME << '\n';
        return 1;
    }

    std::vector<std::string> configkeys;
    configkeys.reserve(config.size());
    for (auto [key, value] : config) {
        configkeys.push_back(key);
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
            wprintw(full_win, "warning: %s file %ith config option had %lu tokens ... skipping\n", CONFIG_FILENAME.c_str(), ocount, opts.size());
            continue;
        }
        if (opts.size() > 2) {
            wprintw(full_win, "warning: %s file %ith config option had %lu tokens ... using first two\n", CONFIG_FILENAME.c_str(), ocount, opts.size());
        }
        std::string name = opts[0], value = opts[1];
        /* wprintw(full_win, "name: %s, value: %s\n", name.c_str(), value.c_str()); */
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });

        if (!config.contains(name)) {
            std::optional<std::pair<std::string, double>> res = extractOne(name, configkeys);
            if (!res.has_value()) {
                wprintw(full_win, "warning: %s file %ith config option \"%s\" not found\n", CONFIG_FILENAME.c_str(), ocount, name.c_str());
                continue;
            }
            wprintw(full_win, "warning: %s file %ith config option \"%s\" not found, did you mean \"%s\"?\n", CONFIG_FILENAME.c_str(), ocount, name.c_str(), res.value().first.c_str());
            continue;
        }

        config[name] = value;
    }

    for (auto [name, value] : config) {
        if (value.empty()) {
            deinit_ncurses();
            std::cerr << "fatal: main: " << CONFIG_FILENAME << " config " << name << " not set (\"" << name << "=...\")\n";
            return 1;
        }
    }
    refresh();
    getch();

    bool hc = str_rdb("hide_caret", "main");
    bool fc = str_rdb("smooth_caret", "main");

    Theme theme{};
    get_theme(config["theme"], theme);

    std::random_device device{};
    std::default_random_engine engine{device()};

    std::vector<std::string> words, quotes;
    get_words(words);
    /* get_quotes(quotes, Quote::szshort); */

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

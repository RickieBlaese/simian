#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <iostream>
#include <thread>
#include <sstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <cstdint>
#include <clocale>

#include <ncurses.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libs/src/rapidjson/include/rapidjson/document.h>
#include <libs/src/rapidjson/include/rapidjson/filereadstream.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <libs/src/cpp-httplib/httplib.h>

#include <libs/src/rapidfuzz-cpp/rapidfuzz/fuzz.hpp>



enum chstate : unsigned int {
    original, correct, err, err_extra
};

struct chinfo_t {
    char ch;
    chstate state = chstate::original;
};

bool has_color = false;

const std::string CONFIG_FILENAME = "main.conf";
const std::string LOG_FILENAME = "main.log";

/* https://vi.stackexchange.com/questions/25151/how-to-change-vim-cursor-shape-in-text-console */
/* but remember to invert output if animating 2nd half !! */
wchar_t get_unicode_caret(std::uint32_t index) {
    const static std::wstring cs = L"▏▎▍▌▋▊▉█▕"; /* 100 ms for one char ? 6.25 ms per cursor */
    return cs[index];
}

std::uint64_t get_current_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

void cubic_bezier(double t, double x1, double y1, double x2, double y2, double &ox, double &oy) {
    ox = 3.0 * (1.0 - t) * (1.0 - t) * t * x1 + 3 * (1.0 - t) * t * t * x2 + t * t * t;
    oy = 3.0 * (1.0 - t) * (1.0 - t) * t * y1 + 3 * (1.0 - t) * t * t * y2 + t * t * t;
}

void ease(double t, double &ox, double &oy) {
    cubic_bezier(t, 0.25, 0.1, 0.25, 1.0, ox, oy);
}

template <typename T>
T coeff_variation(const std::vector<T> &samples) {
    
    const std::size_t sz = samples.size();
    if (sz <= 1) { return 0.0; }
    const T mean = std::accumulate(samples.begin(), samples.end(), 0.0) / sz;

    auto variance_func = [&mean, &sz](T accumulator, const T &val) {
        return accumulator + ((val - mean) * (val - mean) / (sz - 1));
    };

    return std::sqrt(std::accumulate(samples.begin(), samples.end(), 0.0, variance_func)) / mean;
}

struct RGB {
    std::uint16_t r, g, b;
};

struct Theme {
    /* main is used for correct letters, caret is for caret color, text is used for slightly standout text, sub is used for other text color, bg is background color, colorful_error is general error color, and colorful_error_extra is used for incorrect letters typed outside of a word */
    RGB main, caret, sub, sub_alt, bg,
        text, error, error_extra, colorful_error, colorful_error_extra; /* colorful = colorful */

    /* color pairs */
    std::int16_t main_pair, caret_pair, caret_inverse_pair, sub_pair, sub_alt_pair, bg_pair,
        text_pair, error_pair, error_extra_pair, colorful_error_pair, colorful_error_extra_pair;
};

RGB hex_to_rgb(std::uint32_t hexv) {
    return RGB{static_cast<std::uint16_t>(std::round(((hexv >> 16) & 0xFF) / 255.0)), static_cast<std::uint16_t>(std::round(((hexv >> 8) & 0xFF) / 255.0)), static_cast<std::uint16_t>(std::round((hexv & 0xFF) / 255.0))};
}


/* ----- */
std::unordered_map<std::string, std::string> config = {
    {"theme", ""}, {"name", ""}, {"language", ""},
    {"base_color_id", "200"},
    {"caret_wait", "6250"}, {"hide_caret", "false"}, {"smooth_caret", "true"}, {"xterm_support", "true"},
    {"show_decimal_places", "false"}
};
/* ----- */

/* copied from rapidfuzz github */
template <typename Sentence1, typename Iterable, typename Sentence2 = typename Iterable::value_type>
std::optional<std::pair<Sentence2, double>> extract_one(const Sentence1 &query, const Iterable &choices, const double score_cutoff = 0.0) {
    bool match_found = false;
    double best_score = score_cutoff;
    Sentence2 best_match;

    rapidfuzz::fuzz::CachedPartialRatio<typename Sentence1::value_type> scorer(query);

    for (const auto &choice : choices) {
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

bool str_startswith(const std::string &src, const std::string &match) {
    return src.length() >= match.length() ? src.substr(0, match.length()) == match : false;
}



RGB strhex_to_rgb(const std::string &hexv) {
    std::uint16_t r = 0, g = 0, b = 0;
    RGB ret{};
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

/* order is crucial : do not change */
enum CursorType : unsigned int {
    blinking_block, blinking_block_default, steady_block,
    blinking_underline, steady_underline, blinking_bar_xterm, steady_bar_xterm
};

void set_cursor_type(const CursorType &ct) {
    const std::string out = "\33[" + std::to_string(ct) + " q";
    /* using write directly here just in case, we want to get around ncurses all the way */
    write(1, out.c_str(), out.length());
}

bool file_exists(const std::string &filename) {
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


void split(const std::string &s, const std::string &delim, std::vector<std::string> &outs) {
    std::size_t last = 0, next = 0;
    while ((next = s.find(delim, last)) != std::string::npos) {
        outs.push_back(s.substr(last, next - last));
        last = next + 1;
    }
    if (last != s.size()) { outs.push_back(s.substr(last, s.size())); }
}

void pair_init(std::int16_t pairid, std::int16_t cid2, std::int16_t cid3, const RGB &fg, const RGB &bg) {
    /* TODO: maybe check if pair already in use through pair_content ? and adjust if necessary */
    static constexpr double m = 125.0 / 32.0; /* init_color accepts rgb from 0 to 1000 */
    init_color(cid2, static_cast<std::int16_t>(std::round(fg.r * m)), static_cast<std::int16_t>(std::round(fg.g * m)), static_cast<std::int16_t>(std::round(fg.b * m)));
    init_color(cid3, static_cast<std::int16_t>(std::round(bg.r * m)), static_cast<std::int16_t>(std::round(bg.g * m)), static_cast<std::int16_t>(std::round(bg.b * m)));
    init_pair(pairid, cid2, cid3); 
}

std::vector<std::int16_t> ncc_past_colors;

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
bool str_rdb(const std::string &name, const std::string &origin) {
    std::string confs = config[name];
    std::transform(confs.begin(), confs.end(), confs.begin(), [](unsigned char c) { return std::tolower(c); });

    if (str_startswith(confs, "tru") || str_startswith(confs, "y")) {
        return true;
    }

    if (str_startswith(confs, "fals") || str_startswith(confs, "no")) {
        return false;
    }

    deinit_ncurses();
    std::cerr << "fatal: " << origin << ": failed to convert option " << name << " value \"" << confs << "\" to bool\n";
    exit(1);
}

/* origin is for error msgs, report where it was called from and what was read */
std::int64_t str_rdll(const std::string &name, const std::string &origin) {
    std::string confs = config[name];
    std::int64_t r = 0;
    try {
        r = std::stoll(confs);
    } catch (std::exception &e) {
        deinit_ncurses();
        std::cerr << "fatal: " << origin << ": failed to convert option " << name << " value \"" << confs << "\" to long long\n";
        exit(1);
    }

    return r;
}

void outch(const chinfo_t &bchar, const Theme &theme) {
    if (bchar.state == chstate::err) {
        nccon(theme.colorful_error_pair);
        addch(bchar.ch);
        nccoff(theme.colorful_error_pair);
    } else if (bchar.state == chstate::err_extra) {
        nccon(theme.colorful_error_extra_pair);
        addch(bchar.ch);
        nccoff(theme.colorful_error_extra_pair);
    } else if (bchar.state == chstate::correct) {
        nccon(theme.main_pair);
        addch(bchar.ch);
        nccoff(theme.main_pair);
    } else {
        nccon(theme.sub_pair);
        addch(bchar.ch);
        nccoff(theme.sub_pair);
    }
}

template <std::size_t N>
void animate_caret(std::mutex &term_mutex, std::int32_t y, std::int32_t p, std::int32_t chars_covering, bool forwards, std::vector<std::uint64_t> &times, const Theme &theme, std::vector<chinfo_t> &buf, std::int32_t pword, std::bitset<N> &incorrect_words, const std::string &origin) {
    static thread_local std::int32_t last_p = 0;
    const std::uint64_t rdcaret_wait = str_rdll("caret_wait", origin);
    if (str_rdb("smooth_caret", origin)) {
        auto caret_wait = [&]() -> std::uint64_t {
            std::lock_guard guard(term_mutex);
            const std::uint64_t times_wait = (times.size() > 1 ? (times[times.size() - 1] - times[times.size() - 2]) / ((static_cast<std::int32_t>(forwards) * 2 - 1) * (chars_covering) * 15'000) : rdcaret_wait);
            return std::min<std::uint64_t>(rdcaret_wait, times_wait);
        };
        if (p > 0 && forwards) {
            for (int i = 0; i < 8; i++) {
                {
                    std::lock_guard guard(term_mutex);
                    move(y, p - 1);
                    nccon(theme.caret_pair);
                    printw("%lc", get_unicode_caret(i));
                    nccoff(theme.caret_pair);
                    move(y, p - 1);
                    refresh();
                }
                std::this_thread::sleep_for(std::chrono::microseconds(caret_wait()));
            }
            for (int i = 0; i < 7; i++) {
                {
                    std::lock_guard guard(term_mutex);
                    move(y, p - 1);
                    nccon(theme.caret_inverse_pair);
                    printw("%lc", get_unicode_caret(i));
                    nccoff(theme.caret_inverse_pair);
                    move(y, p - 1);
                    refresh();
                }
                std::this_thread::sleep_for(std::chrono::microseconds(caret_wait()));
            }

        } else if (!forwards) {
            for (int i = 7; i >= 0; i--) {
                {
                    std::lock_guard guard(term_mutex);
                    move(y, p);
                    nccon(theme.caret_inverse_pair);
                    printw("%lc", get_unicode_caret(i));
                    nccoff(theme.caret_inverse_pair);
                    move(y, p);
                    refresh();
                }
                std::this_thread::sleep_for(std::chrono::microseconds(caret_wait()));
            }
            for (int i = 7; i >= 1; i--) {
                {
                    std::lock_guard guard(term_mutex);
                    move(y, p);
                    nccon(theme.caret_pair);
                    printw("%lc", get_unicode_caret(i));
                    nccoff(theme.caret_pair);
                    move(y, p);
                    refresh();
                }
                std::this_thread::sleep_for(std::chrono::microseconds(caret_wait()));
            }
        }
        std::lock_guard guard(term_mutex);
        std::int32_t cw = 0;
        std::int32_t ep = p + (static_cast<std::int32_t>(!forwards) - 1);
        for (std::int32_t i = 0; i <= ep; i++) {
            if (buf[i].ch == ' ') {
                cw++;
            }
        }

        curs_set(0);
        if (incorrect_words[cw] && cw < pword && buf[ep].ch != ' ') {
            attron(A_UNDERLINE);
        }
        move(y, ep);
        outch(buf[ep], theme);
        if (incorrect_words[cw] && cw < pword && buf[ep].ch != ' ') {
            attroff(A_UNDERLINE);
        }
        last_p = p;
    }

    std::lock_guard guard(term_mutex);
    if (str_rdb("xterm_support", "mode words")) {
        curs_set(1);
        move(y, p);
        set_cursor_type(CursorType::steady_bar_xterm);
    } else {
        curs_set(0);
        nccon(theme.caret_pair);
        if (p > 0) {
            mvprintw(y, p - 1, "%lc", get_unicode_caret(8));
        }
        nccoff(theme.caret_pair);
    }
    refresh();
}


/* will fetch from monkeytype if not exist locally */
/* filename should not have beginning */
/* origin is where it is called from for errors */
void fetch_file(const std::string &filename, const std::string &origin) {
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

std::string get_file_content(const std::string &filename) {
    std::ifstream file(filename);
    std::stringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    file.close();
    return text;
}

void get_quotes(std::vector<std::string> &outs, Quote size) {
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
    for (const auto &quote : doc["quotes"].GetArray()) {
        const rapidjson::SizeType len = quote["text"].GetStringLength();
        if (len >= group_begin && len < group_end) {
            outs.emplace_back(quote["text"].GetString());
        }
    }
}

void get_words(std::vector<std::string> &outs) {
    const std::string words_filename = "languages/" + config["language"] + ".json";
    fetch_file(words_filename, "get_words");
    std::FILE *pfile = std::fopen(words_filename.c_str(), "r");
    rapidjson::Document doc;
    const std::uintmax_t fsize = std::filesystem::file_size(words_filename);
    char *contents = new char[fsize];
    rapidjson::FileReadStream frs(pfile, contents, fsize);
    doc.ParseStream(frs);
    for (const auto &word : doc["words"].GetArray()) {
        outs.emplace_back(word.GetString());
    }
    std::fclose(pfile);
    delete[] contents;
}


void get_themes_list(std::vector<std::string> &outs) {
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
    for (const auto &theme : doc.GetArray()) {
        outs.emplace_back(theme["name"].GetString());
    }
    std::fclose(pfile);
    delete[] contents;
}

/* will assign color ids up to base + 30, color pairs up to base + 32 */
void assign_theme(const std::int16_t &base, Theme &theme) {
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


void get_theme(const std::string &name, Theme &theme) {
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
    text = text.substr(beginb + 1, endb - beginb - 1);
    split(text, ";", fcolors);
    for (const std::string &f : fcolors) {
        std::size_t beginh = f.find('#') + 1, beginn = f.find("--") + 2;
        std::string cname = f.substr(beginn, f.find(':') - 6 - beginn); /* 6 is width of "-color" suffix */
        RGB color = strhex_to_rgb(f.substr(beginh));


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
            std::cerr << "fatal: get_theme: parsing theme " << name << " failed near \"" << f << "\" - color name \"" << cname << "\" not found\n";
            exit(1);
        }
    }
    /* just hope that the color ids don't conflict with terminal */
    assign_theme(static_cast<std::int16_t>(str_rdll("base_color_id", "get_theme")), theme);
}

void cleart(const Theme &theme) {
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
void addnamestr(const Theme &theme) {
    nccon(theme.text_pair);
    addstr("simian");
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

long double rounddouble(const long double &num, std::int32_t places) {
    const long double coeff = std::pow(10, places);
    return std::round(num * coeff) / coeff;
}

inline std::int64_t roundlong(const long double &num) {
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
        curs_set(1);
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
        nccon(theme.text_pair);
        attron(A_BOLD);
        addstr("simian: terminal monkeytype");
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
        std::vector<chinfo_t> buf;
        buf.reserve(out.size());
        for (char c : out) {
            buf.push_back(chinfo_t{.ch = c, .state = chstate::original});
        }

        std::atomic_int32_t p = 0;
        std::uint64_t start = 0;
        bool started = false;
        bool broken = false;
        std::int_fast32_t char_count = 0;

        curs_set(0);

        std::uint64_t begin_time = get_current_time_ns();
        std::vector<std::uint64_t> times = {begin_time}; /* also protected by term_mutex */
        std::mutex term_mutex;
        std::atomic_bool forwards = true;
        std::atomic_int pword = 0;
        std::bitset<words_limit> incorrect_words;
        std::atomic_int x = 0, y = 0;
        timeout(0);
        auto anitl = [&](std::stop_token stoken) {
            std::int32_t last_p = p;
            while (!stoken.stop_requested()) {
                while (last_p == p) {
                    if (stoken.stop_requested()) {
                        return;
                    }
                }
                if (last_p >= buf.size()) { return; }
                /* now they've changed */
                animate_caret(term_mutex, y, (last_p += static_cast<std::int16_t>(last_p < p) * 2 - 1), p - last_p, forwards, times, theme, buf, pword, incorrect_words, "mode words");
            }
        };
        std::jthread anit(anitl);
        while (p < buf.size()) {
            chtype chin = ERR;
            while (chin == ERR) {
                std::lock_guard guard(term_mutex);
                chin = getch();
            }
            {
                std::lock_guard guard(term_mutex);
                times.push_back(get_current_time_ns());
            }
            
            if (chin == KEY_DL || chin == '\t') {
                broken = true;
                break;
            }

            if (chin == '\n') {
                continue;
            }

            std::uint32_t spaces_end = 0;

            x = 0;
            y = 0;
            {
                std::lock_guard guard(term_mutex);
                getyx(pwin, y, x);
            }
            forwards = true;
            if (chin == KEY_BACKSPACE) {
                if (p <= 0) { continue; }
                if (buf[p - 1].state == chstate::err || buf[p - 1].state == chstate::correct) {
                    buf[p - 1].state = chstate::original;
                }
                if (buf[p].ch == ' ' && buf[p - 1].state == chstate::err_extra) {
                    buf.erase(buf.begin() + p - 1);
                    spaces_end++;
                }
                p--;
                forwards = false;
            } else {
                if (chin != buf[p].ch) {
                    if (chin == ' ') {
                        if (p > 0 ? buf[p - 1].ch == ' ' : true) {
                            continue;
                        }
                        std::int32_t k = p;
                        for (; k < buf.size() && buf[k].ch != ' '; k++) {;}
                        p = k;
                    } else if (p == buf.size() || buf[p].ch == ' ') {
                        buf.insert(buf.begin() + p, chinfo_t{.ch = static_cast<char>(chin), .state = chstate::err_extra});
                    } else {
                        char_count++;
                        buf[p].state = chstate::err;
                    }
                } else {
                    buf[p].state = chstate::correct;
                    char_count++;
                }

                if (!started) {
                    start = get_current_time_ns();
                    started = true;
                }

                p++;
            }


            decltype(buf) tbuf;
            /* both second halves of animation ignore last caret to decrease visual blinking of letters */
            {
                std::lock_guard guard(term_mutex);
                tbuf = buf;
            }
            std::int32_t tpword = 0;
            for (std::int32_t i = p - 1; i >= 0; i--) {
                if (tbuf[i].ch == ' ') {
                    tpword++;
                }
            }
            pword = tpword;

            std::bitset<words_limit> incorrect_words;
            std::int32_t cw = 0;
            std::int32_t j = 0;
            for (const chinfo_t &bchar : tbuf) {
                if (bchar.ch == ' ') {
                    cw++;
                    continue;
                }
                if (bchar.state == chstate::err || bchar.state == chstate::err_extra || (bchar.state == chstate::original && j < p - 1)) {
                    incorrect_words[cw] = true;
                }
                j++;
            }

            std::int32_t i = 0;
            cw = 0;
            for (const chinfo_t &bchar : tbuf) {
                if (bchar.ch == ' ') {
                    cw++;
                }

                {
                    std::lock_guard guard(term_mutex);
                    if (incorrect_words[cw] && cw < pword && bchar.ch != ' ') {
                        attron(A_UNDERLINE);
                    }
                    curs_set(0);
                    move(0, i);
                    outch(bchar, theme);
                    if (incorrect_words[cw] && cw < pword && bchar.ch != ' ') {
                        attroff(A_UNDERLINE);
                    }
                }
                i++;
            }
            {
                std::lock_guard guard(term_mutex);
                curs_set(0);
                move(0, i);
                addstr(std::string(spaces_end, ' ').c_str());
            }

            std::lock_guard guard(term_mutex);
            refresh();
        }

        anit.request_stop();
        anit.join();

        set_cursor_type(CursorType::steady_block);

        nccoff(theme.sub_pair);
        timeout(-1);

        /* this is actually the incorrect way to calculate it, check https://monkeytype.com/about */
        const long double wpm = static_cast<long double>(char_count) * (static_cast<long double>(std::nano::den * 60) / ((current_time() - start) * chars_per_word));

        std::ofstream logf(LOG_FILENAME, std::ios_base::app);
        logf << std::format("{:%FT%TZ}", std::chrono::system_clock::now()) << " | " << (broken ? "broken " : "") << "words " << words_limit << ": " << wpm << '\n';
        logf.close();

        return ask_again(pwin, broken, wpm, theme);
    }

    State zen(WINDOW *pwin, const Theme& theme) {
        cleart(theme);
        nccon(theme.main_pair);
        std::uint_fast32_t char_count = 0;
        std::uint64_t start = 0;
        std::uint16_t curx = 0;
        std::uint16_t cury = 0;
        std::vector<std::int32_t> line_lengths;

        chtype chin = '\0';
        bool started = false;
        std::int32_t this_line_length = 0;

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
                    this_line_length--;
                    move(cury, curx - 1);
                } else if (cury > 0) {
                    move(cury - 1, *line_lengths.rbegin());
                    this_line_length = *line_lengths.rbegin();
                    line_lengths.erase(line_lengths.end() - 1);
                }
            } else if (chin == '\n') {
                addnewline(pwin);
                line_lengths.push_back(this_line_length);
                this_line_length = 0;
            } else {
                addch(chin);
                this_line_length++;
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
        addnamestr(theme);
        addstr(" was designed to be as close to monkeytype as possible, with some caveats due to the terminal interface.");
        addnewline(pwin);
        addstr("this means that to exit most typing interfaces, [tab] can be used.");
        addnewline(pwin);
        addstr("alternatively, [shift] + [enter] may also be used in some circumstances.");
        addnewline(pwin);
        addstr("unfortunately, ");
        addnamestr(theme);
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


int main(int argc, char **argv) {
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

    bool needs_confirmation = false;
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
            needs_confirmation = true;
            continue;
        }

        if (opts.size() > 2) {
            wprintw(full_win, "warning: %s file %ith config option had %lu tokens ... using first two\n", CONFIG_FILENAME.c_str(), ocount, opts.size());
            needs_confirmation = true;
        }

        std::string name = opts[0], value = opts[1];
        /* wprintw(full_win, "name: %s, value: %s\n", name.c_str(), value.c_str()); */
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });

        if (!config.contains(name)) {
            needs_confirmation = true;
            std::optional<std::pair<std::string, double>> res = extract_one(name, configkeys);

            if (res.has_value()) {
                /* wprintw(full_win, "score: %f\n", res.value().second); */
                if (res.value().second < 70) {
                    wprintw(full_win, "warning: %s file %ith config option \"%s\" not found\n", CONFIG_FILENAME.c_str(), ocount, name.c_str());
                } else {
                    wprintw(full_win, "warning: %s file %ith config option \"%s\" not found, did you mean \"%s\"?\n", CONFIG_FILENAME.c_str(), ocount, name.c_str(), res.value().first.c_str());
                }
                continue;
            }

            wprintw(full_win, "warning: %s file %ith config option \"%s\" not found\n", CONFIG_FILENAME.c_str(), ocount, name.c_str());
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
    if (needs_confirmation) { getch(); }

    /* bool hc = str_rdb("hide_caret", "main"); */
    /* bool fc = str_rdb("smooth_caret", "main"); */

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

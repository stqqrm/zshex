// zshex-bin: minimal file explorer. UI is drawn on /dev/tty, keys are read
// from /dev/tty, and stdout carries only protocol lines for the zsh wrapper:
//     CD:<absolute path>
//
// Extra protocol line:  EDIT:<absolute path>   (binary exits; the wrapper opens
// the editor, then relaunches with --state FILE --select NAME)
//
// Build:  g++ -std=c++17 -O2 -o zshex-bin zshex.cpp

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------- terminal

static int tty_fd = -1;
static termios orig_tio;
static bool raw_on = false;

static void tty_write(const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t w = ::write(tty_fd, s.data() + off, s.size() - off);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
    }
}

static void term_restore() {
    if (!raw_on) return;
    raw_on = false;
    static const char seq[] = "\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";
    (void)!::write(tty_fd, seq, sizeof(seq) - 1);
    tcsetattr(tty_fd, TCSAFLUSH, &orig_tio);
}

static void on_signal(int) {
    term_restore();
    _exit(1);
}

static bool term_raw() {
    tty_fd = ::open("/dev/tty", O_RDWR);
    if (tty_fd < 0) return false;
    if (tcgetattr(tty_fd, &orig_tio) < 0) return false;
    termios t = orig_tio;
    t.c_iflag &= ~(IXON | ICRNL | INLCR | BRKINT | ISTRIP);
    t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(tty_fd, TCSAFLUSH, &t) < 0) return false;
    raw_on = true;
    std::atexit(term_restore);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGHUP, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
    tty_write("\x1b[?1049h\x1b[?25l\x1b[?7l");  // alt screen, hide cursor, no wrap
    return true;
}

static int term_rows() {
    winsize ws{};
    if (ioctl(tty_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    return 24;
}

// ------------------------------------------------------------------- keys

enum Key {
    K_EOF = -2, K_ESC = 1000, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_PGUP, K_PGDN, K_HOME, K_END
};

static bool wait_byte(int ms) {
    pollfd p{tty_fd, POLLIN, 0};
    return poll(&p, 1, ms) > 0;
}

static int read_key() {
    unsigned char c;
    if (::read(tty_fd, &c, 1) != 1) return K_EOF;
    if (c != 0x1b) return c;

    // Esc: bare Esc, or the start of an escape sequence
    if (!wait_byte(50)) return K_ESC;
    unsigned char b;
    if (::read(tty_fd, &b, 1) != 1) return K_ESC;
    if (b != '[' && b != 'O') return K_ESC;

    std::string seq;
    while (wait_byte(50)) {
        unsigned char x;
        if (::read(tty_fd, &x, 1) != 1) break;
        seq += static_cast<char>(x);
        if (std::isalpha(x) || x == '~') break;
    }
    if (seq == "A") return K_UP;
    if (seq == "B") return K_DOWN;
    if (seq == "C") return K_RIGHT;
    if (seq == "D") return K_LEFT;
    if (seq == "5~") return K_PGUP;
    if (seq == "6~") return K_PGDN;
    if (seq == "H" || seq == "1~" || seq == "7~") return K_HOME;
    if (seq == "F" || seq == "4~" || seq == "8~") return K_END;
    return K_ESC + 100;  // unknown sequence: ignored
}

// -------------------------------------------------------------------- app

struct Entry {
    std::string name;
    bool is_dir = false;   // follows symlinks
    bool is_link = false;
    bool is_exec = false;
    // stat info (lstat)
    bool stat_ok = true;
    std::string perm, nlink, owner, group, size, age;
    std::string target;            // symlink target text
    bool target_broken = false, target_dir = false, target_exec = false;
    bool statted = false;          // deep search: stat info filled in lazily
    std::string info;              // padded "perms links owner group size age "
};

static std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Fuzzy (subsequence) match. On success, [s, e] is the tightest window
// in `c` that contains the characters of `q` in order.
static bool fuzzy_span(const std::string& c, const std::string& q, size_t& s, size_t& e) {
    if (q.empty()) { s = e = 0; return true; }
    size_t qi = 0, i = 0;
    for (; i < c.size() && qi < q.size(); i++)
        if (c[i] == q[qi]) qi++;
    if (qi < q.size()) return false;
    e = i - 1;
    // walk backwards from the end to tighten the window
    size_t qj = q.size(), j = e + 1;
    while (qj > 0) {
        j--;
        if (c[j] == q[qj - 1]) qj--;
    }
    s = j;
    return true;
}

static std::string perm_string(mode_t m) {
    std::string r(10, '-');
    if (S_ISDIR(m)) r[0] = 'd';
    else if (S_ISLNK(m)) r[0] = 'l';
    else if (S_ISCHR(m)) r[0] = 'c';
    else if (S_ISBLK(m)) r[0] = 'b';
    else if (S_ISFIFO(m)) r[0] = 'p';
    else if (S_ISSOCK(m)) r[0] = 's';
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        if (m & (1u << (8 - i))) r[1 + i] = rwx[i];
    if (m & S_ISUID) r[3] = (m & S_IXUSR) ? 's' : 'S';
    if (m & S_ISGID) r[6] = (m & S_IXGRP) ? 's' : 'S';
    if (m & S_ISVTX) r[9] = (m & S_IXOTH) ? 't' : 'T';
    return r;
}

static std::string human_size(double v) {
    static const char* u[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int i = 0;
    double d = v;
    while (d >= 1024 && i < 5) { d /= 1024; i++; }
    char buf[32];
    if (i == 0) std::snprintf(buf, sizeof buf, "%.0fB", v);
    else std::snprintf(buf, sizeof buf, "%.1f%s", d, u[i]);
    return buf;
}

static std::string ago(long s) {
    if (s < 0) s = 0;
    if (s < 60) return std::to_string(s) + "s ago";
    if (s < 3600) return std::to_string(s / 60) + "m ago";
    if (s < 86400) return std::to_string(s / 3600) + "h ago";
    if (s < 2592000) return std::to_string(s / 86400) + "d ago";
    if (s < 31536000) return std::to_string(s / 2592000) + "mo ago";
    return std::to_string(s / 31536000) + "y ago";
}

static std::string lpad(const std::string& s, size_t w) { return s.size() >= w ? s : std::string(w - s.size(), ' ') + s; }
static std::string rpad(const std::string& s, size_t w) { return s.size() >= w ? s : s + std::string(w - s.size(), ' '); }

static void fill_stat(Entry& e, time_t now) {
    struct stat st;
    if (lstat(e.name.c_str(), &st) != 0) {
        e.stat_ok = false;
        e.perm = "----------"; e.nlink = "0"; e.owner = "?"; e.group = "?";
        e.size = "0B"; e.age = "0s ago";
        return;
    }
    e.perm = perm_string(st.st_mode);
    e.nlink = std::to_string(static_cast<unsigned long>(st.st_nlink));
    // cached: recursive search stats thousands of files
    static std::map<uid_t, std::string> users;
    static std::map<gid_t, std::string> groups;
    auto u = users.find(st.st_uid);
    if (u == users.end()) {
        passwd* pw = getpwuid(st.st_uid);
        u = users.emplace(st.st_uid, pw ? std::string(pw->pw_name) : std::to_string(st.st_uid)).first;
    }
    e.owner = u->second;
    auto g = groups.find(st.st_gid);
    if (g == groups.end()) {
        group* gr = getgrgid(st.st_gid);
        g = groups.emplace(st.st_gid, gr ? std::string(gr->gr_name) : std::to_string(st.st_gid)).first;
    }
    e.group = g->second;
    e.size = human_size(static_cast<double>(st.st_size));
    e.age = ago(static_cast<long>(now - st.st_mtime));
    if (S_ISLNK(st.st_mode)) {
        char buf[4096];
        ssize_t r = readlink(e.name.c_str(), buf, sizeof buf);
        if (r > 0) e.target.assign(buf, static_cast<size_t>(r));
        struct stat ts;
        if (stat(e.name.c_str(), &ts) != 0) e.target_broken = true;
        else {
            e.target_dir = S_ISDIR(ts.st_mode);
            e.target_exec = S_ISREG(ts.st_mode) && (ts.st_mode & 0111);
        }
    }
}

static std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    return s;
}

static void send_edit(const fs::path& p) {
    std::fputs("EDIT:", stdout);
    std::fputs(p.string().c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

static void send_cd(const fs::path& p) {
    std::fputs("CD:", stdout);
    std::fputs(p.string().c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

struct App {
    std::vector<Entry> all;
    std::vector<int> idx;      // indices into `all` currently shown
    int sel = 0, top = 0, win = 1;
    std::string last_query;    // for n / N
    int search_dir = 1;        // +1 after "/", -1 after "?"
    bool done = false;         // set when a file was handed to the editor
    bool deep_mode = false;    // recursive search active: stat info is lazy
    bool scanning = false;     // recursive search still reading directories
    std::string state_path;    // --state: history/memory kept across relaunches
    std::string init_select;   // --select: entry to highlight on start
    std::vector<std::string> hb, hf;             // directory history back / forward
    std::map<std::string, std::string> memory;   // dir -> last selected name

    void load() {
        all.clear();
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);

        std::vector<Entry> hidden, visible;
        for (fs::directory_iterator it(".", ec), end; !ec && it != end; it.increment(ec)) {
            Entry e;
            e.name = it->path().filename().string();
            std::error_code e1;
            auto ls = fs::symlink_status(it->path(), e1);
            e.is_link = fs::is_symlink(ls);
            auto st = fs::status(it->path(), e1);
            e.is_dir = fs::is_directory(st);
            e.is_exec = fs::is_regular_file(st) &&
                        (st.permissions() & (fs::perms::owner_exec | fs::perms::group_exec |
                                             fs::perms::others_exec)) != fs::perms::none;
            (e.name[0] == '.' ? hidden : visible).push_back(e);
        }
        auto cmp = [](const Entry& a, const Entry& b) {
            std::string x = lower(a.name), y = lower(b.name);
            return x != y ? x < y : a.name < b.name;
        };
        std::sort(hidden.begin(), hidden.end(), cmp);
        std::sort(visible.begin(), visible.end(), cmp);

        if (cwd != cwd.root_path()) { Entry up; up.name = ".."; up.is_dir = true; all.push_back(up); }
        all.insert(all.end(), hidden.begin(), hidden.end());
        all.insert(all.end(), visible.begin(), visible.end());

        time_t now = std::time(nullptr);
        size_t wnl = 0, wown = 0, wgrp = 0, wsize = 0, wage = 0;
        for (Entry& e : all) {
            fill_stat(e, now);
            wnl = std::max(wnl, e.nlink.size());
            wown = std::max(wown, e.owner.size());
            wgrp = std::max(wgrp, e.group.size());
            wsize = std::max(wsize, e.size.size());
            wage = std::max(wage, e.age.size());
        }
        for (Entry& e : all)
            e.info = e.perm + "  " + lpad(e.nlink, wnl) + " " + rpad(e.owner, wown) + " " +
                     rpad(e.group, wgrp) + " " + lpad(e.size, wsize) + "  " + lpad(e.age, wage) + " ";
        show_all();
        auto it = memory.find(cwd.string());
        if (it != memory.end()) { select_name(it->second); center(); }
    }

    void save_state() {
        if (state_path.empty()) return;
        remember();
        std::ofstream f(state_path);
        if (!f) return;
        for (auto& d : hb) f << "B\t" << d << '\n';
        for (auto& d : hf) f << "F\t" << d << '\n';
        for (auto& m : memory) f << "M\t" << m.first << '\t' << m.second << '\n';
        f << "Q\t" << last_query << '\n';
        f << "D\t" << search_dir << '\n';
    }

    void load_state() {
        if (state_path.empty()) return;
        std::ifstream f(state_path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.size() < 2 || line[1] != '\t') continue;
            std::string v = line.substr(2);
            switch (line[0]) {
                case 'B': hb.push_back(v); break;
                case 'F': hf.push_back(v); break;
                case 'M': {
                    size_t t = v.find('\t');
                    if (t != std::string::npos) memory[v.substr(0, t)] = v.substr(t + 1);
                    break;
                }
                case 'Q': last_query = v; break;
                case 'D': search_dir = (v == "-1") ? -1 : 1; break;
            }
        }
    }

    void remember() {
        if (idx.empty()) return;
        std::error_code ec;
        memory[fs::current_path(ec).string()] = all[idx[sel]].name;
    }

    // put the selection in the middle of the window when possible
    void center() {
        int w = std::max(3, term_rows() - 2);
        int n = static_cast<int>(idx.size());
        top = std::max(0, std::min(sel - w / 2, n - w));
    }

    void show_all() {
        idx.resize(all.size());
        for (size_t i = 0; i < all.size(); i++) idx[i] = static_cast<int>(i);
        sel = top = 0;
    }

    void select_name(const std::string& name) {
        for (size_t i = 0; i < idx.size(); i++)
            if (all[idx[i]].name == name) { sel = static_cast<int>(i); return; }
    }

    bool change_dir(const std::string& target) {
        std::error_code ec;
        fs::path from = fs::current_path(ec);
        remember();
        fs::current_path(target, ec);
        if (ec) return false;
        fs::path now = fs::current_path(ec);
        if (now != from) { hb.push_back(from.string()); hf.clear(); }
        send_cd(now);
        load();
        return true;
    }

    // Ctrl-O (back = true) / Ctrl-I (back = false)
    void hist(bool back) {
        auto& from = back ? hb : hf;
        auto& to = back ? hf : hb;
        while (!from.empty()) {
            std::string target = from.back();
            from.pop_back();
            std::error_code ec;
            fs::path cur = fs::current_path(ec);
            remember();
            fs::current_path(target, ec);
            if (ec) continue;  // directory vanished: try the next one
            to.push_back(cur.string());
            send_cd(fs::current_path(ec));
            load();
            return;
        }
    }

    // gd: follow a symlink; directory -> cd into it, file -> select it in its dir
    void follow() {
        if (idx.empty()) return;
        const Entry& e = all[idx[sel]];
        if (!e.is_link) return;
        std::error_code ec;
        fs::path t = fs::canonical(e.name, ec);
        if (ec) return;
        if (fs::is_directory(t, ec)) {
            change_dir(t.string());
        } else {
            std::string name = t.filename().string();
            if (change_dir(t.parent_path().string())) { select_name(name); center(); }
        }
    }

    void go_up() {
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        if (cwd == cwd.root_path()) return;
        std::string from = cwd.filename().string();
        if (change_dir("..")) { select_name(from); center(); }
    }

    void enter() {
        if (idx.empty()) return;
        const Entry& e = all[idx[sel]];
        if (!e.is_dir) {
            std::error_code ec;
            send_edit(fs::current_path(ec) / e.name);
            done = true;
            return;
        }
        std::string name = e.name;  // copy: load() invalidates the reference
        if (name == "..") go_up();
        else change_dir(name);
    }

    void move(int d) {
        if (idx.empty()) { sel = 0; return; }
        sel = std::clamp(sel + d, 0, static_cast<int>(idx.size()) - 1);
    }

    // n / N: jump to next / previous fuzzy match in the full list
    void next_match(int step) {
        if (last_query.empty() || all.empty()) return;
        std::string q = lower(last_query);
        int n = static_cast<int>(all.size());
        for (int i = 1; i <= n; i++) {
            int k = ((sel + step * i) % n + n) % n;
            size_t s, e;
            if (fuzzy_span(lower(all[k].name), q, s, e)) { sel = k; return; }
        }
    }

    void draw(const std::string& prompt, bool searching) {
        win = std::max(3, term_rows() - 2);
        int n = static_cast<int>(idx.size());
        if (sel >= n) sel = n - 1;
        if (sel < 0) sel = 0;
        if (sel < top) top = sel;
        if (sel >= top + win) top = sel - win + 1;

        std::error_code ec;
        std::string cwd = fs::current_path(ec).string();
        if (const char* home = std::getenv("HOME")) {
            std::string h = home;
            if (!h.empty() && cwd.compare(0, h.size(), h) == 0 &&
                (cwd.size() == h.size() || cwd[h.size()] == '/'))
                cwd = "~" + cwd.substr(h.size());
        }

        std::string b = "\x1b[?25l\x1b[H\x1b[2K\x1b[91m";
        if (searching) b += prompt;
        else b += " zshex  " + cwd + "  [" + std::to_string(n ? sel + 1 : 0) + "/" + std::to_string(n) + "]";
        b += "\x1b[0m\r\n";

        // recursive search: stat only the visible rows, size columns to them
        size_t wnl = 0, wown = 0, wgrp = 0, wsize = 0, wage = 0;
        if (deep_mode) {
            time_t now = std::time(nullptr);
            for (int i = top; i < std::min(n, top + win); i++) {
                Entry& e = all[idx[i]];
                if (!e.statted) {
                    fill_stat(e, now);
                    e.statted = true;
                    auto x = [&](int k) { char c = e.perm[k]; return c == 'x' || c == 's' || c == 't'; };
                    e.is_exec = !e.is_link && e.perm[0] == '-' && (x(3) || x(6) || x(9));
                }
                wnl = std::max(wnl, e.nlink.size());
                wown = std::max(wown, e.owner.size());
                wgrp = std::max(wgrp, e.group.size());
                wsize = std::max(wsize, e.size.size());
                wage = std::max(wage, e.age.size());
            }
        }

        for (int i = top; i < top + win; i++) {
            b += "\x1b[2K";
            if (i < n) {
                const Entry& e = all[idx[i]];
                std::string info = !deep_mode ? e.info
                    : e.perm + "  " + lpad(e.nlink, wnl) + " " + rpad(e.owner, wown) + " " +
                      rpad(e.group, wgrp) + " " + lpad(e.size, wsize) + "  " + lpad(e.age, wage) + " ";
                const char* color = (e.is_link && e.target_broken) ? "\x1b[91m"
                                  : e.is_link ? "\x1b[36m" : e.is_dir ? "\x1b[34m"
                                  : e.is_exec ? "\x1b[92m" : "";
                std::string ind = e.name == ".." ? "" : e.is_link ? "@" : e.is_dir ? "/"
                                : e.is_exec ? "*" : "";
                std::string line;
                if (!e.stat_ok) line += "\x1b[90m" + info + "\x1b[0m";
                else line += info;
                line += std::string(color) + e.name + ind + "\x1b[0m";
                if (e.is_link && !e.target.empty()) {
                    std::string tind = e.target_broken ? "@" : e.target_dir ? "/" : e.target_exec ? "*" : "";
                    line += " > " + std::string(color) + e.target + tind + "\x1b[0m";
                }
                if (i == sel) {
                    line = replace_all(line, "\x1b[0m", "\x1b[0m\x1b[7m");
                    b += "\x1b[7m " + line + "\x1b[K\x1b[0m";
                } else {
                    b += " " + line;
                }
            }
            b += "\r\n";
        }
        b += "\x1b[2K\x1b[90m";
        if (searching) b += scanning ? " enter open  up/down move  esc cancel  (scanning...)"
                                     : " enter open  up/down move  esc cancel";
        else b += " j/k move  enter/l open (dir: cd, file: editor)  -/h up  gd follow link  ^O ^I back/fwd  / ? fuzzy search (recursive)  n/N next/prev  q close";
        b += "\x1b[0m\x1b[J";
        if (searching) {
            size_t w = 0;
            for (unsigned char c : prompt) if ((c & 0xC0) != 0x80) w++;
            b += "\x1b[1;" + std::to_string(w + 1) + "H\x1b[?25h";
        }
        tty_write(b);
    }

    // Incremental recursive walk (breadth-first, so shallow entries come
    // first). Uses readdir's d_type, so no stat() per entry is needed;
    // permissions/owner/size are looked up lazily for visible rows in draw().
    struct Walker {
        std::vector<std::string> dirs{""};
        size_t next = 0;
        bool done() const { return next >= dirs.size(); }
    };

    // Reads whole directories until `budget_ms` has passed (always at least
    // one directory) or the walk is finished. Appends to `out`.
    static void walk_step(Walker& w, std::vector<Entry>& out, int budget_ms) {
        const size_t kMax = 30000;
        auto t0 = std::chrono::steady_clock::now();
        while (!w.done()) {
            if (out.size() >= kMax) { w.next = w.dirs.size(); break; }
            const std::string dir = w.dirs[w.next++];
            if (DIR* d = opendir(dir.empty() ? "." : dir.c_str())) {
                while (dirent* de = readdir(d)) {
                    const char* nm = de->d_name;
                    if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2]))) continue;
                    std::string base = nm;
                    Entry e;
                    e.name = dir.empty() ? base : dir + "/" + base;
                    bool link = de->d_type == DT_LNK, isdir = de->d_type == DT_DIR;
                    struct stat st;
                    if (de->d_type == DT_UNKNOWN && lstat(e.name.c_str(), &st) == 0) {
                        link = S_ISLNK(st.st_mode);
                        isdir = S_ISDIR(st.st_mode);
                    }
                    e.is_link = link;
                    e.is_dir = isdir;
                    if (link && stat(e.name.c_str(), &st) == 0) e.is_dir = S_ISDIR(st.st_mode);
                    // real directories only: symlinked dirs are not followed
                    if (isdir && base != ".git" && base != "node_modules") w.dirs.push_back(e.name);
                    out.push_back(std::move(e));
                }
                closedir(d);
            }
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            if (ms >= budget_ms) break;
        }
    }

    // Open a search result. `picked` may be a path relative to the cwd.
    void open_picked(const std::string& picked) {
        size_t sl = picked.rfind('/');
        if (sl == std::string::npos) { enter(); return; }
        std::error_code ec;
        if (fs::is_directory(picked, ec)) { change_dir(picked); return; }
        std::string parent = picked.substr(0, sl), base = picked.substr(sl + 1);
        if (change_dir(parent)) { select_name(base); center(); enter(); }
    }

    // Live recursive fuzzy filter over everything below the cwd. The tree is
    // scanned in small slices between keypresses, so the UI never blocks.
    // Returns true if the user pressed Enter on a match; `picked` is then its
    // path relative to the cwd.
    bool search(char key, std::string& picked) {
        std::vector<Entry> saved = std::move(all);
        all.clear();
        deep_mode = true;
        auto finish = [&]() { deep_mode = false; scanning = false; all = std::move(saved); show_all(); };

        Walker w;
        walk_step(w, all, 0);  // the cwd itself, so the list shows up at once
        scanning = !w.done();

        std::vector<std::string> lbase, lpath;
        std::vector<int> ldepth;
        auto sync = [&]() {
            for (size_t i = lpath.size(); i < all.size(); i++) {
                ldepth.push_back(static_cast<int>(std::count(all[i].name.begin(), all[i].name.end(), '/')));
                std::string lp = lower(all[i].name);
                size_t sl = lp.rfind('/');
                lbase.push_back(sl == std::string::npos ? lp : lp.substr(sl + 1));
                lpath.push_back(std::move(lp));
            }
        };
        sync();

        std::string q;
        int s0 = sel, t0 = top;
        bool moved = false, from_walk = false;
        while (true) {
            // when the list is only being refreshed because the scan found
            // more entries, keep the selection if the user has moved it
            int keep = (from_walk && moved && !idx.empty()) ? idx[sel] : -1;
            from_walk = false;

            std::string lq = lower(q);
            // a trailing "/" means "directories only" (the "/" shown after
            // directory names is just a marker, it is not part of the name)
            bool dirs_only = !lq.empty() && lq.back() == '/';
            if (dirs_only) lq.pop_back();
            // a "/" inside the query matches against the whole relative path
            bool by_path = lq.find('/') != std::string::npos;
            // rank: literal (substring) matches before fuzzy ones, then
            // nearest to the cwd (fewest path components), then tier (exact <
            // prefix < substring of name < substring of path < fuzzy name <
            // fuzzy path), then fewest extra characters, then earliest match
            // position, then list order
            struct Hit { int cls, depth, tier; size_t extra, pos; int i; };
            std::vector<Hit> hits;
            for (size_t i = 0; i < all.size(); i++) {
                int ii = static_cast<int>(i);
                int depth = ldepth[i];
                if (dirs_only && !all[i].is_dir) continue;
                if (lq.empty()) {
                    // empty query: just the current directory's entries
                    if (depth == 0) hits.push_back({0, 0, 0, 0, 0, ii});
                    continue;
                }
                const std::string& b = by_path ? lpath[i] : lbase[i];
                size_t pos = b.find(lq);
                if (pos != std::string::npos) {
                    int tier = b == lq ? 0 : pos == 0 ? 1 : 2;
                    hits.push_back({0, depth, tier, b.size() - lq.size(), pos, ii});
                    continue;
                }
                // substring somewhere else in the path (e.g. a directory name)
                if (!by_path) {
                    size_t pp = lpath[i].find(lq);
                    if (pp != std::string::npos) {
                        hits.push_back({0, depth, 3, lpath[i].size() - lq.size(), pp, ii});
                        continue;
                    }
                }
                size_t s, e;
                if (fuzzy_span(b, lq, s, e))
                    hits.push_back({1, depth, 4, (e - s + 1) - lq.size(), s, ii});
                else if (fuzzy_span(lpath[i], lq, s, e))
                    hits.push_back({1, depth, 5, (e - s + 1) - lq.size(), s, ii});
            }
            std::stable_sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) {
                if (x.cls != y.cls) return x.cls < y.cls;
                if (x.depth != y.depth) return x.depth < y.depth;
                if (x.tier != y.tier) return x.tier < y.tier;
                if (x.extra != y.extra) return x.extra < y.extra;
                return x.pos < y.pos;
            });
            idx.clear();
            for (const Hit& h : hits) idx.push_back(h.i);
            sel = top = 0;
            if (keep >= 0)
                for (size_t j = 0; j < idx.size(); j++)
                    if (idx[j] == keep) { sel = static_cast<int>(j); break; }

            bool requery = false;
            while (!requery) {
                draw(std::string(1, key) + q, true);

                // wait for a key; while none is pending, scan a little more
                int k = 0;
                bool have_key = false;
                while (!have_key && !requery) {
                    if (scanning && !wait_byte(0)) {
                        size_t before = all.size();
                        walk_step(w, all, 8);
                        scanning = !w.done();
                        sync();
                        if (!lq.empty() && (all.size() != before || !scanning)) requery = from_walk = true;
                        else if (!scanning) draw(std::string(1, key) + q, true);  // drop the "scanning" note
                    } else {
                        k = read_key();
                        have_key = true;
                    }
                }
                if (requery) break;

                if (k == K_EOF || k == K_ESC) { finish(); sel = s0; top = t0; return false; }
                if (k == '\n' || k == '\r') {
                    if (idx.empty()) { finish(); sel = s0; top = t0; return false; }
                    picked = all[idx[sel]].name;
                    last_query = dirs_only ? q.substr(0, q.size() - 1) : q;
                    finish();
                    select_name(picked);
                    return true;
                }
                switch (k) {
                    case K_UP: case 0x10: move(-1); moved = true; break;
                    case K_DOWN: case 0x0e: move(1); moved = true; break;
                    case K_PGUP: move(-win); moved = true; break;
                    case K_PGDN: move(win); moved = true; break;
                    case K_HOME: sel = 0; moved = true; break;
                    case K_END: sel = idx.empty() ? 0 : static_cast<int>(idx.size()) - 1; moved = true; break;
                    case 0x7f: case 0x08:
                        while (!q.empty() && (static_cast<unsigned char>(q.back()) & 0xC0) == 0x80) q.pop_back();
                        if (!q.empty()) q.pop_back();
                        requery = true;
                        moved = false;
                        break;
                    case 0x15: q.clear(); requery = true; moved = false; break;
                    default:
                        if ((k >= 32 && k < 127) || (k >= 128 && k < 256)) {
                            q += static_cast<char>(k);
                            requery = true;
                            moved = false;
                        }
                }
            }
        }
    }

    void run() {
        load_state();
        load();
        if (!init_select.empty()) { select_name(init_select); center(); }
        while (true) {
            draw("", false);
            int k = read_key();
            switch (k) {
                case K_EOF: case 'q': case 0x05: case K_ESC: return;  // q, Ctrl-E, Esc
                case 'j': case K_DOWN: move(1); break;
                case 'k': case K_UP: move(-1); break;
                case K_PGDN: move(win); break;
                case K_PGUP: move(-win); break;
                case 'G': case K_END: sel = static_cast<int>(idx.size()) - 1; break;
                case K_HOME: sel = 0; break;
                case 'g': {
                    int k2 = read_key();
                    if (k2 == 'g') sel = 0;
                    else if (k2 == 'd') follow();
                    break;
                }
                case 0x0f: hist(true); break;    // Ctrl-O
                case '\t': hist(false); break;   // Ctrl-I
                case '~': { const char* h = std::getenv("HOME"); if (h) change_dir(h); break; }
                case '-': case 'h': case K_LEFT: case 0x7f: case 0x08: go_up(); break;
                case 'l': case K_RIGHT: case '\n': case '\r': enter(); break;
                case '/': case '?':
                    search_dir = (k == '?') ? -1 : 1;
                    {
                        std::string picked;
                        if (search(static_cast<char>(k), picked)) open_picked(picked);
                    }
                    break;
                case 'n': next_match(search_dir); break;
                case 'N': next_match(-search_dir); break;
            }
            if (done) return;
        }
    }
};

int main(int argc, char** argv) {
    App app;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--state" && i + 1 < argc) app.state_path = argv[++i];
        else if (a == "--select" && i + 1 < argc) app.init_select = argv[++i];
    }
    if (!term_raw()) {
        std::fputs("zshex-bin: cannot open /dev/tty\n", stderr);
        return 1;
    }
    app.run();
    app.save_state();
    return 0;
}

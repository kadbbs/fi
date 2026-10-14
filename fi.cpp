#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <poll.h>
#include <utility>

void die(const char* msg);
void write_all(const char* data, size_t size);

// ─── 终端配置 RAII ───────────────────────────────────────────
struct TermiosGuard {
    termios orig;
    bool active = false;

    TermiosGuard() {
        if (tcgetattr(STDIN_FILENO, &orig) == -1) die("tcgetattr");
        enable_raw();
    }

    void enable_raw() {
        if (active) return;
        termios raw = orig;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~(OPOST);
        raw.c_cflag |= (CS8);
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
        active = true;
    }

    void restore() {
        if (!active) return;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig) == -1) die("tcsetattr");
        active = false;
    }

    ~TermiosGuard() {
        if (active && tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig) == -1)
            perror("tcsetattr");
    }
    TermiosGuard(const TermiosGuard&) = delete;
    TermiosGuard& operator=(const TermiosGuard&) = delete;
};

// ─── 按键枚举 ────────────────────────────────────────────────
enum Key {
    BACKSPACE = 127,
    ARROW_UP    = 1000,
    ARROW_DOWN,
    ARROW_LEFT,
    ARROW_RIGHT,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

enum class InputMode {
    Editing,
    Command,
    ConfirmQuit,
    ConfirmOpen,
};

// ─── 全局状态 ────────────────────────────────────────────────
struct Buffer {
    std::vector<std::string> rows;
    int cx = 0, cy = 0;
    int row_offset = 0, col_offset = 0;
    std::string filename;
    bool dirty = false;
    int num_rows = 0;
};

static Buffer buf;
static int screen_rows, screen_cols;
static InputMode input_mode = InputMode::Editing;
static std::string command_line;
static std::string status_message;
static std::string pending_filename;

// ─── 辅助函数 ────────────────────────────────────────────────
void die(const char* msg) {
    perror(msg);
    exit(1);
}

void write_all(const char* data, size_t size) {
    while (size > 0) {
        const ssize_t written = write(STDOUT_FILENO, data, size);
        if (written == -1 && errno == EINTR) continue;
        if (written <= 0) die("write");
        data += written;
        size -= static_cast<size_t>(written);
    }
}

bool read_byte(unsigned char* byte, int timeout_ms) {
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    int result;
    do {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result == -1 && errno == EINTR);
    if (result == -1) die("poll");
    if (result == 0) return false;

    ssize_t nread;
    do {
        nread = read(STDIN_FILENO, byte, 1);
    } while (nread == -1 && errno == EINTR);
    if (nread != 1) die("read");
    return true;
}

int get_cursor_pos(int* rows, int* cols) {
    char seq[32];
    unsigned int i = 0;
    write_all("\x1b[6n", 4);
    while (i < sizeof(seq) - 1) {
        unsigned char byte;
        if (!read_byte(&byte, 1000)) break;
        seq[i] = static_cast<char>(byte);
        if (seq[i] == 'R') break;
        i++;
    }
    seq[i] = '\0';
    if (seq[0] != '\x1b' || seq[1] != '[') return -1;
    if (sscanf(&seq[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

void get_window_size() {
    winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 ||
        ws.ws_col == 0 || ws.ws_row < 2) {
        write_all("\x1b[999C\x1b[999B", 12);
        if (get_cursor_pos(&screen_rows, &screen_cols) == -1 ||
            screen_rows < 2 || screen_cols < 1) {
            screen_rows = 24;
            screen_cols = 80;
        }
        screen_rows--;
    } else {
        screen_cols = ws.ws_col;
        screen_rows = ws.ws_row - 1;
    }
}

// ─── 文件操作 ────────────────────────────────────────────────
bool open_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        if (access(path.c_str(), F_OK) == 0 || errno != ENOENT) return false;
    }
    Buffer opened;
    opened.filename = path;
    std::string line;
    while (ifs.is_open() && std::getline(ifs, line)) {
        opened.rows.push_back(line);
    }
    if (ifs.bad()) return false;
    if (opened.rows.empty()) opened.rows.push_back("");
    opened.num_rows = static_cast<int>(opened.rows.size());
    buf = std::move(opened);
    status_message.clear();
    return true;
}

bool save_file(const std::string& path = "") {
    const std::string destination = path.empty() ? buf.filename : path;
    if (destination.empty()) {
        status_message = "No file name; use :w <file>";
        return false;
    }
    std::ofstream ofs(destination);
    if (!ofs.is_open()) {
        status_message = "Cannot write file: " + destination;
        return false;
    }
    for (int i = 0; i < buf.num_rows; i++) {
        ofs << buf.rows[i];
        if (i != buf.num_rows - 1) ofs << '\n';
    }
    ofs << '\n';
    if (!ofs) {
        status_message = "Write failed: " + destination;
        return false;
    }
    buf.filename = destination;
    buf.dirty = false;
    status_message = "Saved " + destination;
    return true;
}

// ─── 输入处理 ────────────────────────────────────────────────
int read_key() {
    unsigned char key;
    ssize_t nread;
    do {
        nread = read(STDIN_FILENO, &key, 1);
    } while (nread == -1 && errno == EINTR);
    if (nread != 1) die("read");
    int c = key;
    if (c == '\x1b') {
        unsigned char seq[3];
        if (!read_byte(&seq[0], 30)) return '\x1b';
        if (!read_byte(&seq[1], 30)) return '\x1b';
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (!read_byte(&seq[2], 30)) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    }
    return c;
}

// ─── 光标移动 ────────────────────────────────────────────────
void move_cursor(int key) {
    if (key == ARROW_LEFT) {
        if (buf.cx > 0) {
            buf.cx--;
        } else if (buf.cy > 0) {
            buf.cy--;
            buf.cx = buf.rows[buf.cy].size();
        }
    } else if (key == ARROW_RIGHT) {
        if (buf.cx < static_cast<int>(buf.rows[buf.cy].size())) {
            buf.cx++;
        } else if (buf.cy < buf.num_rows - 1) {
            buf.cy++;
            buf.cx = 0;
        }
    } else {
        const int target_row = key == ARROW_UP ? buf.cy - 1 : buf.cy + 1;
        if (target_row < 0 || target_row >= buf.num_rows) return;
        buf.cy = target_row;
        buf.cx = std::min(buf.cx, static_cast<int>(buf.rows[buf.cy].size()));
    }
}

// ─── 编辑操作 ────────────────────────────────────────────────
void insert_char(int c) {
    if (buf.cy == buf.num_rows) {
        buf.rows.push_back("");
        buf.num_rows++;
        buf.cx = 0;
    }
    buf.rows[buf.cy].insert(buf.cx, 1, (char)c);
    buf.cx++;
    buf.dirty = true;
}

void insert_newline() {
    if (buf.cx == 0) {
        buf.rows.insert(buf.rows.begin() + buf.cy, "");
    } else {
        std::string& row = buf.rows[buf.cy];
        std::string new_row = row.substr(buf.cx);
        row.erase(buf.cx);
        buf.rows.insert(buf.rows.begin() + buf.cy + 1, new_row);
    }
    buf.num_rows++;
    buf.cy++;
    buf.cx = 0;
    buf.dirty = true;
}

void del_char() {
    if (buf.cy == buf.num_rows) return;
    if (buf.cx == 0 && buf.cy == 0) return;
    if (buf.cx > 0) {
        buf.rows[buf.cy].erase(buf.cx - 1, 1);
        buf.cx--;
    } else {
        int prev_len = buf.rows[buf.cy - 1].size();
        buf.rows[buf.cy - 1] += buf.rows[buf.cy];
        buf.rows.erase(buf.rows.begin() + buf.cy);
        buf.num_rows--;
        buf.cy--;
        buf.cx = prev_len;
    }
    buf.dirty = true;
}

void delete_char() {
    if (buf.cy >= buf.num_rows) return;
    std::string& row = buf.rows[buf.cy];
    if (buf.cx < static_cast<int>(row.size())) {
        row.erase(buf.cx, 1);
    } else if (buf.cy < buf.num_rows - 1) {
        row += buf.rows[buf.cy + 1];
        buf.rows.erase(buf.rows.begin() + buf.cy + 1);
        buf.num_rows--;
    } else {
        return;
    }
    buf.dirty = true;
}

// ─── 渲染 ────────────────────────────────────────────────────
void scroll() {
    if (buf.cy < buf.row_offset) buf.row_offset = buf.cy;
    if (buf.cy >= buf.row_offset + screen_rows)
        buf.row_offset = buf.cy - screen_rows + 1;
    if (buf.cx < buf.col_offset) buf.col_offset = buf.cx;
    if (buf.cx >= buf.col_offset + screen_cols)
        buf.col_offset = buf.cx - screen_cols + 1;
}

void draw_rows(std::string& ab) {
    for (int y = 0; y < screen_rows; y++) {
        int filerow = y + buf.row_offset;
        if (filerow >= buf.num_rows) {
            if (buf.num_rows == 0 && y == screen_rows / 3) {
                char welcome[80];
                int len = snprintf(welcome, sizeof(welcome),
                    "Tiny Editor -- no args");
                if (len > screen_cols) len = screen_cols;
                int padding = (screen_cols - len) / 2;
                if (padding) { ab += "~"; padding--; }
                while (padding--) ab += " ";
                ab += welcome;
            } else {
                ab += "~";
            }
        } else {
            const std::string& row = buf.rows[filerow];
            const size_t start = std::min(static_cast<size_t>(buf.col_offset), row.size());
            const size_t len = std::min(row.size() - start, static_cast<size_t>(screen_cols));
            ab.append(row.data() + start, len);
        }
        ab += "\x1b[K";  // 清除行尾
        ab += "\r\n";
    }
}

void draw_status_bar(std::string& ab) {
    ab += "\x1b[7m";
    if (input_mode == InputMode::Command) {
        const std::string prompt = ":" + command_line;
        const size_t start = prompt.size() > static_cast<size_t>(screen_cols)
            ? prompt.size() - static_cast<size_t>(screen_cols) : 0;
        ab.append(prompt, start, static_cast<size_t>(screen_cols));
        ab += "\x1b[K\x1b[m";
        return;
    }
    if (input_mode == InputMode::ConfirmQuit) {
        ab += "Save changes before quitting? (y/n, Esc to cancel)";
        ab += "\x1b[K\x1b[m";
        return;
    }
    if (input_mode == InputMode::ConfirmOpen) {
        const std::string prompt = "Save changes before opening " +
            pending_filename + "? (y/n, Esc to cancel)";
        ab.append(prompt, 0, static_cast<size_t>(screen_cols));
        ab += "\x1b[K\x1b[m";
        return;
    }
    if (!status_message.empty()) {
        ab.append(status_message, 0, static_cast<size_t>(screen_cols));
        ab += "\x1b[K\x1b[m";
        return;
    }  // 反显
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        buf.filename.empty() ? "[No Name]" : buf.filename.c_str(),
        buf.num_rows, buf.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
        buf.cy + 1, buf.num_rows);
    if (len > screen_cols) len = screen_cols;
    ab.append(status, len);
    while (len < screen_cols) {
        if (screen_cols - len == rlen) {
            ab.append(rstatus, rlen);
            break;
        }
        ab += " ";
        len++;
    }
    ab += "\x1b[m";
}

void refresh_screen() {
    scroll();
    std::string ab;
    ab += "\x1b[?25l";  // 隐藏光标
    ab += "\x1b[H";     // 光标归位

    draw_rows(ab);
    draw_status_bar(ab);

    char buf_pos[32];
    if (input_mode == InputMode::Command) {
        const size_t prompt_col = command_line.size() + 2;
        const size_t visible_col = std::min(prompt_col,
            static_cast<size_t>(screen_cols));
        snprintf(buf_pos, sizeof(buf_pos), "\x1b[%d;%dH",
            screen_rows + 1, static_cast<int>(visible_col));
    } else {
        snprintf(buf_pos, sizeof(buf_pos), "\x1b[%d;%dH",
            (buf.cy - buf.row_offset) + 1,
            (buf.cx - buf.col_offset) + 1);
    }
    ab += buf_pos;
    ab += "\x1b[?25h";  // 显示光标

    write_all(ab.data(), ab.size());
}

// ─── 主循环 ──────────────────────────────────────────────────
bool execute_command(const std::string& line, TermiosGuard& terminal) {
    const size_t separator = line.find_first_of(" \t");
    const std::string name = line.substr(0, separator);
    const size_t argument_start = separator == std::string::npos
        ? std::string::npos : line.find_first_not_of(" \t", separator);
    const std::string argument = argument_start == std::string::npos
        ? "" : line.substr(argument_start);

    if (name == "w" || name == "write") {
        save_file(argument);
    } else if (name == "q" || name == "quit") {
        if (buf.dirty) {
            input_mode = InputMode::ConfirmQuit;
        } else {
            write_all("\x1b[2J\x1b[H", 7);
            return false;
        }
    } else if (name == "q!" || name == "quit!") {
        write_all("\x1b[2J\x1b[H", 7);
        return false;
    } else if (name == "wq" || name == "x") {
        if (save_file(argument)) {
            write_all("\x1b[2J\x1b[H", 7);
            return false;
        }
    } else if (name == "open" || name == "edit" || name == "e") {
        if (argument.empty()) {
            status_message = "Usage: :open <file>";
        } else if (buf.dirty) {
            pending_filename = argument;
            input_mode = InputMode::ConfirmOpen;
        } else if (!open_file(argument)) {
            status_message = "Cannot open file: " + argument;
        }
    } else if (name == "find" || name == "search") {
        if (argument.empty()) {
            status_message = "Usage: :find <text>";
        } else {
            bool found = false;
            for (int offset = 0; offset < buf.num_rows; offset++) {
                const int row = (buf.cy + offset) % buf.num_rows;
                const size_t start = row == buf.cy
                    ? static_cast<size_t>(buf.cx) : 0;
                const size_t column = buf.rows[row].find(argument, start);
                if (column != std::string::npos) {
                    buf.cy = row;
                    buf.cx = static_cast<int>(column);
                    status_message = "Found: " + argument;
                    found = true;
                    break;
                }
            }
            if (!found) status_message = "Not found: " + argument;
        }
    } else if (name == "shell") {
        terminal.restore();
        write_all("\x1b[2J\x1b[H", 7);
        const char* shell = std::getenv("SHELL");
        if (shell == nullptr || shell[0] == '\0') shell = "/bin/sh";
        const pid_t child = fork();
        if (child == -1) {
            status_message = "Cannot start shell";
        } else if (child == 0) {
            execlp(shell, shell, "-i", static_cast<char*>(nullptr));
            perror("shell");
            _exit(127);
        } else {
            int child_status;
            pid_t result;
            do {
                result = waitpid(child, &child_status, 0);
            } while (result == -1 && errno == EINTR);
            if (result == -1) status_message = "Failed waiting for shell";
            else if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
                status_message = "Shell exited with an error";
            else
                status_message = "Shell exited";
        }
        terminal.enable_raw();
        get_window_size();
    } else if (name == "help") {
        status_message = ":w [file] :q :q! :wq :open <file> :find <text> :shell";
    } else if (!name.empty()) {
        status_message = "Unknown command: " + name + " (try :help)";
    }
    return true;
}

bool process_keypress(TermiosGuard& terminal) {
    const int c = read_key();
    if (input_mode == InputMode::Command) {
        if (c == '\x1b') {
            input_mode = InputMode::Editing;
        } else if (c == '\r' || c == '\n') {
            const std::string line = command_line;
            command_line.clear();
            input_mode = InputMode::Editing;
            return execute_command(line, terminal);
        } else if (c == BACKSPACE || c == '\b') {
            if (!command_line.empty()) command_line.pop_back();
        } else if (isprint(c)) {
            command_line.push_back(static_cast<char>(c));
        }
        return true;
    }

    if (input_mode == InputMode::ConfirmQuit ||
        input_mode == InputMode::ConfirmOpen) {
        const bool quitting = input_mode == InputMode::ConfirmQuit;
        if (c == '\x1b') {
            input_mode = InputMode::Editing;
            pending_filename.clear();
        } else if (c == 'y' || c == 'Y') {
            input_mode = InputMode::Editing;
            if (save_file()) {
                if (quitting) {
                    write_all("\x1b[2J\x1b[H", 7);
                    return false;
                }
                const std::string filename = pending_filename;
                pending_filename.clear();
                if (!open_file(filename))
                    status_message = "Cannot open file: " + filename;
            } else {
                pending_filename.clear();
            }
        } else if (c == 'n' || c == 'N') {
            input_mode = InputMode::Editing;
            if (quitting) {
                write_all("\x1b[2J\x1b[H", 7);
                return false;
            }
            const std::string filename = pending_filename;
            pending_filename.clear();
            if (!open_file(filename))
                status_message = "Cannot open file: " + filename;
        }
        return true;
    }

    switch (c) {
        case '\x1b':
            command_line.clear();
            status_message.clear();
            input_mode = InputMode::Command;
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            move_cursor(c);
            break;
        case HOME_KEY:
            buf.cx = 0;
            break;
        case END_KEY:
            if (buf.cy < buf.num_rows)
                buf.cx = buf.rows[buf.cy].size();
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) buf.cy = buf.row_offset;
                else if (c == PAGE_DOWN) buf.cy = buf.row_offset + screen_rows - 1;
                if (buf.cy > buf.num_rows - 1) buf.cy = buf.num_rows - 1;
                int len = buf.rows[buf.cy].size();
                if (buf.cx > len) buf.cx = len;
            }
            break;
        case BACKSPACE:
        case '\b':
            del_char();
            break;
        case DEL_KEY:
            delete_char();
            break;
        case '\r':
            insert_newline();
            break;
        default:
            if (isprint(c)) insert_char(c);
            break;
    }
    return true;
}

int main(int argc, char* argv[]) {
    TermiosGuard guard;
    get_window_size();
    if (argc >= 2 && !open_file(argv[1])) {
        fprintf(stderr, "Cannot open file: %s\n", argv[1]);
        return 1;
    }
    if (argc < 2) {
        buf.rows.push_back("");
        buf.num_rows = 1;
    }

    while (true) {
        refresh_screen();
        if (!process_keypress(guard)) break;
    }

    return 0;
}

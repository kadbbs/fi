#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <deque>
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
#include <signal.h>

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
    F6_KEY,
};

enum class InputMode {
    Editing,
    Command,
    ConfirmQuit,
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

struct ShellColor {
    int mode = 0;
    int first = -1;
    int second = -1;
    int third = -1;
};

struct ShellStyle {
    ShellColor foreground;
    ShellColor background;
    unsigned int attributes = 0;
};

struct ShellPane {
    int master_fd = -1;
    pid_t pid = -1;
    bool focused = false;
    int cursor_x = 0;
    int cursor_y = 0;
    int saved_cursor_x = 0;
    int saved_cursor_y = 0;
    std::vector<std::string> rows;
    std::vector<std::vector<ShellStyle>> styles;
    std::deque<std::string> history;
    std::deque<std::vector<ShellStyle>> history_styles;
    ShellStyle current_style;
    int history_offset = 0;
    int scroll_top = 0;
    int scroll_bottom = 0;
    bool origin_mode = false;
    std::string escape_sequence;
    int escape_state = 0;
};

static Buffer buf;
static std::vector<Buffer> buffers;
static size_t active_buffer = 0;
static ShellPane shell;
static int screen_rows, screen_cols;
static InputMode input_mode = InputMode::Editing;
static std::string command_line;
static std::string status_message;
static bool show_line_numbers = true;

// ─── 辅助函数 ────────────────────────────────────────────────
int shell_height() {
    return shell.master_fd < 0 ? 0 : std::min(std::max(3, screen_rows / 3),
        std::max(0, screen_rows - 2));
}

int editor_height() {
    return std::max(1, screen_rows - (shell.master_fd < 0 ? 0 : shell_height() + 1) - 1);
}

int sidebar_width() {
    return screen_cols >= 50 ? std::min(24, screen_cols / 3) : 0;
}

void clear_shell_row(int row) {
    if (row < 0 || row >= static_cast<int>(shell.rows.size())) return;
    shell.rows[row].assign(static_cast<size_t>(screen_cols), ' ');
    shell.styles[row].assign(static_cast<size_t>(screen_cols), ShellStyle{});
}

void save_shell_history_row(int row) {
    if (row < 0 || row >= static_cast<int>(shell.rows.size())) return;
    constexpr size_t max_history_rows = 2000;
    shell.history.push_back(shell.rows[row]);
    shell.history_styles.push_back(shell.styles[row]);
    if (shell.history.size() > max_history_rows) {
        shell.history.pop_front();
        shell.history_styles.pop_front();
    }
}

void scroll_shell_region_up(int top, int bottom, int count) {
    if (top < 0 || bottom >= static_cast<int>(shell.rows.size()) || top > bottom)
        return;
    count = std::min(count, bottom - top + 1);
    for (int i = 0; i < count; i++) {
        if (top == 0 && bottom == static_cast<int>(shell.rows.size()) - 1)
            save_shell_history_row(top);
        for (int row = top; row < bottom; row++) {
            shell.rows[row] = std::move(shell.rows[row + 1]);
            shell.styles[row] = std::move(shell.styles[row + 1]);
        }
        clear_shell_row(bottom);
    }
}

void scroll_shell_region_down(int top, int bottom, int count) {
    if (top < 0 || bottom >= static_cast<int>(shell.rows.size()) || top > bottom)
        return;
    count = std::min(count, bottom - top + 1);
    for (int i = 0; i < count; i++) {
        for (int row = bottom; row > top; row--) {
            shell.rows[row] = std::move(shell.rows[row - 1]);
            shell.styles[row] = std::move(shell.styles[row - 1]);
        }
        clear_shell_row(top);
    }
}

void shell_line_feed() {
    if (shell.cursor_y < shell.scroll_bottom) {
        shell.cursor_y++;
        return;
    }
    scroll_shell_region_up(shell.scroll_top, shell.scroll_bottom, 1);
}

int csi_parameter(const std::vector<int>& parameters, size_t index, int fallback = 1) {
    if (index >= parameters.size() || parameters[index] == 0) return fallback;
    return parameters[index];
}

int clamp_value(int value, int lower, int upper) {
    return std::max(lower, std::min(value, upper));
}

void set_shell_color(ShellColor& color, int mode, int first,
                     int second = -1, int third = -1) {
    color.mode = mode;
    color.first = first;
    color.second = second;
    color.third = third;
}

void apply_shell_sgr(const std::vector<int>& parameters) {
    const std::vector<int> defaults = parameters.empty()
        ? std::vector<int>{0} : parameters;
    for (size_t i = 0; i < defaults.size(); i++) {
        const int code = defaults[i];
        if (code == 0) shell.current_style = ShellStyle{};
        else if (code == 1) shell.current_style.attributes |= 1U;
        else if (code == 2) shell.current_style.attributes |= 2U;
        else if (code == 3) shell.current_style.attributes |= 4U;
        else if (code == 4) shell.current_style.attributes |= 8U;
        else if (code == 7) shell.current_style.attributes |= 16U;
        else if (code == 22) shell.current_style.attributes &= ~3U;
        else if (code == 23) shell.current_style.attributes &= ~4U;
        else if (code == 24) shell.current_style.attributes &= ~8U;
        else if (code == 27) shell.current_style.attributes &= ~16U;
        else if (code >= 30 && code <= 37)
            set_shell_color(shell.current_style.foreground, 1, code - 30);
        else if (code == 39)
            shell.current_style.foreground = ShellColor{};
        else if (code >= 40 && code <= 47)
            set_shell_color(shell.current_style.background, 1, code - 40);
        else if (code == 49)
            shell.current_style.background = ShellColor{};
        else if (code >= 90 && code <= 97)
            set_shell_color(shell.current_style.foreground, 1, code - 90 + 8);
        else if (code >= 100 && code <= 107)
            set_shell_color(shell.current_style.background, 1, code - 100 + 8);
        else if ((code == 38 || code == 48) && i + 1 < defaults.size()) {
            ShellColor& color = code == 38 ? shell.current_style.foreground
                                           : shell.current_style.background;
            if (defaults[i + 1] == 5 && i + 2 < defaults.size()) {
                set_shell_color(color, 2, clamp_value(defaults[i + 2], 0, 255));
                i += 2;
            } else if (defaults[i + 1] == 2 && i + 4 < defaults.size()) {
                set_shell_color(color, 3, clamp_value(defaults[i + 2], 0, 255),
                    clamp_value(defaults[i + 3], 0, 255),
                    clamp_value(defaults[i + 4], 0, 255));
                i += 4;
            }
        }
    }
}

void append_shell_color(std::string& output, const ShellColor& color,
                        bool foreground) {
    if (color.mode == 0) {
        output += foreground ? ";39" : ";49";
    } else if (color.mode == 1) {
        const int base = foreground ? 30 : 40;
        output += ";" + std::to_string(color.first < 8
            ? base + color.first
            : (foreground ? 90 : 100) + color.first - 8);
    } else if (color.mode == 2) {
        output += foreground ? ";38;5;" : ";48;5;";
        output += std::to_string(color.first);
    } else if (color.mode == 3) {
        output += foreground ? ";38;2;" : ";48;2;";
        output += std::to_string(color.first) + ";" +
            std::to_string(color.second) + ";" + std::to_string(color.third);
    }
}

bool same_shell_style(const ShellStyle& left, const ShellStyle& right) {
    return left.attributes == right.attributes &&
        left.foreground.mode == right.foreground.mode &&
        left.foreground.first == right.foreground.first &&
        left.foreground.second == right.foreground.second &&
        left.foreground.third == right.foreground.third &&
        left.background.mode == right.background.mode &&
        left.background.first == right.background.first &&
        left.background.second == right.background.second &&
        left.background.third == right.background.third;
}

void append_shell_style(std::string& output, const ShellStyle& style) {
    output += "\x1b[0";
    if (style.attributes & 1U) output += ";1";
    if (style.attributes & 2U) output += ";2";
    if (style.attributes & 4U) output += ";3";
    if (style.attributes & 8U) output += ";4";
    if (style.attributes & 16U) output += ";7";
    append_shell_color(output, style.foreground, true);
    append_shell_color(output, style.background, false);
    output += "m";
}

void execute_shell_csi(char final) {
    std::vector<int> parameters;
    std::stringstream stream(shell.escape_sequence);
    std::string field;
    while (std::getline(stream, field, ';'))
        parameters.push_back(field.empty() ? 0 : std::atoi(field.c_str()));
    const int first = parameters.empty() ? 0 : parameters[0];
    if (!shell.escape_sequence.empty() && shell.escape_sequence[0] == '?') {
        if (final == 'h' || final == 'l') {
            std::stringstream private_stream(shell.escape_sequence.substr(1));
            std::string private_field;
            while (std::getline(private_stream, private_field, ';')) {
                if (std::atoi(private_field.c_str()) == 6) {
                    shell.origin_mode = final == 'h';
                    shell.cursor_x = 0;
                    shell.cursor_y = shell.origin_mode ? shell.scroll_top : 0;
                }
            }
        }
        return;
    }
    if (!shell.escape_sequence.empty() && shell.escape_sequence[0] == '>')
        return;

    switch (final) {
        case 'A': shell.cursor_y = std::max(shell.origin_mode
                ? shell.scroll_top : 0,
            shell.cursor_y - csi_parameter(parameters, 0)); break;
        case 'B': shell.cursor_y = std::min(shell.origin_mode
                ? shell.scroll_bottom : static_cast<int>(shell.rows.size()) - 1,
            shell.cursor_y + csi_parameter(parameters, 0)); break;
        case 'C': shell.cursor_x = std::min(screen_cols - 1,
            shell.cursor_x + csi_parameter(parameters, 0)); break;
        case 'D': shell.cursor_x = std::max(0,
            shell.cursor_x - csi_parameter(parameters, 0)); break;
        case 'G': shell.cursor_x = std::min(screen_cols - 1,
            csi_parameter(parameters, 0) - 1); break;
        case 'd': shell.cursor_y = clamp_value(
            (shell.origin_mode ? shell.scroll_top : 0) +
                csi_parameter(parameters, 0) - 1,
            shell.origin_mode ? shell.scroll_top : 0,
            shell.origin_mode ? shell.scroll_bottom
                : static_cast<int>(shell.rows.size()) - 1); break;
        case 'H':
        case 'f':
            shell.cursor_y = clamp_value(
                (shell.origin_mode ? shell.scroll_top : 0) +
                    csi_parameter(parameters, 0) - 1,
                shell.origin_mode ? shell.scroll_top : 0,
                shell.origin_mode ? shell.scroll_bottom
                    : static_cast<int>(shell.rows.size()) - 1);
            shell.cursor_x = std::min(screen_cols - 1,
                csi_parameter(parameters, 1) - 1);
            break;
        case 'J':
            if (first == 2 || first == 3) {
                for (int row = 0; row < static_cast<int>(shell.rows.size()); row++)
                    clear_shell_row(row);
            } else if (first == 0) {
                for (int col = shell.cursor_x; col < screen_cols; col++) {
                    shell.rows[shell.cursor_y][col] = ' ';
                    shell.styles[shell.cursor_y][col] = ShellStyle{};
                }
                for (int row = shell.cursor_y + 1;
                     row < static_cast<int>(shell.rows.size()); row++)
                    clear_shell_row(row);
            } else if (first == 1) {
                for (int col = 0; col <= shell.cursor_x; col++) {
                    shell.rows[shell.cursor_y][col] = ' ';
                    shell.styles[shell.cursor_y][col] = ShellStyle{};
                }
                for (int row = 0; row < shell.cursor_y; row++)
                    clear_shell_row(row);
            }
            break;
        case 'K':
            if (first == 0) {
                for (int col = shell.cursor_x; col < screen_cols; col++) {
                    shell.rows[shell.cursor_y][col] = ' ';
                    shell.styles[shell.cursor_y][col] = ShellStyle{};
                }
            } else if (first == 1) {
                for (int col = 0; col <= shell.cursor_x; col++) {
                    shell.rows[shell.cursor_y][col] = ' ';
                    shell.styles[shell.cursor_y][col] = ShellStyle{};
                }
            }
            else if (first == 2)
                clear_shell_row(shell.cursor_y);
            break;
        case 'm':
            apply_shell_sgr(parameters);
            break;
        case '@':
        case 'P':
        case 'X': {
            const int count = std::min(csi_parameter(parameters, 0),
                screen_cols - shell.cursor_x);
            auto& row = shell.rows[shell.cursor_y];
            auto& styles = shell.styles[shell.cursor_y];
            if (final == '@') {
                row.insert(static_cast<size_t>(shell.cursor_x),
                    static_cast<size_t>(count), ' ');
                styles.insert(styles.begin() + shell.cursor_x,
                    static_cast<size_t>(count), ShellStyle{});
                row.resize(static_cast<size_t>(screen_cols));
                styles.resize(static_cast<size_t>(screen_cols));
            } else if (final == 'P') {
                row.erase(static_cast<size_t>(shell.cursor_x),
                    static_cast<size_t>(count));
                styles.erase(styles.begin() + shell.cursor_x,
                    styles.begin() + shell.cursor_x + count);
                row.append(static_cast<size_t>(count), ' ');
                styles.insert(styles.end(), static_cast<size_t>(count), ShellStyle{});
            } else {
                for (int col = shell.cursor_x; col < shell.cursor_x + count; col++) {
                    row[col] = ' ';
                    styles[col] = ShellStyle{};
                }
            }
            break;
        }
        case 'L':
            if (shell.cursor_y >= shell.scroll_top &&
                shell.cursor_y <= shell.scroll_bottom)
                scroll_shell_region_down(shell.cursor_y, shell.scroll_bottom,
                    csi_parameter(parameters, 0));
            break;
        case 'M':
            if (shell.cursor_y >= shell.scroll_top &&
                shell.cursor_y <= shell.scroll_bottom)
                scroll_shell_region_up(shell.cursor_y, shell.scroll_bottom,
                    csi_parameter(parameters, 0));
            break;
        case 'S':
            scroll_shell_region_up(shell.scroll_top, shell.scroll_bottom,
                csi_parameter(parameters, 0));
            break;
        case 'T':
            scroll_shell_region_down(shell.scroll_top, shell.scroll_bottom,
                csi_parameter(parameters, 0));
            break;
        case 'r': {
            const int top = csi_parameter(parameters, 0) - 1;
            const int bottom = csi_parameter(parameters, 1,
                static_cast<int>(shell.rows.size())) - 1;
            if (top >= 0 && bottom < static_cast<int>(shell.rows.size()) &&
                top < bottom) {
                shell.scroll_top = top;
                shell.scroll_bottom = bottom;
                shell.cursor_x = 0;
                shell.cursor_y = top;
            }
            break;
        }
        case 's':
            shell.saved_cursor_x = shell.cursor_x;
            shell.saved_cursor_y = shell.cursor_y;
            break;
        case 'u':
            shell.cursor_x = shell.saved_cursor_x;
            shell.cursor_y = shell.saved_cursor_y;
            break;
        default:
            break;
    }
}

void consume_shell_output(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (shell.escape_state == 1) {
            if (c == '[') {
                shell.escape_sequence.clear();
                shell.escape_state = 2;
            } else if (c == ']') {
                shell.escape_state = 3;
            } else {
                if (c == '7') {
                    shell.saved_cursor_x = shell.cursor_x;
                    shell.saved_cursor_y = shell.cursor_y;
                } else if (c == '8') {
                    shell.cursor_x = shell.saved_cursor_x;
                    shell.cursor_y = shell.saved_cursor_y;
                } else if (c == 'D') {
                    shell_line_feed();
                } else if (c == 'M') {
                    if (shell.cursor_y == shell.scroll_top)
                        scroll_shell_region_down(shell.scroll_top,
                            shell.scroll_bottom, 1);
                    else
                        shell.cursor_y = std::max(0, shell.cursor_y - 1);
                } else if (c == 'c') {
                    for (int row = 0; row < static_cast<int>(shell.rows.size()); row++)
                        clear_shell_row(row);
                    shell.cursor_x = shell.cursor_y = 0;
                    shell.scroll_top = 0;
                    shell.scroll_bottom = static_cast<int>(shell.rows.size()) - 1;
                    shell.origin_mode = false;
                    shell.current_style = ShellStyle{};
                }
                shell.escape_state = 0;
            }
            continue;
        }
        if (shell.escape_state == 2) {
            if (c >= 0x40 && c <= 0x7e) {
                execute_shell_csi(static_cast<char>(c));
                shell.escape_state = 0;
            } else if (shell.escape_sequence.size() < 64) {
                shell.escape_sequence.push_back(static_cast<char>(c));
            }
            continue;
        }
        if (shell.escape_state == 3) {
            if (c == '\a') shell.escape_state = 0;
            else if (c == '\x1b') shell.escape_state = 1;
            continue;
        }
        if (c == '\x1b') shell.escape_state = 1;
        else if (c == '\x0c') {
            for (int row = 0; row < static_cast<int>(shell.rows.size()); row++)
                clear_shell_row(row);
            shell.cursor_x = shell.cursor_y = 0;
        }
        else if (c == '\r') shell.cursor_x = 0;
        else if (c == '\n') shell_line_feed();
        else if (c == '\b') shell.cursor_x = std::max(0, shell.cursor_x - 1);
        else if (c == '\t') shell.cursor_x = std::min(screen_cols - 1,
            (shell.cursor_x + 8) & ~7);
        else if (c >= 0x20) {
            if (shell.cursor_x >= screen_cols) {
                shell.cursor_x = 0;
                shell_line_feed();
            }
            shell.rows[shell.cursor_y][shell.cursor_x] = static_cast<char>(c);
            shell.styles[shell.cursor_y][shell.cursor_x] = shell.current_style;
            shell.cursor_x++;
        }
    }
}

void send_key_to_shell(int key) {
    std::string sequence;
    switch (key) {
        case ARROW_UP: sequence = "\x1b[A"; break;
        case ARROW_DOWN: sequence = "\x1b[B"; break;
        case ARROW_RIGHT: sequence = "\x1b[C"; break;
        case ARROW_LEFT: sequence = "\x1b[D"; break;
        case HOME_KEY: sequence = "\x1b[H"; break;
        case END_KEY: sequence = "\x1b[F"; break;
        case PAGE_UP: sequence = "\x1b[5~"; break;
        case PAGE_DOWN: sequence = "\x1b[6~"; break;
        case DEL_KEY: sequence = "\x1b[3~"; break;
        case BACKSPACE: sequence = "\x7f"; break;
        default:
            if (key >= 0 && key <= 255)
                sequence.push_back(static_cast<char>(key));
            break;
    }
    if (!sequence.empty()) {
        ssize_t written;
        do {
            written = write(shell.master_fd, sequence.data(), sequence.size());
        } while (written == -1 && errno == EINTR);
        if (written == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
            status_message = "Shell input failed";
    }
}

int line_number_width() {
    const int available_width = screen_cols - sidebar_width();
    if (!show_line_numbers || available_width < 2) return 0;
    int digits = 1;
    for (int rows = std::max(buf.num_rows, 1); rows >= 10; rows /= 10)
        digits++;
    return std::min(digits + 2, available_width - 1);
}

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
std::string normalized_file_path(const std::string& path) {
    std::string absolute = path;
    if (path.empty() || path[0] != '/') {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)) == nullptr) return path;
        absolute = std::string(cwd) + "/" + path;
    }
    std::vector<std::string> components;
    size_t start = 0;
    while (start < absolute.size()) {
        const size_t end = absolute.find('/', start);
        const std::string component = absolute.substr(start,
            end == std::string::npos ? std::string::npos : end - start);
        if (!component.empty() && component != ".") {
            if (component == "..") {
                if (!components.empty()) components.pop_back();
            } else {
                components.push_back(component);
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::string normalized = "/";
    for (size_t i = 0; i < components.size(); i++) {
        if (i > 0) normalized += '/';
        normalized += components[i];
    }
    return normalized;
}

void sync_current_buffer() {
    if (buffers.empty()) {
        buffers.push_back(buf);
        active_buffer = 0;
    } else {
        buffers[active_buffer] = buf;
    }
}

bool switch_buffer(size_t index) {
    if (index >= buffers.size()) {
        status_message = "No such open buffer";
        return false;
    }
    sync_current_buffer();
    active_buffer = index;
    buf = buffers[active_buffer];
    status_message = "Switched to " +
        (buf.filename.empty() ? std::string("[No Name]") : buf.filename);
    return true;
}

bool open_file(const std::string& path) {
    for (size_t i = 0; i < buffers.size(); i++) {
        if (!buffers[i].filename.empty() &&
            normalized_file_path(buffers[i].filename) == normalized_file_path(path))
            return switch_buffer(i);
    }
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
    if (buffers.empty()) {
        buffers.push_back(opened);
        active_buffer = 0;
    } else {
        sync_current_buffer();
        buffers.push_back(opened);
        active_buffer = buffers.size() - 1;
    }
    buf = std::move(opened);
    status_message = "Opened " + path;
    return true;
}

bool save_buffer(Buffer& buffer, const std::string& path = "") {
    const std::string destination = path.empty() ? buffer.filename : path;
    if (destination.empty()) {
        status_message = "No file name; use :w <file>";
        return false;
    }
    std::ofstream ofs(destination);
    if (!ofs.is_open()) {
        status_message = "Cannot write file: " + destination;
        return false;
    }
    for (int i = 0; i < buffer.num_rows; i++) {
        ofs << buffer.rows[i];
        if (i != buffer.num_rows - 1) ofs << '\n';
    }
    ofs << '\n';
    if (!ofs) {
        status_message = "Write failed: " + destination;
        return false;
    }
    buffer.filename = destination;
    buffer.dirty = false;
    status_message = "Saved " + destination;
    return true;
}

bool save_file(const std::string& path = "") {
    const bool saved = save_buffer(buf, path);
    if (saved) sync_current_buffer();
    return saved;
}

bool save_all_buffers() {
    sync_current_buffer();
    for (Buffer& buffer : buffers) {
        if (buffer.dirty && !save_buffer(buffer)) return false;
    }
    buf = buffers[active_buffer];
    return true;
}

bool has_modified_buffers() {
    sync_current_buffer();
    return std::any_of(buffers.begin(), buffers.end(),
        [](const Buffer& buffer) { return buffer.dirty; });
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
        unsigned char seq[4];
        if (!read_byte(&seq[0], 30)) return '\x1b';
        if (!read_byte(&seq[1], 30)) return '\x1b';
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                unsigned int length = 1;
                seq[0] = seq[1];
                while (length < 3) {
                    if (!read_byte(&seq[length + 1], 30)) return '\x1b';
                    if (seq[length + 1] == '~') {
                        int number = seq[0] - '0';
                        if (length > 1) number = number * 10 + seq[1] - '0';
                        switch (number) {
                            case 1:
                            case 7: return HOME_KEY;
                            case 3: return DEL_KEY;
                            case 4:
                            case 8: return END_KEY;
                            case 5: return PAGE_UP;
                            case 6: return PAGE_DOWN;
                            case 17: return F6_KEY;
                        }
                        return '\x1b';
                    }
                    if (seq[length + 1] < '0' || seq[length + 1] > '9')
                        return '\x1b';
                    seq[length] = seq[length + 1];
                    length++;
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
    const int text_width = std::max(1,
        screen_cols - sidebar_width() - line_number_width());
    const int visible_rows = std::max(1, editor_height());
    if (buf.cy < buf.row_offset) buf.row_offset = buf.cy;
    if (buf.cy >= buf.row_offset + visible_rows)
        buf.row_offset = buf.cy - visible_rows + 1;
    if (buf.cx < buf.col_offset) buf.col_offset = buf.cx;
    if (buf.cx >= buf.col_offset + text_width)
        buf.col_offset = buf.cx - text_width + 1;
}

void draw_rows(std::string& ab) {
    const int side_width = sidebar_width();
    const int side_text_width = std::max(0, side_width - 1);
    const int gutter_width = line_number_width();
    const int text_width = std::max(1, screen_cols - side_width - gutter_width);
    const int visible_rows = editor_height();
    const int sidebar_offset = std::max(0,
        static_cast<int>(active_buffer) - visible_rows + 1);
    for (int y = 0; y < visible_rows; y++) {
        if (side_width > 0) {
            const int item = sidebar_offset + y;
            if (item < static_cast<int>(buffers.size())) {
                const Buffer& entry = buffers[static_cast<size_t>(item)];
                const size_t slash = entry.filename.find_last_of('/');
                std::string label = entry.filename.empty() ? "[No Name]"
                    : entry.filename.substr(slash == std::string::npos
                        ? 0 : slash + 1);
                if (entry.dirty) label += '*';
                const bool selected = static_cast<size_t>(item) == active_buffer;
                std::string cell = selected ? "> " : "  ";
                cell += label;
                if (cell.size() > static_cast<size_t>(side_text_width))
                    cell.resize(static_cast<size_t>(side_text_width));
                if (selected) ab += "\x1b[7m";
                ab += cell;
                ab.append(static_cast<size_t>(side_text_width) - cell.size(), ' ');
                if (selected) ab += "\x1b[m";
            } else {
                ab.append(static_cast<size_t>(side_text_width), ' ');
            }
            ab += "\x1b[36m|\x1b[m";
        }
        int filerow = y + buf.row_offset;
        if (filerow >= buf.num_rows) {
            if (buf.num_rows == 0 && y == visible_rows / 3) {
                char welcome[80];
                int len = snprintf(welcome, sizeof(welcome),
                    "Tiny Editor -- no args");
                if (len > text_width) len = text_width;
                int padding = (text_width - len) / 2;
                ab.append(static_cast<size_t>(gutter_width), ' ');
                if (padding) { ab += "~"; padding--; }
                while (padding--) ab += " ";
                ab += welcome;
            } else {
                if (gutter_width > 0) {
                    ab += "~";
                    ab.append(static_cast<size_t>(gutter_width - 1), ' ');
                } else {
                    ab += "~";
                }
            }
        } else {
            if (gutter_width > 0) {
                char line_number[32];
                const int number_width = gutter_width - 1;
                const int length = snprintf(line_number, sizeof(line_number),
                    "%*d ", number_width, filerow + 1);
                ab.append(line_number, static_cast<size_t>(
                    std::min(length, gutter_width)));
            }
            const std::string& row = buf.rows[filerow];
            const size_t start = std::min(static_cast<size_t>(buf.col_offset), row.size());
            const size_t len = std::min(row.size() - start,
                static_cast<size_t>(text_width));
            ab.append(row.data() + start, len);
        }
        ab += "\x1b[K";  // 清除行尾
        if (y + 1 < visible_rows) ab += "\r\n";
    }

    if (shell.master_fd >= 0) {
        char shell_header_pos[32];
        snprintf(shell_header_pos, sizeof(shell_header_pos), "\x1b[%d;1H",
            editor_height() + 1);
        ab += shell_header_pos;
        ab += shell.focused ? "\x1b[1;36m" : "\x1b[36m";
        const std::string header = shell.focused
            ? "--- SHELL | F6: editor | PgUp/PgDn: scrollback"
            : "--- SHELL | F6: focus";
        ab.append(header, 0, static_cast<size_t>(screen_cols));
        ab += "\x1b[0m\x1b[K\r\n";
        const size_t history_size = shell.history.size();
        const size_t total_rows = history_size + shell.rows.size();
        const size_t height = shell.rows.size();
        const size_t offset = std::min(static_cast<size_t>(shell.history_offset),
            history_size);
        const size_t start = total_rows > height + offset
            ? total_rows - height - offset : 0;
        for (size_t visible = 0; visible < height; visible++) {
            const size_t source = start + visible;
            const std::string* row = nullptr;
            const std::vector<ShellStyle>* styles = nullptr;
            if (source < history_size) {
                row = &shell.history[source];
                styles = &shell.history_styles[source];
            } else if (source - history_size < shell.rows.size()) {
                row = &shell.rows[source - history_size];
                styles = &shell.styles[source - history_size];
            }
            ShellStyle active_style;
            bool style_applied = false;
            for (int col = 0; col < screen_cols; col++) {
                const ShellStyle& cell_style = styles == nullptr
                    ? active_style : (*styles)[static_cast<size_t>(col)];
                if (!style_applied || !same_shell_style(active_style, cell_style)) {
                    append_shell_style(ab, cell_style);
                    active_style = cell_style;
                    style_applied = true;
                }
                ab += row == nullptr ? ' ' : (*row)[static_cast<size_t>(col)];
            }
            ab += "\x1b[0m\x1b[K";
            if (visible + 1 < height) ab += "\r\n";
        }
    }

    char buffer_bar_pos[32];
    snprintf(buffer_bar_pos, sizeof(buffer_bar_pos), "\x1b[%d;1H", screen_rows);
    ab += buffer_bar_pos;
    ab += "\x1b[46;30m";
    std::string buffer_bar = " FILES ";
    for (size_t order = 0; order < buffers.size(); order++) {
        const size_t i = order == 0 ? active_buffer
            : (order <= active_buffer ? order - 1 : order);
        const Buffer& entry = buffers[i];
        const size_t slash = entry.filename.find_last_of('/');
        std::string label = entry.filename.empty() ? "[No Name]"
            : entry.filename.substr(slash == std::string::npos ? 0 : slash + 1);
        if (entry.dirty) label += '*';
        const std::string item = order == 0
            ? " [" + std::to_string(i + 1) + ":" + label + "]"
            : " " + std::to_string(i + 1) + ":" + label;
        buffer_bar += item;
    }
    ab.append(buffer_bar, 0, static_cast<size_t>(screen_cols));
    const size_t shown = std::min(buffer_bar.size(),
        static_cast<size_t>(screen_cols));
    if (shown < static_cast<size_t>(screen_cols))
        ab.append(static_cast<size_t>(screen_cols) - shown, ' ');
    ab += "\x1b[0m";
}

void draw_status_bar(std::string& ab) {
    ab += "\x1b[7m";
    if (shell.focused && input_mode == InputMode::Editing) {
        ab += "SHELL | F6: editor | PgUp/PgDn: scrollback | Up/Down: command history";
        if (shell.history_offset > 0) ab += " | older output";
        ab += "\x1b[K\x1b[m";
        return;
    }
    if (input_mode == InputMode::Command) {
        const std::string prompt = ":" + command_line;
        const size_t start = prompt.size() > static_cast<size_t>(screen_cols)
            ? prompt.size() - static_cast<size_t>(screen_cols) : 0;
        ab.append(prompt, start, static_cast<size_t>(screen_cols));
        ab += "\x1b[K\x1b[m";
        return;
    }
    if (input_mode == InputMode::ConfirmQuit) {
        ab += "Save all modified files before quitting? (y/n, Esc to cancel)";
        ab += "\x1b[K\x1b[m";
        return;
    }
    if (!status_message.empty()) {
        ab.append(status_message, 0, static_cast<size_t>(screen_cols));
        ab += "\x1b[K\x1b[m";
        return;
    }  // 反显
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s | %d lines %s",
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
    sync_current_buffer();
    scroll();
    std::string ab;
    ab += "\x1b[?25l";  // 隐藏光标
    ab += "\x1b[H";     // 光标归位

    draw_rows(ab);
    char status_pos[32];
    snprintf(status_pos, sizeof(status_pos), "\x1b[%d;1H", screen_rows + 1);
    ab += status_pos;
    draw_status_bar(ab);

    char buf_pos[32];
    if (input_mode == InputMode::Command) {
        const size_t prompt_col = command_line.size() + 2;
        const size_t visible_col = std::min(prompt_col,
            static_cast<size_t>(screen_cols));
        snprintf(buf_pos, sizeof(buf_pos), "\x1b[%d;%dH",
            screen_rows + 1, static_cast<int>(visible_col));
    } else if (shell.master_fd >= 0 && shell.focused) {
        snprintf(buf_pos, sizeof(buf_pos), "\x1b[%d;%dH",
            editor_height() + 2 + shell.cursor_y,
            std::min(shell.cursor_x + 1, screen_cols));
    } else {
        snprintf(buf_pos, sizeof(buf_pos), "\x1b[%d;%dH",
            (buf.cy - buf.row_offset) + 1,
            sidebar_width() + line_number_width() +
                (buf.cx - buf.col_offset) + 1);
    }
    ab += buf_pos;
    ab += "\x1b[?25h";  // 显示光标

    write_all(ab.data(), ab.size());
}

// ─── 主循环 ──────────────────────────────────────────────────
bool execute_command(const std::string& line) {
    const size_t separator = line.find_first_of(" \t");
    const std::string name = line.substr(0, separator);
    const size_t argument_start = separator == std::string::npos
        ? std::string::npos : line.find_first_not_of(" \t", separator);
    const std::string argument = argument_start == std::string::npos
        ? "" : line.substr(argument_start);

    if (name == "set" && argument == "number") {
        show_line_numbers = true;
        status_message = "Line numbers enabled";
    } else if (name == "noset" && argument == "number") {
        show_line_numbers = false;
        status_message = "Line numbers disabled";
    } else if (name == "w" || name == "write") {
        save_file(argument);
    } else if (name == "q" || name == "quit") {
        if (has_modified_buffers()) {
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
            if (has_modified_buffers()) input_mode = InputMode::ConfirmQuit;
            else {
                write_all("\x1b[2J\x1b[H", 7);
                return false;
            }
        }
    } else if (name == "open" || name == "edit" || name == "e") {
        if (argument.empty()) {
            status_message = "Usage: :open <file>";
        } else if (!open_file(argument)) {
            status_message = "Cannot open file: " + argument;
        }
    } else if (name == "buffer" || name == "b") {
        if (argument.empty()) {
            status_message = "Usage: :buffer <number> (see :buffers)";
        } else {
            char* end = nullptr;
            const long number = std::strtol(argument.c_str(), &end, 10);
            if (end == argument.c_str() || *end != '\0' || number < 1 ||
                static_cast<size_t>(number) > buffers.size()) {
                status_message = "No such open buffer: " + argument;
            } else {
                switch_buffer(static_cast<size_t>(number - 1));
            }
        }
    } else if (name == "bnext" || name == "bn") {
        if (buffers.size() > 1)
            switch_buffer((active_buffer + 1) % buffers.size());
        else
            status_message = "Only one buffer is open";
    } else if (name == "bprev" || name == "bp") {
        if (buffers.size() > 1)
            switch_buffer((active_buffer + buffers.size() - 1) % buffers.size());
        else
            status_message = "Only one buffer is open";
    } else if (name == "buffers") {
        std::string listing = "Open files:";
        for (size_t i = 0; i < buffers.size(); i++) {
            const size_t slash = buffers[i].filename.find_last_of('/');
            const std::string label = buffers[i].filename.empty() ? "[No Name]"
                : buffers[i].filename.substr(slash == std::string::npos
                    ? 0 : slash + 1);
            listing += " " + std::to_string(i + 1) +
                (i == active_buffer ? ">" : ":") + label +
                (buffers[i].dirty ? "*" : "");
        }
        status_message = listing;
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
    } else if (name == "shell" && argument == "close") {
        if (shell.master_fd >= 0) {
            close(shell.master_fd);
            shell.master_fd = -1;
            if (shell.pid > 0) {
                kill(-shell.pid, SIGHUP);
                int child_status;
                while (waitpid(shell.pid, &child_status, 0) == -1 && errno == EINTR) {}
                shell.pid = -1;
            }
            shell.rows.clear();
            shell.styles.clear();
            shell.history.clear();
            shell.history_styles.clear();
            shell.history_offset = 0;
            shell.focused = false;
            status_message = "Shell panel closed";
        } else {
            status_message = "Shell panel is not open";
        }
    } else if (name == "shell") {
        if (shell.master_fd >= 0) {
            shell.focused = true;
            status_message.clear();
        } else {
            int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (master == -1 || grantpt(master) == -1 || unlockpt(master) == -1) {
                if (master >= 0) close(master);
                status_message = "Cannot create shell PTY";
            } else {
                char* slave_name = ptsname(master);
                if (slave_name == nullptr) {
                    close(master);
                    status_message = "Cannot locate shell PTY";
                } else {
                    const pid_t child = fork();
                    if (child == -1) {
                        close(master);
                        status_message = "Cannot start shell";
                    } else if (child == 0) {
                        close(master);
                        if (setsid() == -1) _exit(126);
                        const int slave = open(slave_name, O_RDWR);
                        if (slave == -1 || ioctl(slave, TIOCSCTTY, 0) == -1)
                            _exit(126);
                        dup2(slave, STDIN_FILENO);
                        dup2(slave, STDOUT_FILENO);
                        dup2(slave, STDERR_FILENO);
                        if (slave > STDERR_FILENO) close(slave);
                        const char* shell_path = std::getenv("SHELL");
                        if (shell_path == nullptr || shell_path[0] == '\0')
                            shell_path = "/bin/sh";
                        execlp(shell_path, shell_path, "-i",
                            static_cast<char*>(nullptr));
                        _exit(127);
                    } else {
                        winsize window{};
                        window.ws_row = static_cast<unsigned short>(
                            std::max(1, std::min(shell_height(), screen_rows)));
                        window.ws_col = static_cast<unsigned short>(screen_cols);
                        ioctl(master, TIOCSWINSZ, &window);
                        shell.master_fd = master;
                        shell.pid = child;
                        shell.focused = true;
                        shell.cursor_x = 0;
                        shell.cursor_y = 0;
                        shell.rows.assign(static_cast<size_t>(shell_height()),
                            std::string(static_cast<size_t>(screen_cols), ' '));
                        shell.styles.assign(static_cast<size_t>(shell_height()),
                            std::vector<ShellStyle>(static_cast<size_t>(screen_cols)));
                        shell.history.clear();
                        shell.history_styles.clear();
                        shell.history_offset = 0;
                        shell.current_style = ShellStyle{};
                        shell.scroll_top = 0;
                        shell.scroll_bottom = shell_height() - 1;
                        shell.origin_mode = false;
                        shell.escape_sequence.clear();
                        shell.escape_state = 0;
                        status_message.clear();
                    }
                }
            }
        }
    } else if (name == "help") {
        status_message = ":open <file> :buffers :buffer <n> :bnext :bprev :set number :noset number :w :q :q! :wq :find <text> :shell";
    } else if (!name.empty()) {
        status_message = "Unknown command: " + name + " (try :help)";
    }
    return true;
}

bool process_keypress() {
    const int c = read_key();
    if (shell.master_fd >= 0 && shell.focused &&
        input_mode == InputMode::Editing) {
        if (c == F6_KEY) {
            shell.focused = false;
            status_message.clear();
        } else if (c == PAGE_UP) {
            shell.history_offset = std::min(
                shell.history_offset + std::max(1, shell_height() / 2),
                static_cast<int>(shell.history.size()));
        } else if (c == PAGE_DOWN) {
            shell.history_offset = std::max(0,
                shell.history_offset - std::max(1, shell_height() / 2));
        } else {
            send_key_to_shell(c);
        }
        return true;
    }
    if (shell.master_fd >= 0 && c == F6_KEY &&
        input_mode == InputMode::Editing) {
        shell.focused = true;
        status_message.clear();
        return true;
    }

    if (input_mode == InputMode::Command) {
        if (c == '\x1b') {
            input_mode = InputMode::Editing;
        } else if (c == '\r' || c == '\n') {
            const std::string line = command_line;
            command_line.clear();
            input_mode = InputMode::Editing;
            return execute_command(line);
        } else if (c == BACKSPACE || c == '\b') {
            if (!command_line.empty()) command_line.pop_back();
        } else if (isprint(c)) {
            command_line.push_back(static_cast<char>(c));
        }
        return true;
    }

    if (input_mode == InputMode::ConfirmQuit) {
        if (c == '\x1b') {
            input_mode = InputMode::Editing;
        } else if (c == 'y' || c == 'Y') {
            input_mode = InputMode::Editing;
            if (save_all_buffers()) {
                write_all("\x1b[2J\x1b[H", 7);
                return false;
            }
        } else if (c == 'n' || c == 'N') {
            input_mode = InputMode::Editing;
            write_all("\x1b[2J\x1b[H", 7);
            return false;
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
                else if (c == PAGE_DOWN) buf.cy = buf.row_offset + editor_height() - 1;
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
    if (argc >= 2) {
        for (int i = 1; i < argc; i++) {
            if (!open_file(argv[i])) {
                fprintf(stderr, "Cannot open file: %s\n", argv[i]);
                return 1;
            }
        }
        switch_buffer(0);
    }
    if (argc < 2) {
        buf.rows.push_back("");
        buf.num_rows = 1;
        buffers.push_back(buf);
    }

    while (true) {
        refresh_screen();
        pollfd descriptors[2] = {
            {STDIN_FILENO, POLLIN, 0},
            {shell.master_fd, POLLIN | POLLHUP, 0},
        };
        const nfds_t descriptor_count = shell.master_fd >= 0 ? 2 : 1;
        int ready;
        do {
            ready = poll(descriptors, descriptor_count, -1);
        } while (ready == -1 && errno == EINTR);
        if (ready == -1) die("poll");

        if (descriptor_count == 2 &&
            (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char output[4096];
            bool finished = false;
            while (true) {
                const ssize_t count = read(shell.master_fd, output, sizeof(output));
                if (count > 0) {
                    consume_shell_output(output, static_cast<size_t>(count));
                } else if (count == -1 && errno == EINTR) {
                    continue;
                } else if (count == -1 &&
                    (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                } else {
                    finished = true;
                    break;
                }
            }
            if (finished) {
                close(shell.master_fd);
                shell.master_fd = -1;
                shell.focused = false;
                shell.rows.clear();
                shell.styles.clear();
                shell.history.clear();
                shell.history_styles.clear();
                if (shell.pid > 0) {
                    int child_status;
                    while (waitpid(shell.pid, &child_status, 0) == -1 &&
                           errno == EINTR) {}
                    shell.pid = -1;
                }
                status_message = "Shell exited";
            }
        }
        if (descriptors[0].revents & POLLIN) {
            if (!process_keypress()) break;
        }
    }

    if (shell.master_fd >= 0) close(shell.master_fd);
    if (shell.pid > 0) {
        kill(-shell.pid, SIGHUP);
        int child_status;
        while (waitpid(shell.pid, &child_status, 0) == -1 && errno == EINTR) {}
    }
    return 0;
}

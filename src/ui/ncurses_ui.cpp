#include "ncurses_ui.h"
#include "log_wrapper.h"  // 用于 g_ui_should_exit
#include <csignal>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <cstdio>
#include <climits>
#include <unistd.h>

namespace detsim {
namespace ui {

// 静态成员初始化
std::atomic<bool> NCursesUI::resize_pending_{false};

// ============== Selection ==============

void Selection::clear() {
    start_buf_line = start_col = end_buf_line = end_col = -1;
    active = false;
    valid = false;
}

void Selection::normalize(int& l1, int& c1, int& l2, int& c2) const {
    if (start_buf_line < end_buf_line || 
        (start_buf_line == end_buf_line && start_col <= end_col)) {
        l1 = start_buf_line; c1 = start_col;
        l2 = end_buf_line; c2 = end_col;
    } else {
        l1 = end_buf_line; c1 = end_col;
        l2 = start_buf_line; c2 = start_col;
    }
}

bool Selection::contains(int buf_line, int col) const {
    if (!valid) return false;
    int l1, c1, l2, c2;
    normalize(l1, c1, l2, c2);
    if (buf_line < l1 || buf_line > l2) return false;
    if (buf_line == l1 && buf_line == l2) return col >= c1 && col <= c2;
    if (buf_line == l1) return col >= c1;
    if (buf_line == l2) return col <= c2;
    return true;
}

std::string Selection::toString() const {
    return "Selection(" + std::to_string(start_buf_line) + "," + 
           std::to_string(start_col) + " -> " + 
           std::to_string(end_buf_line) + "," + std::to_string(end_col) + ")";
}

// ============== Window ==============

Window::Window(int height, int width, int start_y, int start_x)
    : win_(nullptr), height_(height), width_(width), 
      start_y_(start_y), start_x_(start_x) {}

Window::~Window() {
    if (win_) delwin(win_);
}

void Window::init() {
    win_ = newwin(height_, width_, start_y_, start_x_);
    leaveok(win_, TRUE);
}

void Window::resize(int height, int width, int start_y, int start_x) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    height_ = height; width_ = width;
    start_y_ = start_y; start_x_ = start_x;
    if (win_) {
        wresize(win_, height_, width_);
        mvwin(win_, start_y_, start_x_);
    }
}

void Window::print(const std::string& text) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!win_) return;
    wprintw(win_, "%s", text.c_str());
    wrefresh(win_);
}

void Window::display(const std::string& text, int y, int x) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!win_) return;
    mvwprintw(win_, y, x, "%s", text.c_str());
    wrefresh(win_);
}

void Window::clear() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!win_) return;
    werase(win_);
    wrefresh(win_);
}

void Window::refresh() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!win_) return;
    wrefresh(win_);
}

// ============== ContentWindow ==============

ContentWindow::ContentWindow(int height, int width, int start_y, int start_x,
                             size_t max_lines)
    : Window(height, width, start_y, start_x), 
      max_lines_(max_lines), scroll_offset_(0) {}

void ContentWindow::init() {
    Window::init();
    scrollok(win_, TRUE);
    idlok(win_, TRUE);
    keypad(win_, TRUE);
    nodelay(win_, FALSE);
    
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    mouseinterval(0);
}

void ContentWindow::resize(int height, int width, int start_y, int start_x) {
    Window::resize(height, width, start_y, start_x);
    render_buffer();
}

int ContentWindow::window_row_to_buffer_line(int win_row, int* out_col_offset) {
    int content_height = height_ - 1;
    size_t total_lines = buffer_.size();
    
    if (out_col_offset) *out_col_offset = 0;
    
    if (total_lines == 0) return -1;
    
    // 计算软换行后的总行数（与 render_buffer 一致）
    auto calc_wrapped_rows = [&](size_t buf_idx) -> int {
        if (buf_idx >= buffer_.size()) return 0;
        const std::string& line = buffer_[buf_idx];
        if (line.empty()) return 1;
        return (line.length() + width_ - 1) / width_;
    };
    
    // 计算所有行的软换行总数
    size_t total_wrapped_rows = 0;
    for (size_t i = 0; i < total_lines; i++) {
        total_wrapped_rows += calc_wrapped_rows(i);
    }
    
    // 确定起始逻辑行和偏移（与 render_buffer 逻辑一致）
    size_t start_line = 0;
    int rows_to_skip = 0;
    size_t current_wrapped_row = 0;
    
    if (total_wrapped_rows > (size_t)content_height) {
        size_t max_offset = total_wrapped_rows - content_height;
        size_t actual_offset = std::min(scroll_offset_, max_offset);
        size_t target_skip = total_wrapped_rows - content_height - actual_offset;
        
        size_t wrapped_count = 0;
        for (size_t i = 0; i < total_lines; i++) {
            size_t line_rows = calc_wrapped_rows(i);
            if (wrapped_count + line_rows > target_skip) {
                start_line = i;
                rows_to_skip = target_skip - wrapped_count;
                current_wrapped_row = 0;  // 从 start_line 的 rows_to_skip 开始算
                break;
            }
            wrapped_count += line_rows;
        }
    }
    
    // 找到 win_row 对应的逻辑行
    // win_row 是屏幕上的行号（0=屏幕顶部）
    // 需要计算屏幕第 win_row 行对应哪个逻辑行的哪个偏移
    
    // 从 start_line 的 rows_to_skip 偏移开始算
    size_t screen_row = 0;
    
    for (size_t buf_idx = start_line; buf_idx < total_lines; buf_idx++) {
        int line_rows = calc_wrapped_rows(buf_idx);
        int start_row_in_line = (buf_idx == start_line) ? rows_to_skip : 0;
        
        for (int line_row = start_row_in_line; line_row < line_rows; line_row++) {
            if (screen_row == (size_t)win_row) {
                if (out_col_offset) *out_col_offset = line_row * width_;
                return buf_idx;
            }
            screen_row++;
        }
    }
    
    return -1;
}

std::string ContentWindow::extract_selected_text() {
    if (!selection_.valid) return "";
    
    int l1, c1, l2, c2;
    selection_.normalize(l1, c1, l2, c2);
    
    std::ostringstream result;
    bool first_line = true;
    
    for (int buf_line = l1; buf_line <= l2 && buf_line < (int)buffer_.size(); buf_line++) {
        if (buf_line < 0) continue;
        
        const std::string& line = buffer_[buf_line];
        int start_col = (buf_line == l1) ? c1 : 0;
        int end_col = (buf_line == l2) ? c2 : (int)line.length() - 1;
        
        if (end_col >= (int)line.length()) end_col = line.length() - 1;
        if (start_col < 0) start_col = 0;
        if (start_col > end_col) continue;
        
        if (!first_line) result << '\n';
        first_line = false;
        
        result << line.substr(start_col, end_col - start_col + 1);
    }
    
    return result.str();
}

void ContentWindow::render_buffer() {
    if (!win_) return;
    
    werase(win_);
    
    int content_height = height_ - 1;
    size_t total_lines = buffer_.size();
    
    // 计算软换行后的总行数
    auto calc_wrapped_rows = [&](size_t buf_idx) -> int {
        if (buf_idx >= buffer_.size()) return 0;
        const std::string& line = buffer_[buf_idx];
        if (line.empty()) return 1;
        return (line.length() + width_ - 1) / width_;  // 向上取整
    };
    
    // 计算累计的软换行偏移
    size_t total_wrapped_rows = 0;
    for (size_t i = 0; i < total_lines; i++) {
        total_wrapped_rows += calc_wrapped_rows(i);
    }
    
    // 确定起始逻辑行和其在显示中的偏移
    // scroll_offset 表示从底部往上的偏移量（值越大，显示越旧的内容）
    size_t start_line = 0;
    int rows_to_skip = 0;  // 在起始行中要跳过的显示行数（从该行的开头跳过）
    
    if (total_wrapped_rows > (size_t)content_height) {
        size_t max_offset = total_wrapped_rows - content_height;
        scroll_offset_ = std::min(scroll_offset_, max_offset);
        
        // 从底部往上数 scroll_offset + content_height 行，找到起始位置
        // 比如总共有100显示行，屏幕高20，scroll_offset=5
        // 我们要显示从第 (100-20-5)=75 行开始的内容
        size_t target_skip = total_wrapped_rows - content_height - scroll_offset_;
        
        size_t wrapped_count = 0;
        for (size_t i = 0; i < total_lines; i++) {
            size_t line_rows = calc_wrapped_rows(i);
            if (wrapped_count + line_rows > target_skip) {
                start_line = i;
                // 在这一行内跳过多少行
                rows_to_skip = target_skip - wrapped_count;
                break;
            }
            wrapped_count += line_rows;
        }
    } else {
        scroll_offset_ = 0;
        start_line = 0;
    }
    
    // 绘制内容行（支持软换行）
    int row = 0;
    for (size_t buf_idx = start_line; buf_idx < total_lines && row < content_height; buf_idx++) {
        const std::string& line = buffer_[buf_idx];
        int line_rows = calc_wrapped_rows(buf_idx);
        
        // 处理从中间行开始显示的情况
        int start_row_in_line = 0;
        if (buf_idx == start_line && rows_to_skip > 0) {
            start_row_in_line = rows_to_skip;
        }
        
        // 绘制该逻辑行的各个软换行片段
        for (int line_row = start_row_in_line; line_row < line_rows && row < content_height; line_row++) {
            size_t col_start = line_row * width_;
            size_t col_end = std::min(col_start + width_, line.length());
            
            for (size_t col = col_start; col < col_end; col++) {
                int screen_col = col - col_start;
                if (selection_.valid && selection_.contains(buf_idx, col)) {
                    wattron(win_, A_REVERSE);
                    mvwaddch(win_, row, screen_col, line[col]);
                    wattroff(win_, A_REVERSE);
                } else {
                    mvwaddch(win_, row, screen_col, line[col]);
                }
            }
            row++;
        }
    }
    
    // 输入行 - 根据模式显示不同的提示符
    std::string display_input = current_input_.substr(0, width_ - 4);
    int cursor_x = 2;  // 默认：普通命令模式 "> " 占 2 字符
    
    if (in_prompt_mode_) {
        // 提示模式：显示提示文本和用户输入
        std::string prompt_display = prompt_text_;
        if (prompt_display.length() > 30) {
            prompt_display = prompt_display.substr(0, 27) + "...";
        }
        // 计算光标起始位置: "[prompt] " 占 prompt_display.length() + 3 字符
        cursor_x = prompt_display.length() + 3;
        if (password_mode_) {
            // 密码模式：显示 *
            std::string stars(current_input_.length(), '*');
            mvwprintw(win_, height_ - 1, 0, "[%s] %s", prompt_display.c_str(), stars.c_str());
        } else {
            mvwprintw(win_, height_ - 1, 0, "[%s] %s", prompt_display.c_str(), display_input.c_str());
        }
    } else {
        // 普通命令模式
        mvwprintw(win_, height_ - 1, 0, "> %s", display_input.c_str());
    }
    
    cursor_x += std::min(input_cursor_pos_, (int)display_input.length());
    cursor_x = std::min(cursor_x, width_ - 1);
    wmove(win_, height_ - 1, cursor_x);
    
    curs_set(1);
    leaveok(win_, FALSE);
    wrefresh(win_);
    leaveok(win_, TRUE);
}

void ContentWindow::add_line(const std::string& line) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    buffer_.push_back(line);
    if (buffer_.size() > max_lines_) {
        buffer_.pop_front();
    }
    line_incomplete_ = false;  // add_line 添加的是完整行
    scroll_offset_ = 0;
    render_buffer();
}

void ContentWindow::print(const std::string& text) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (text.empty()) return;
    
    size_t start = 0;
    size_t end = text.find('\n');
    
    // 如果之前有未完成的行，先追加到最后一行
    if (line_incomplete_ && !buffer_.empty()) {
        if (end != std::string::npos) {
            // 有换行：追加第一部分，然后处理剩余
            buffer_.back() += text.substr(start, end - start);
            start = end + 1;
            end = text.find('\n', start);
            line_incomplete_ = false;
        } else {
            // 无换行：全部追加到最后一行
            buffer_.back() += text.substr(start);
            // line_incomplete_ 保持 true
            scroll_offset_ = 0;
            render_buffer();
            return;
        }
    }
    
    // 处理剩余的换行分割
    while (end != std::string::npos) {
        buffer_.push_back(text.substr(start, end - start));
        if (buffer_.size() > max_lines_) {
            buffer_.pop_front();
        }
        start = end + 1;
        end = text.find('\n', start);
    }
    
    // 处理最后一部分（无换行）
    if (start < text.length()) {
        buffer_.push_back(text.substr(start));
        if (buffer_.size() > max_lines_) {
            buffer_.pop_front();
        }
        line_incomplete_ = true;  // 标记此行未完成
    } else {
        line_incomplete_ = false;  // 以换行结尾，行已完成
    }
    
    scroll_offset_ = 0;
    render_buffer();
}

void ContentWindow::history_prev() {
    if (command_history_.empty()) return;
    
    if (history_index_ == -1) {
        saved_input_ = current_input_;
        history_index_ = command_history_.size() - 1;
    } else if (history_index_ > 0) {
        history_index_--;
    } else {
        return;
    }
    
    current_input_ = command_history_[history_index_];
    input_cursor_pos_ = current_input_.length();
    render_buffer();
}

void ContentWindow::history_next() {
    if (history_index_ == -1) return;
    
    if (history_index_ < (int)command_history_.size() - 1) {
        history_index_++;
        current_input_ = command_history_[history_index_];
    } else {
        history_index_ = -1;
        current_input_ = saved_input_;
        saved_input_.clear();
    }
    input_cursor_pos_ = current_input_.length();
    render_buffer();
}

void ContentWindow::add_to_history(const std::string& cmd) {
    if (cmd.empty()) return;
    if (command_history_.empty() || command_history_.back() != cmd) {
        command_history_.push_back(cmd);
        if (command_history_.size() > max_history_) {
            command_history_.erase(command_history_.begin());
        }
    }
    history_index_ = -1;
    saved_input_.clear();
}

void ContentWindow::handle_mouse_event(MEVENT& event) {
    int content_height = height_ - 1;
    int rel_y = event.y - start_y_;
    int rel_x = event.x - start_x_;
    
    // 滚轮事件
    if (event.bstate & BUTTON4_PRESSED) {
        handle_scroll_with_selection(-3, rel_y, rel_x);
        return;
    }
    if (event.bstate & BUTTON5_PRESSED) {
        handle_scroll_with_selection(3, rel_y, rel_x);
        return;
    }
    
    // 左键按下
    if (event.bstate & BUTTON1_PRESSED) {
        int col_offset = 0;
        int buf_line = window_row_to_buffer_line(rel_y, &col_offset);
        if (buf_line >= 0 && rel_y >= 0 && rel_y < content_height) {
            mouse_button1_down_ = true;
            mouse_rel_y_ = rel_y;
            mouse_rel_x_ = rel_x;
            selection_.active = true;
            selection_.valid = true;
            selection_.start_buf_line = buf_line;
            selection_.start_col = col_offset + rel_x;
            selection_.end_buf_line = buf_line;
            selection_.end_col = col_offset + rel_x;
            render_buffer();
        }
        return;
    }
    
    // 左键释放
    if (event.bstate & BUTTON1_RELEASED) {
        if (mouse_button1_down_) {
            mouse_button1_down_ = false;
            mouse_rel_y_ = -1;
            mouse_rel_x_ = -1;
            int col_offset = 0;
            int buf_line = window_row_to_buffer_line(rel_y, &col_offset);
            if (buf_line >= 0) {
                selection_.end_buf_line = buf_line;
                selection_.end_col = col_offset + rel_x;
            }
            selection_.active = false;
            selection_.valid = true;
            selected_text_ = extract_selected_text();
            
            if (on_selection_ && !selected_text_.empty()) {
                on_selection_(selected_text_);
            }
            if (!selected_text_.empty()) {
                copy_to_clipboard(selected_text_);
            }
            render_buffer();
        }
        return;
    }
    
    // 鼠标拖动
    bool is_drag_event = mouse_button1_down_ ||
                        (mouse_button1_down_ && 
                         ((event.bstate & BUTTON1_CLICKED) || 
                          (event.bstate & BUTTON1_DOUBLE_CLICKED) ||
                          (event.bstate & REPORT_MOUSE_POSITION)));
    
    if (is_drag_event && mouse_button1_down_) {
        mouse_rel_y_ = rel_y;
        mouse_rel_x_ = rel_x;
        
        int col_offset = 0;
        int buf_line = window_row_to_buffer_line(rel_y, &col_offset);
        
        if (buf_line >= 0 && rel_y >= 0 && rel_y < content_height) {
            selection_.end_buf_line = buf_line;
            selection_.end_col = col_offset + rel_x;
            selection_.valid = true;
            render_buffer();
        } else if (rel_y < 0) {
            if (scroll_offset_ < buffer_.size()) {
                scroll_offset_++;
                buf_line = window_row_to_buffer_line(0, &col_offset);
                if (buf_line >= 0) {
                    selection_.end_buf_line = buf_line;
                    selection_.end_col = col_offset + 0;
                    selection_.valid = true;
                    render_buffer();
                }
            }
        } else if (rel_y >= content_height) {
            if (scroll_offset_ > 0) {
                scroll_offset_--;
                buf_line = window_row_to_buffer_line(content_height - 1, &col_offset);
                if (buf_line >= 0 && buf_line < (int)buffer_.size()) {
                    selection_.end_buf_line = buf_line;
                    selection_.end_col = buffer_[buf_line].length();
                    selection_.valid = true;
                    render_buffer();
                }
            }
        }
        return;
    }
}

void ContentWindow::handle_scroll_with_selection(int direction, int mouse_y, int mouse_x) {
    int lines = (direction < 0) ? -direction : direction;
    bool up = (direction < 0);
    
    mouse_rel_y_ = mouse_y;
    mouse_rel_x_ = mouse_x;
    
    if (mouse_button1_down_ || selection_.active) {
        if (up) {
            scroll_offset_ += lines;
        } else {
            if (scroll_offset_ >= (size_t)lines) {
                scroll_offset_ -= lines;
            } else {
                scroll_offset_ = 0;
            }
        }
        
        int col_offset = 0;
        int buf_line = window_row_to_buffer_line(mouse_y, &col_offset);
        if (buf_line >= 0) {
            selection_.end_buf_line = buf_line;
            selection_.end_col = col_offset + mouse_x;
            selection_.valid = true;
        }
    } else {
        if (up) {
            scroll_offset_ += lines;
        } else {
            if (scroll_offset_ >= (size_t)lines) {
                scroll_offset_ -= lines;
            } else {
                scroll_offset_ = 0;
            }
        }
    }
    render_buffer();
}

bool ContentWindow::handle_input(int ch) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    
    if (ch == KEY_MOUSE) {
        MEVENT event;
        if (getmouse(&event) == OK) {
            handle_mouse_event(event);
        }
        return false;
    }
    
    if (ch == KEY_RESIZE) {
        return true;  // 调用者应处理 resize
    }
    
    // 提示模式处理
    if (in_prompt_mode_) {
        handle_prompt_input(ch);
        return false;
    }
    
    // 按键清除选区
    if (ch != ERR && ch != KEY_UP && ch != KEY_DOWN) {
        selection_.clear();
        selected_text_.clear();
    }
    
    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        std::string cmd = current_input_;
        buffer_.push_back("> " + current_input_);
        if (buffer_.size() > max_lines_) buffer_.pop_front();
        line_incomplete_ = false;  // 命令行是完整行
        add_to_history(current_input_);
        scroll_offset_ = 0;
        current_input_.clear();
        input_cursor_pos_ = 0;
        selection_.clear();
        selected_text_.clear();
        mouse_button1_down_ = false;
        render_buffer();
        
        // 在锁外调用回调，避免死锁
        if (on_command_) {
            mutex_.unlock();
            on_command_(cmd);
            mutex_.lock();
        }
        return true;
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b' || ch == KEY_DC) {
        if (!current_input_.empty() && input_cursor_pos_ > 0) {
            current_input_.erase(input_cursor_pos_ - 1, 1);
            input_cursor_pos_--;
            if (history_index_ != -1) {
                history_index_ = -1;
                saved_input_.clear();
            }
            render_buffer();
        }
    } else if (ch == KEY_LEFT) {
        if (input_cursor_pos_ > 0) {
            input_cursor_pos_--;
            render_buffer();
        }
    } else if (ch == KEY_RIGHT) {
        if (input_cursor_pos_ < (int)current_input_.length()) {
            input_cursor_pos_++;
            render_buffer();
        }
    } else if (ch == KEY_UP) {
        history_prev();
    } else if (ch == KEY_DOWN) {
        history_next();
    } else if (ch == KEY_HOME) {
        input_cursor_pos_ = 0;
        render_buffer();
    } else if (ch == KEY_END) {
        input_cursor_pos_ = current_input_.length();
        render_buffer();
    } else if (ch >= 32 && ch < 127) {
        if (current_input_.length() < (size_t)(width_ - 4)) {
            current_input_.insert(input_cursor_pos_, 1, (char)ch);
            input_cursor_pos_++;
            if (history_index_ != -1) {
                history_index_ = -1;
                saved_input_.clear();
            }
            render_buffer();
        }
    }
    return false;
}

std::string ContentWindow::get_current_input() const {
    return current_input_;
}

void ContentWindow::clear_input() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    current_input_.clear();
    input_cursor_pos_ = 0;
    history_index_ = -1;
    saved_input_.clear();
    render_buffer();
}

std::string ContentWindow::get_selected_text() const {
    return selected_text_;
}

bool ContentWindow::has_selection() const {
    return selection_.valid;
}

void ContentWindow::clear_selection() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    selection_.clear();
    selected_text_.clear();
    mouse_button1_down_ = false;
    render_buffer();
}

size_t ContentWindow::get_history_size() const {
    return command_history_.size();
}

void ContentWindow::set_on_command(std::function<void(const std::string&)> callback) {
    on_command_ = callback;
}

void ContentWindow::set_on_selection(std::function<void(const std::string&)> callback) {
    on_selection_ = callback;
}

void ContentWindow::activate_input() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!win_) return;
    
    int cursor_x;
    if (in_prompt_mode_) {
        std::string prompt_display = prompt_text_;
        if (prompt_display.length() > 30) {
            prompt_display = prompt_display.substr(0, 27) + "...";
        }
        // "[prompt] " 占 prompt_display.length() + 3 字符
        cursor_x = prompt_display.length() + 3;
    } else {
        cursor_x = 2;  // 普通命令模式 "> "
    }
    
    cursor_x += std::min(input_cursor_pos_, (int)current_input_.length());
    cursor_x = std::min(cursor_x, width_ - 1);
    wmove(win_, height_ - 1, cursor_x);
    curs_set(1);
    leaveok(win_, FALSE);
    wrefresh(win_);
    leaveok(win_, TRUE);
}

void ContentWindow::scroll_up(int lines) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    scroll_offset_ += lines;
    render_buffer();
}

void ContentWindow::scroll_down(int lines) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (scroll_offset_ >= (size_t)lines) {
        scroll_offset_ -= lines;
    } else {
        scroll_offset_ = 0;
    }
    render_buffer();
}

// ============== StatusWindow ==============

StatusWindow::StatusWindow(int height, int width, int start_y, int start_x)
    : Window(height, width, start_y, start_x) {}

void StatusWindow::init() {
    Window::init();
    scrollok(win_, FALSE);
}

void StatusWindow::resize(int height, int width, int start_y, int start_x) {
    Window::resize(height, width, start_y, start_x);
    refresh_display();
}

void StatusWindow::set_content_window(ContentWindow* ref) {
    content_win_ref_ = ref;
}

void StatusWindow::set_line(int row, const std::string& text) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // 找到或添加
    for (auto& line : status_lines_) {
        if (line.first == row) {
            line.second = text;
            return;
        }
    }
    status_lines_.push_back({row, text});
}

void StatusWindow::clear_line(int row) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    status_lines_.erase(
        std::remove_if(status_lines_.begin(), status_lines_.end(),
            [row](const auto& p) { return p.first == row; }),
        status_lines_.end());
}

void StatusWindow::refresh_display() {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!win_) return;
        
        werase(win_);
        box(win_, 0, 0);
        
        for (const auto& line : status_lines_) {
            if (line.first >= 0 && line.first < height_) {
                mvwprintw(win_, line.first, 2, "%s", line.second.c_str());
            }
        }
        
        wrefresh(win_);
    }
    
    // 在锁外调用 activate_input，避免死锁
    if (content_win_ref_) {
        content_win_ref_->activate_input();
    }
}

// ============== NCursesUI ==============

NCursesUI::NCursesUI(int status_height) 
    : status_height_(status_height), running_(false), initialized_(false) {}

NCursesUI::~NCursesUI() {
    shutdown();
}

bool NCursesUI::init() {
    if (initialized_) return true;
    
    initscr();
    cbreak();
    noecho();
    
    if (has_colors()) {
        start_color();
        use_default_colors();
    }
    
    // 设置信号处理
    std::signal(SIGWINCH, on_resize_signal);
    
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    
    int content_height = rows - status_height_;
    
    content_win_ = new ContentWindow(content_height, cols, 0, 0, 10000);
    status_win_ = new StatusWindow(status_height_, cols, content_height, 0);
    
    status_win_->set_content_window(content_win_);
    
    content_win_->init();
    status_win_->init();
    
    // 启用扩展鼠标事件
    printf("\033[?1002h");
    fflush(stdout);
    
    // 初始化状态行
    status_win_->set_line(1, "Status Window");
    status_win_->refresh_display();
    
    initialized_ = true;
    return true;
}

void NCursesUI::shutdown() {
    if (!initialized_) return;
    
    delete content_win_;
    delete status_win_;
    content_win_ = nullptr;
    status_win_ = nullptr;
    
    endwin();
    initialized_ = false;
}

void NCursesUI::on_resize_signal(int sig) {
    resize_pending_.store(true);
}

bool NCursesUI::check_resize() {
    return resize_pending_.load();
}

void NCursesUI::handle_resize() {
    if (!initialized_) return;
    
    resize_pending_.store(false);
    
    endwin();
    refresh();
    
    // 重新启用鼠标事件（endwin/refresh 会重置）
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    mouseinterval(0);
    // 启用扩展鼠标事件
    printf("\033[?1002h");
    fflush(stdout);
    
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    
    int content_height = rows - status_height_;
    if (content_height < 5) content_height = 5;
    
    content_win_->resize(content_height, cols, 0, 0);
    status_win_->resize(status_height_, cols, content_height, 0);
    
    content_win_->refresh();
    status_win_->refresh_display();
}

void NCursesUI::run() {
    if (!initialized_) return;
    
    running_ = true;
    g_ui_should_exit = false;  // 重置退出标志
    
    while (running_) {
        // 检查退出请求
        if (g_ui_should_exit) {
            running_ = false;
            break;
        }
        
        if (check_resize()) {
            handle_resize();
        }
        
        // 使用非阻塞读取，以便检查退出标志
        wtimeout(content_win_->get_window(), 100);  // 100ms 超时
        int ch = wgetch(content_win_->get_window());
        wtimeout(content_win_->get_window(), -1);   // 恢复阻塞
        
        if (ch == ERR) {
            // 超时，继续循环检查退出标志
            continue;
        }
        
        if (check_resize()) {
            handle_resize();
            continue;
        }
        
        if (ch == KEY_RESIZE) {
            handle_resize();
            continue;
        }
        
        content_win_->handle_input(ch);
    }
}

bool NCursesUI::poll_input(int timeout_ms) {
    if (!initialized_) return false;
    
    if (check_resize()) {
        handle_resize();
    }
    
    // 设置超时
    wtimeout(content_win_->get_window(), timeout_ms);
    int ch = wgetch(content_win_->get_window());
    wtimeout(content_win_->get_window(), -1);  // 恢复阻塞
    
    if (ch == ERR) return false;
    
    if (ch == KEY_RESIZE || check_resize()) {
        handle_resize();
        return true;
    }
    
    return content_win_->handle_input(ch);
}

void NCursesUI::print(const std::string& text) {
    if (content_win_) content_win_->print(text);
}

void NCursesUI::add_line(const std::string& line) {
    if (content_win_) content_win_->add_line(line);
}

void NCursesUI::set_status_line(int row, const std::string& text) {
    if (status_win_) {
        status_win_->set_line(row, text);
        status_win_->refresh_display();
    }
}

void NCursesUI::set_on_command(std::function<void(const std::string&)> callback) {
    if (content_win_) content_win_->set_on_command(callback);
}

void NCursesUI::set_on_selection(std::function<void(const std::string&)> callback) {
    if (content_win_) content_win_->set_on_selection(callback);
}

// ============== Utility Functions ==============

// Base64 编码辅助函数（用于 OSC 52）
static const char base64_chars[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const std::string& input) {
    std::string ret;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];
    size_t in_len = input.length();
    const unsigned char* bytes_to_encode = reinterpret_cast<const unsigned char*>(input.c_str());
    
    while (in_len--) {
        char_array_3[i++] = *(bytes_to_encode++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for(i = 0; i < 4; i++)
                ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    
    if (i) {
        for(j = i; j < 3; j++)
            char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        for (j = 0; j < (i + 1); j++)
            ret += base64_chars[char_array_4[j]];
        while((i++ < 3))
            ret += '=';
    }
    
    return ret;
}

// 使用 OSC 52 转义序列复制到剪贴板（支持 tmux、Windows Terminal 等）
static bool copy_to_clipboard_osc52(const std::string& text) {
    if (text.empty()) return false;
    if (text.length() > 100000) return false;  // OSC 52 有长度限制
    
    std::string encoded = base64_encode(text);
    std::string osc52;
    
    // 检测是否在 tmux 中
    const char* tmux = getenv("TMUX");
    if (tmux) {
        // tmux 需要特殊的转义序列
        osc52 = "\033Ptmux;\033\033]52;c;" + encoded + "\033\\\033\\";
    } else {
        osc52 = "\033]52;c;" + encoded + "\033\\";
    }
    
    // 写入终端
    ssize_t written = write(STDOUT_FILENO, osc52.c_str(), osc52.length());
    return written == (ssize_t)osc52.length();
}

bool copy_to_clipboard(const std::string& text) {
    if (text.empty()) return false;
    
    FILE* xclip = popen("xclip -selection clipboard 2>/dev/null", "w");
    if (xclip) {
        fwrite(text.c_str(), 1, text.length(), xclip);
        if (pclose(xclip) == 0) return true;
    }
    
    FILE* wlcopy = popen("wl-copy 2>/dev/null", "w");
    if (wlcopy) {
        fwrite(text.c_str(), 1, text.length(), wlcopy);
        if (pclose(wlcopy) == 0) return true;
    }
    
    FILE* xsel = popen("xsel -b -i 2>/dev/null", "w");
    if (xsel) {
        fwrite(text.c_str(), 1, text.length(), xsel);
        if (pclose(xsel) == 0) return true;
    }
    
    FILE* pbcopy = popen("pbcopy 2>/dev/null", "w");
    if (pbcopy) {
        fwrite(text.c_str(), 1, text.length(), pbcopy);
        if (pclose(pbcopy) == 0) return true;
    }
    
    FILE* clip = popen("clip.exe 2>/dev/null", "w");
    if (clip) {
        fwrite(text.c_str(), 1, text.length(), clip);
        if (pclose(clip) == 0) return true;
    }
    
    std::string ps_cmd = "powershell.exe -command \"Set-Clipboard -Value '" + text + "'\" 2>/dev/null";
    FILE* ps = popen(ps_cmd.c_str(), "w");
    if (ps) {
        if (pclose(ps) == 0) return true;
    }
    
    return false;
}

// ============== ContentWindow Prompt Methods ==============

void ContentWindow::enter_prompt_mode(const std::string& prompt_text, 
                                      const std::string& default_value,
                                      bool password) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    in_prompt_mode_ = true;
    prompt_text_ = prompt_text;
    password_mode_ = password;
    prompt_completed_ = false;
    prompt_cancelled_ = false;
    prompt_result_.clear();
    
    // 如果有默认值，预填充输入
    if (!default_value.empty()) {
        current_input_ = default_value;
        input_cursor_pos_ = current_input_.length();
    } else {
        current_input_.clear();
        input_cursor_pos_ = 0;
    }
    
    render_buffer();
}

void ContentWindow::exit_prompt_mode() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    in_prompt_mode_ = false;
    password_mode_ = false;
    prompt_text_.clear();
    current_input_.clear();
    input_cursor_pos_ = 0;
    render_buffer();
}

std::string ContentWindow::wait_for_prompt_result() {
    // 等待提示完成（阻塞）
    while (in_prompt_mode_ && !prompt_completed_ && !prompt_cancelled_) {
        // 使用短超时轮询
        wtimeout(win_, 50);
        int ch = wgetch(win_);
        wtimeout(win_, -1);  // 恢复阻塞
        
        if (ch != ERR) {
            if (ch == KEY_RESIZE) {
                // 调整大小事件，由上层处理
                continue;
            }
            handle_input(ch);
        }
    }
    
    std::string result = prompt_result_;
    exit_prompt_mode();
    return result;
}

void ContentWindow::handle_prompt_input(int ch) {
    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        // 完成输入
        prompt_result_ = current_input_;
        
        // 去除首尾空格
        size_t start = prompt_result_.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) {
            size_t end = prompt_result_.find_last_not_of(" \t\r\n");
            prompt_result_ = prompt_result_.substr(start, end - start + 1);
        } else {
            prompt_result_.clear();
        }
        
        prompt_completed_ = true;
        render_buffer();
        return;
    }
    
    if (ch == 27) {  // ESC 键取消
        prompt_cancelled_ = true;
        prompt_result_.clear();
        render_buffer();
        return;
    }
    
    if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b' || ch == KEY_DC) {
        if (!current_input_.empty() && input_cursor_pos_ > 0) {
            current_input_.erase(input_cursor_pos_ - 1, 1);
            input_cursor_pos_--;
            render_buffer();
        }
        return;
    }
    
    if (ch == KEY_LEFT) {
        if (input_cursor_pos_ > 0) {
            input_cursor_pos_--;
            render_buffer();
        }
        return;
    }
    
    if (ch == KEY_RIGHT) {
        if (input_cursor_pos_ < (int)current_input_.length()) {
            input_cursor_pos_++;
            render_buffer();
        }
        return;
    }
    
    if (ch == KEY_HOME) {
        input_cursor_pos_ = 0;
        render_buffer();
        return;
    }
    
    if (ch == KEY_END) {
        input_cursor_pos_ = current_input_.length();
        render_buffer();
        return;
    }
    
    if (ch >= 32 && ch < 127) {
        // 可打印字符
        if (current_input_.length() < (size_t)(width_ - 4)) {
            current_input_.insert(input_cursor_pos_, 1, (char)ch);
            input_cursor_pos_++;
            render_buffer();
        }
        return;
    }
}

std::string ContentWindow::prompt(const std::string& prompt_text, 
                                  const std::string& default_value) {
    enter_prompt_mode(prompt_text, default_value, false);
    return wait_for_prompt_result();
}

int ContentWindow::prompt_int(const std::string& prompt_text, 
                              int default_value,
                              int min_val, 
                              int max_val) {
    std::string default_str = (default_value != 0) ? std::to_string(default_value) : "";
    
    while (true) {
        std::string input = prompt(prompt_text + " (" + std::to_string(min_val) + 
                                   "-" + std::to_string(max_val) + ")", default_str);
        
        if (input.empty()) {
            return default_value;  // 取消或空输入，返回默认值
        }
        
        try {
            size_t pos;
            int value = std::stoi(input, &pos);
            if (pos != input.length()) {
                add_line("Invalid number: " + input);
                continue;
            }
            if (value < min_val || value > max_val) {
                add_line("Out of range! Must be between " + 
                        std::to_string(min_val) + " and " + std::to_string(max_val));
                default_str = input;  // 保留输入，方便修改
                continue;
            }
            return value;
        } catch (const std::exception&) {
            add_line("Invalid number: " + input);
        }
    }
}

double ContentWindow::prompt_float(const std::string& prompt_text,
                                   double default_value,
                                   double min_val,
                                   double max_val) {
    std::string default_str = (default_value != 0.0) ? std::to_string(default_value) : "";
    
    while (true) {
        std::string input = prompt(prompt_text, default_str);
        
        if (input.empty()) {
            return default_value;
        }
        
        try {
            size_t pos;
            double value = std::stod(input, &pos);
            if (pos != input.length()) {
                add_line("Invalid number: " + input);
                continue;
            }
            if (value < min_val || value > max_val) {
                add_line("Out of range!");
                default_str = input;
                continue;
            }
            return value;
        } catch (const std::exception&) {
            add_line("Invalid number: " + input);
        }
    }
}

bool ContentWindow::prompt_yn(const std::string& prompt_text, bool default_yes) {
    std::string default_str = default_yes ? "Y/n" : "y/N";
    
    while (true) {
        std::string input = prompt(prompt_text + " (" + default_str + ")", 
                                   default_yes ? "y" : "n");
        
        if (input.empty()) {
            return default_yes;
        }
        
        // 取第一个字符并转为小写
        char c = std::tolower(input[0]);
        if (c == 'y') return true;
        if (c == 'n') return false;
        
        add_line("Please enter 'y' or 'n'");
    }
}

int ContentWindow::prompt_choice(const std::string& prompt_text,
                                 const std::vector<std::string>& options,
                                 int default_idx) {
    if (options.empty()) return -1;
    
    // 显示选项
    add_line(prompt_text);
    for (size_t i = 0; i < options.size(); i++) {
        std::string marker = (i == (size_t)default_idx) ? " [default]" : "";
        add_line("  " + std::to_string(i+1) + ". " + options[i] + marker);
    }
    
    std::string default_str;
    if (default_idx >= 0 && default_idx < (int)options.size()) {
        default_str = std::to_string(default_idx + 1);
    }
    
    while (true) {
        std::string input = prompt("Enter choice (1-" + std::to_string(options.size()) + "):", 
                                   default_str);
        
        if (input.empty()) {
            return default_idx;  // 返回默认值
        }
        
        try {
            size_t pos;
            int choice = std::stoi(input, &pos);
            if (pos != input.length()) {
                add_line("Invalid number");
                continue;
            }
            choice--;  // 转为 0-based
            if (choice < 0 || choice >= (int)options.size()) {
                add_line("Out of range");
                continue;
            }
            return choice;
        } catch (const std::exception&) {
            add_line("Invalid number");
        }
    }
}

std::string ContentWindow::prompt_validate(const std::string& prompt_text,
                                           std::function<bool(const std::string&)> validator,
                                           const std::string& error_msg) {
    while (true) {
        std::string input = prompt(prompt_text);
        
        if (input.empty()) {
            return "";  // 取消
        }
        
        if (validator(input)) {
            return input;
        }
        
        add_line(error_msg);
    }
}

std::string ContentWindow::prompt_password(const std::string& prompt_text) {
    enter_prompt_mode(prompt_text, "", true);
    return wait_for_prompt_result();
}

} // namespace ui
} // namespace detsim

#ifndef NCURSES_UI_H
#define NCURSES_UI_H

#include <ncurses.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <functional>
#include <atomic>
#include <climits>

namespace detsim {
namespace ui {

// 选区结构
struct Selection {
    int start_buf_line = -1;
    int start_col = -1;
    int end_buf_line = -1;
    int end_col = -1;
    bool active = false;
    bool valid = false;
    
    void clear();
    void normalize(int& l1, int& c1, int& l2, int& c2) const;
    bool contains(int buf_line, int col) const;
    std::string toString() const;
};

// 窗口基类
class Window {
protected:
    WINDOW* win_;
    int height_, width_, start_y_, start_x_;
    std::recursive_mutex mutex_;  // 使用递归锁，允许同一线程多次获取
    
public:
    Window(int height, int width, int start_y, int start_x);
    virtual ~Window();
    
    virtual void init();
    virtual void resize(int height, int width, int start_y, int start_x);
    virtual void print(const std::string& text);
    virtual void display(const std::string& text, int y, int x);
    void clear();
    void refresh();
    
    WINDOW* get_window() { return win_; }
    int get_height() const { return height_; }
    int get_width() const { return width_; }
    int get_start_y() const { return start_y_; }
    int get_start_x() const { return start_x_; }
};

// 带滚动缓冲区的内容窗口
class ContentWindow : public Window {
private:
    std::deque<std::string> buffer_;
    size_t max_lines_;
    size_t scroll_offset_ = 0;
    std::string current_input_;
    int input_cursor_pos_ = 0;
    
    // 命令历史
    std::vector<std::string> command_history_;
    size_t max_history_ = 1000;
    int history_index_ = -1;
    std::string saved_input_;
    
    // 选区
    Selection selection_;
    std::string selected_text_;
    
    // 鼠标状态
    bool mouse_button1_down_ = false;
    int mouse_rel_y_ = -1;
    int mouse_rel_x_ = -1;
    
    // 提示模式状态
    bool in_prompt_mode_ = false;
    std::string prompt_text_;
    std::string prompt_result_;
    bool prompt_completed_ = false;
    bool prompt_cancelled_ = false;
    bool password_mode_ = false;  // 密码输入模式（显示*）
    
    // 流式输出状态（用于 print 追加到未完成行）
    bool line_incomplete_ = false;
    
    // 回调
    std::function<void(const std::string&)> on_command_;
    std::function<void(const std::string&)> on_selection_;
    
    // 内部方法
    int window_row_to_buffer_line(int win_row, int* out_col_offset = nullptr);
    std::string extract_selected_text();
    void render_buffer();
    void handle_mouse_event(MEVENT& event);
    void handle_scroll_with_selection(int direction, int mouse_y, int mouse_x);
    void history_prev();
    void history_next();
    void add_to_history(const std::string& cmd);
    
    // 提示模式内部方法
    void enter_prompt_mode(const std::string& prompt_text, 
                          const std::string& default_value = "",
                          bool password = false);
    void exit_prompt_mode();
    std::string wait_for_prompt_result();
    void handle_prompt_input(int ch);
    
public:
    ContentWindow(int height, int width, int start_y, int start_x, 
                  size_t max_lines = 10000);
    
    void init() override;
    void resize(int height, int width, int start_y, int start_x) override;
    
    // 内容输出
    void add_line(const std::string& line);
    void print(const std::string& text) override;
    
    // 输入处理
    bool handle_input(int ch);
    
    // 获取当前输入
    std::string get_current_input() const;
    void clear_input();
    
    // 获取选区信息
    std::string get_selected_text() const;
    bool has_selection() const;
    void clear_selection();
    
    // 获取历史数量
    size_t get_history_size() const;
    
    // 设置回调
    void set_on_command(std::function<void(const std::string&)> callback);
    void set_on_selection(std::function<void(const std::string&)> callback);
    
    // 激活输入（移动光标）
    void activate_input();
    
    // 滚动
    void scroll_up(int lines = 3);
    void scroll_down(int lines = 3);
    
    // ========== 同步输入接口（替代 scanf） ==========
    
    // 基本提示输入 - 显示提示信息，等待用户输入，返回输入字符串
    // prompt: 提示文本（如 "Enter value: "）
    // default_value: 默认值（用户直接回车则使用此值）
    // 返回: 用户输入的字符串（去除首尾空格），取消输入返回空字符串
    std::string prompt(const std::string& prompt_text, 
                       const std::string& default_value = "");
    
    // 整数输入
    // prompt: 提示文本
    // default_value: 默认值
    // min_val, max_val: 有效范围（可选）
    // 返回: 输入的整数，取消输入返回 default_value
    int prompt_int(const std::string& prompt_text, 
                   int default_value = 0,
                   int min_val = INT_MIN, 
                   int max_val = INT_MAX);
    
    // 浮点数输入
    double prompt_float(const std::string& prompt_text,
                        double default_value = 0.0,
                        double min_val = -1e308,
                        double max_val = 1e308);
    
    // Yes/No 确认
    // 返回: true 表示 Yes，false 表示 No 或取消
    bool prompt_yn(const std::string& prompt_text, bool default_yes = true);
    
    // 选项选择
    // options: 选项列表
    // default_idx: 默认选中索引（-1 表示无默认）
    // 返回: 选中选项的索引，取消返回 -1
    int prompt_choice(const std::string& prompt_text,
                      const std::vector<std::string>& options,
                      int default_idx = -1);
    
    // 带验证的输入
    // validator: 验证函数，返回 true 表示输入有效
    std::string prompt_validate(const std::string& prompt_text,
                                std::function<bool(const std::string&)> validator,
                                const std::string& error_msg = "Invalid input, try again");
    
    // 密码输入（显示为 *）
    std::string prompt_password(const std::string& prompt_text);
};

// 状态窗口
class StatusWindow : public Window {
private:
    ContentWindow* content_win_ref_ = nullptr;
    std::vector<std::pair<int, std::string>> status_lines_;
    
public:
    StatusWindow(int height, int width, int start_y, int start_x);
    
    void init() override;
    void resize(int height, int width, int start_y, int start_x) override;
    
    // 设置关联的内容窗口（用于获取选择信息等）
    void set_content_window(ContentWindow* ref);
    
    // 更新状态行
    void set_line(int row, const std::string& text);
    void clear_line(int row);
    
    // 刷新显示
    void refresh_display();
};

// 主 UI 管理器
class NCursesUI {
private:
    ContentWindow* content_win_ = nullptr;
    StatusWindow* status_win_ = nullptr;
    int status_height_ = 8;
    bool running_ = false;
    bool initialized_ = false;
    
    // 信号处理
    static std::atomic<bool> resize_pending_;
    static void on_resize_signal(int sig);
    
public:
    NCursesUI(int status_height = 8);
    ~NCursesUI();
    
    // 初始化
    bool init();
    void shutdown();
    
    // 运行主循环（阻塞）
    void run();
    
    // 单次处理输入（非阻塞）
    bool poll_input(int timeout_ms = 0);
    
    // 检查并处理窗口大小变化
    bool check_resize();
    void handle_resize();
    
    // 获取窗口引用
    ContentWindow* get_content_window() { return content_win_; }
    StatusWindow* get_status_window() { return status_win_; }
    
    // 便捷方法
    void print(const std::string& text);
    void add_line(const std::string& line);
    void set_status_line(int row, const std::string& text);
    
    // 设置回调
    void set_on_command(std::function<void(const std::string&)> callback);
    void set_on_selection(std::function<void(const std::string&)> callback);
    
    // 检查是否初始化
    bool is_initialized() const { return initialized_; }
    
    // ========== 同步输入接口（替代 scanf） ==========
    // 这些方法是 ContentWindow prompt 方法的便捷包装
    
    std::string prompt(const std::string& prompt_text, 
                       const std::string& default_value = "") {
        return content_win_ ? content_win_->prompt(prompt_text, default_value) : "";
    }
    
    int prompt_int(const std::string& prompt_text, 
                   int default_value = 0,
                   int min_val = INT_MIN, 
                   int max_val = INT_MAX) {
        return content_win_ ? content_win_->prompt_int(prompt_text, default_value, min_val, max_val) 
                           : default_value;
    }
    
    double prompt_float(const std::string& prompt_text,
                        double default_value = 0.0,
                        double min_val = -1e308,
                        double max_val = 1e308) {
        return content_win_ ? content_win_->prompt_float(prompt_text, default_value, min_val, max_val)
                           : default_value;
    }
    
    bool prompt_yn(const std::string& prompt_text, bool default_yes = true) {
        return content_win_ ? content_win_->prompt_yn(prompt_text, default_yes) : default_yes;
    }
    
    int prompt_choice(const std::string& prompt_text,
                      const std::vector<std::string>& options,
                      int default_idx = -1) {
        return content_win_ ? content_win_->prompt_choice(prompt_text, options, default_idx) : default_idx;
    }
    
    std::string prompt_validate(const std::string& prompt_text,
                                std::function<bool(const std::string&)> validator,
                                const std::string& error_msg = "Invalid input, try again") {
        return content_win_ ? content_win_->prompt_validate(prompt_text, validator, error_msg) : "";
    }
    
    std::string prompt_password(const std::string& prompt_text) {
        return content_win_ ? content_win_->prompt_password(prompt_text) : "";
    }
};

// 工具函数
bool copy_to_clipboard(const std::string& text);

} // namespace ui
} // namespace detsim

#endif // NCURSES_UI_H

// ============================================================
// HP P1102w Printer Diagnostic Tool (Complete Edition)
// - Continuous wake mode to prevent deep sleep
// - Full diagnostic UI with all buttons
// - Advanced Queue Manager (auto-refresh, age highlight, cancel options)
// - Output controls (raw/cleaned, ANSI stripping, timestamped export)
// - Auto-recovery assessment for disabled queues
// - Config persistence for all settings
//
// Requires: C++17, gtkmm-3.0, CUPS utilities (lpstat, cancel), HPLIP (hp-info),
//           optional sudo for cupsdisable/cupsenable/systemctl/journalctl.
// ============================================================

#include <gtkmm.h>
#include <giomm/file.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cctype>

// ============================================================
// Configuration
// ============================================================
static const std::string PRINTER_IP   = "192.168.4.68";
static const int         PRINTER_PORT = 9100;
static const std::string PRINTER_NAME = "HP_LaserJet_Professional_P1102w";

// Config persistence location
static std::string config_dir_path() {
    return Glib::build_filename(Glib::get_user_config_dir(), "hp_p1102w_printer_diag");
}
static std::string config_file_path() {
    return Glib::build_filename(config_dir_path(), "config.ini");
}

// ============================================================
// Helpers
// ============================================================
static inline std::string trim_copy(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c); };
    while (!s.empty() && is_space((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && is_space((unsigned char)s.back())) s.pop_back();
    return s;
}

// Strips ANSI escape sequences (best-effort)
static std::string strip_ansi(const std::string& input) {
    std::string output;
    bool in_escape = false;

    for (unsigned char c : input) {
        if (c == 0x1B) { // ESC
            in_escape = true;
            continue;
        }
        if (in_escape) {
            if (c >= '@' && c <= '~') in_escape = false;
            continue;
        }
        output.push_back((char)c);
    }
    return output;
}

static std::string now_timestamp_yyyymmdd_hhmmss() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

static void ensure_config_dir_exists() {
    auto dir = Gio::File::create_for_path(config_dir_path());
    try {
        if (!dir->query_exists()) dir->make_directory_with_parents();
    } catch (...) {
        // Non-fatal
    }
}

// ============================================================
// Data model
// ============================================================
struct PrintJob {
    std::string job_id;
    std::string user;
    std::string file;
    std::string status;
    std::optional<std::chrono::system_clock::time_point> submitted_at;
};

// ============================================================
// CupsClient abstraction
// ============================================================
class CupsClient {
public:
    explicit CupsClient(std::function<std::string(const std::string&)> exec)
        : m_exec(std::move(exec)) {}

    std::string printer_state_raw() {
        return m_exec("lpstat -p \"" + PRINTER_NAME + "\" 2>&1");
    }

    std::string printer_long_raw() {
        return m_exec("lpstat -l -p \"" + PRINTER_NAME + "\" 2>&1");
    }

    std::string get_printer_friendly_name() {
        const std::string out = printer_long_raw();
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) {
            auto pos = line.find("Description:");
            if (pos != std::string::npos) {
                std::string desc = line.substr(pos + 12);
                auto l = desc.find_first_not_of(" \t");
                auto r = desc.find_last_not_of(" \t\r\n");
                if (l != std::string::npos && r != std::string::npos) 
                    desc = desc.substr(l, r - l + 1);
                if (!desc.empty()) return desc;
            }
        }
        return PRINTER_NAME;
    }

    bool has_recoverable_reason_hint(std::string* matched_reason = nullptr) {
        std::string out = printer_long_raw();
        std::transform(out.begin(), out.end(), out.begin(), 
                      [](unsigned char c){ return (char)std::tolower(c); });

        const std::vector<std::pair<std::string, std::string>> needles = {
            {"out of paper", "out of paper"},
            {"media-empty", "media-empty"},
            {"media empty", "media empty"},
        };

        for (const auto& p : needles) {
            if (out.find(p.first) != std::string::npos) {
                if (matched_reason) *matched_reason = p.second;
                return true;
            }
        }
        return false;
    }

    bool queue_disabled() {
        return printer_state_raw().find("disabled") != std::string::npos;
    }

    std::vector<PrintJob> get_jobs() {
        std::string out = m_exec("lpstat -W not-completed -o -l 2>&1");
        if (out.find("Unknown option") != std::string::npos ||
            out.find("invalid option") != std::string::npos) {
            out = m_exec("lpstat -o -l 2>&1");
        }
        return parse_lpstat_jobs(out);
    }

    void cancel_job(const std::string& job_id) {
        m_exec("cancel '" + job_id + "' 2>&1");
    }

    void cancel_all() {
        m_exec("cancel -a 2>&1");
    }

    void cancel_all_from_user(const std::string& user) {
        for (const auto& j : get_jobs())
            if (j.user == user) cancel_job(j.job_id);
    }

    void pause_queue() {
        m_exec("sudo cupsdisable \"" + PRINTER_NAME + "\" 2>&1");
    }

    void resume_queue() {
        m_exec("sudo cupsenable \"" + PRINTER_NAME + "\" 2>&1");
    }

private:
    std::function<std::string(const std::string&)> m_exec;

    static std::optional<std::chrono::system_clock::time_point> parse_datetime_from_line(const std::string& rest) {
        static const std::vector<std::string> months = {
            "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
        };

        auto is_month = [&](const std::string& t) {
            for (const auto& m : months) if (t == m) return true;
            return false;
        };

        std::istringstream iss(rest);
        std::vector<std::string> tokens;
        for (std::string t; iss >> t;) tokens.push_back(t);

        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!is_month(tokens[i])) continue;
            if (i == 0 || i + 2 >= tokens.size()) continue;

            std::string day  = tokens[i - 1];
            std::string mon  = tokens[i];
            std::string year = tokens[i + 1];
            std::string time = tokens[i + 2];

            if (time.find(':') == std::string::npos) continue;

            std::tm tm{};
            std::istringstream dt(day + " " + mon + " " + year + " " + time);
            dt >> std::get_time(&tm, "%d %b %Y %H:%M:%S");
            if (dt.fail()) {
                std::tm tm2{};
                std::istringstream dt2(day + " " + mon + " " + year + " " + time);
                dt2 >> std::get_time(&tm2, "%d %b %Y %H:%M");
                if (dt2.fail()) continue;
                tm = tm2;
            }

            if (i + 3 < tokens.size()) {
                std::string ampm = tokens[i + 3];
                if (ampm == "AM" || ampm == "PM") {
                    int hour = tm.tm_hour;
                    if (ampm == "AM") {
                        if (hour == 12) hour = 0;
                    } else {
                        if (hour != 12) hour += 12;
                    }
                    tm.tm_hour = hour;
                }
            }

            tm.tm_isdst = -1;
            std::time_t tt = std::mktime(&tm);
            if (tt == (std::time_t)-1) continue;

            return std::chrono::system_clock::from_time_t(tt);
        }

        return std::nullopt;
    }

    static std::vector<PrintJob> parse_lpstat_jobs(const std::string& text) {
        std::vector<PrintJob> jobs;
        std::istringstream ss(text);
        std::string line;

        PrintJob current;
        bool active = false;

        auto flush = [&]() {
            if (active && !current.job_id.empty()) jobs.push_back(current);
            current = PrintJob{};
            active = false;
        };

        while (std::getline(ss, line)) {
            if (trim_copy(line).empty()) { flush(); continue; }

            bool continuation = !line.empty() && std::isspace((unsigned char)line[0]);

            if (!continuation) {
                flush();
                active = true;
                std::istringstream ls(line);
                ls >> current.job_id >> current.user;

                std::string rest;
                std::getline(ls, rest);
                rest = trim_copy(rest);

                current.status = rest;
                current.submitted_at = parse_datetime_from_line(rest);
            } else if (active) {
                std::string cont = trim_copy(line);
                if (!cont.empty()) {
                    if (!current.file.empty()) current.file += " | ";
                    current.file += cont;
                }
            }
        }

        flush();
        return jobs;
    }
};

// ============================================================
// Advanced Queue Manager Dialog
// ============================================================
class QueueDialog : public Gtk::Dialog {
public:
    QueueDialog(Gtk::Window& parent,
                CupsClient& cups,
                std::function<void(const std::string&)> log_info,
                std::function<void(const std::string&)> log_ok,
                std::function<void(const std::string&)> log_warn,
                std::function<void(const std::string&)> log_err)
        : Gtk::Dialog("Print Queue Manager", parent, true),
          m_cups(cups),
          m_log_info(std::move(log_info)),
          m_log_ok(std::move(log_ok)),
          m_log_warn(std::move(log_warn)),
          m_log_err(std::move(log_err)) {

        set_default_size(980, 480);

        m_root.set_orientation(Gtk::ORIENTATION_VERTICAL);
        m_root.set_spacing(8);
        m_root.set_border_width(10);
        get_content_area()->pack_start(m_root);

        m_controls.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
        m_controls.set_spacing(8);

        m_lbl_refresh.set_text("Auto-refresh (sec):");
        m_spin_refresh.set_range(0, 3600);
        m_spin_refresh.set_increments(1, 10);
        m_spin_refresh.set_value(5);

        m_lbl_age.set_text("Highlight older than (min):");
        m_spin_age.set_range(0, 1440);
        m_spin_age.set_increments(1, 5);
        m_spin_age.set_value(10);

        m_btn_refresh.set_label("Refresh Now");

        m_controls.pack_start(m_lbl_refresh, false, false, 0);
        m_controls.pack_start(m_spin_refresh, false, false, 0);
        m_controls.pack_start(m_lbl_age, false, false, 0);
        m_controls.pack_start(m_spin_age, false, false, 0);
        m_controls.pack_end(m_btn_refresh, false, false, 0);

        m_root.pack_start(m_controls, false, false, 0);

        m_status.set_xalign(0.0f);
        m_root.pack_start(m_status, false, false, 0);

        m_store = Gtk::ListStore::create(m_cols);
        m_tree.set_model(m_store);
        m_tree.get_selection()->set_mode(Gtk::SELECTION_SINGLE);
        m_tree.set_headers_clickable(true);

        add_text_column("Job ID",  m_cols.job_id, 220);
        add_text_column("User",    m_cols.user, 120);
        add_text_column("Age",     m_cols.age, 90);
        add_text_column("Status",  m_cols.status, 320);
        add_text_column("File",    m_cols.file, 380);

        m_scrolled.add(m_tree);
        m_scrolled.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        m_root.pack_start(m_scrolled, true, true, 0);

        m_actions.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
        m_actions.set_spacing(8);

        m_btn_cancel_selected.set_label("Cancel Selected Job");
        m_btn_cancel_user.set_label("Cancel All From Selected User");
        m_btn_cancel_all.set_label("Cancel ALL Jobs");
        m_btn_pause.set_label("Pause Queue");
        m_btn_resume.set_label("Resume Queue");

        m_actions.pack_start(m_btn_cancel_selected, false, false, 0);
        m_actions.pack_start(m_btn_cancel_user, false, false, 0);
        m_actions.pack_start(m_btn_cancel_all, false, false, 0);
        m_actions.pack_end(m_btn_resume, false, false, 0);
        m_actions.pack_end(m_btn_pause, false, false, 0);

        m_root.pack_start(m_actions, false, false, 0);

        add_button("Close", Gtk::RESPONSE_CLOSE);

        m_btn_refresh.signal_clicked().connect(sigc::mem_fun(*this, &QueueDialog::refresh));
        m_btn_cancel_selected.signal_clicked().connect(sigc::mem_fun(*this, &QueueDialog::cancel_selected));
        m_btn_cancel_user.signal_clicked().connect(sigc::mem_fun(*this, &QueueDialog::cancel_all_from_user));
        m_btn_cancel_all.signal_clicked().connect(sigc::mem_fun(*this, &QueueDialog::cancel_all_jobs));
        m_btn_pause.signal_clicked().connect(sigc::mem_fun(*this, &QueueDialog::pause_queue));
        m_btn_resume.signal_clicked().connect(sigc::mem_fun(*this, &QueueDialog::resume_queue));

        m_spin_refresh.signal_value_changed().connect(sigc::mem_fun(*this, &QueueDialog::restart_timer));
        m_spin_age.signal_value_changed().connect(sigc::mem_fun(*this, &QueueDialog::apply_highlight_only));

        refresh();
        restart_timer();

        show_all_children();
    }

private:
    struct Columns : Gtk::TreeModel::ColumnRecord {
        Columns() {
            add(job_id);
            add(user);
            add(age);
            add(status);
            add(file);
            add(age_minutes);
            add(bg_color);
            add(bg_set);
        }
        Gtk::TreeModelColumn<std::string> job_id;
        Gtk::TreeModelColumn<std::string> user;
        Gtk::TreeModelColumn<std::string> age;
        Gtk::TreeModelColumn<std::string> status;
        Gtk::TreeModelColumn<std::string> file;
        Gtk::TreeModelColumn<int>         age_minutes;
        Gtk::TreeModelColumn<std::string> bg_color;
        Gtk::TreeModelColumn<bool>        bg_set;
    };

    CupsClient& m_cups;

    std::function<void(const std::string&)> m_log_info;
    std::function<void(const std::string&)> m_log_ok;
    std::function<void(const std::string&)> m_log_warn;
    std::function<void(const std::string&)> m_log_err;

    Gtk::Box m_root{Gtk::ORIENTATION_VERTICAL};
    Gtk::Box m_controls{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Box m_actions{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::ScrolledWindow m_scrolled;
    Gtk::TreeView m_tree;

    Gtk::Label m_lbl_refresh;
    Gtk::SpinButton m_spin_refresh;
    Gtk::Label m_lbl_age;
    Gtk::SpinButton m_spin_age;

    Gtk::Label m_status;

    Gtk::Button m_btn_refresh;
    Gtk::Button m_btn_cancel_selected;
    Gtk::Button m_btn_cancel_user;
    Gtk::Button m_btn_cancel_all;
    Gtk::Button m_btn_pause;
    Gtk::Button m_btn_resume;

    Columns m_cols;
    Glib::RefPtr<Gtk::ListStore> m_store;

    sigc::connection m_timer_conn;

    void add_text_column(const Glib::ustring& title,
                         const Gtk::TreeModelColumn<std::string>& col,
                         int min_width_px) {
        auto* renderer = Gtk::manage(new Gtk::CellRendererText());
        auto* column   = Gtk::manage(new Gtk::TreeViewColumn(title, *renderer));

        column->add_attribute(renderer->property_text(), col);
        column->add_attribute(renderer->property_cell_background(), m_cols.bg_color);
        column->add_attribute(renderer->property_cell_background_set(), m_cols.bg_set);

        column->set_resizable(true);
        column->set_min_width(min_width_px);
        m_tree.append_column(*column);
    }

    static std::string fmt_age(const std::optional<std::chrono::system_clock::time_point>& submitted_at,
                               int& out_minutes) {
        out_minutes = 0;
        if (!submitted_at.has_value()) {
            out_minutes = 0;
            return "unknown";
        }

        auto now  = std::chrono::system_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::minutes>(now - *submitted_at);

        long mins = diff.count();
        if (mins < 0) mins = 0;
        out_minutes = (int)mins;

        if (mins < 1) return "<1m";
        if (mins < 60) return std::to_string(mins) + "m";

        long hours = mins / 60;
        long rem   = mins % 60;
        if (hours < 24) return std::to_string(hours) + "h " + std::to_string(rem) + "m";

        long days = hours / 24;
        hours = hours % 24;
        return std::to_string(days) + "d " + std::to_string(hours) + "h";
    }

    void set_status_line() {
        std::string raw = trim_copy(m_cups.printer_state_raw());
        bool disabled = raw.find("disabled") != std::string::npos;

        std::string summary = disabled ? "Queue Status: DISABLED / PAUSED" : "Queue Status: ENABLED";
        if (!raw.empty()) summary += "   (" + raw + ")";
        m_status.set_text(summary);
    }

    void apply_highlight_only() {
        const int threshold = (int)m_spin_age.get_value();
        const std::string color = "#3b2f1b";

        for (auto& row : m_store->children()) {
            int age = row[m_cols.age_minutes];
            bool highlight = (threshold > 0 && age >= threshold);
            row[m_cols.bg_set] = highlight;
            row[m_cols.bg_color] = highlight ? color : "";
        }
        m_tree.queue_draw();
    }

    void refresh() {
        set_status_line();
        m_store->clear();

        auto jobs = m_cups.get_jobs();
        const int threshold = (int)m_spin_age.get_value();
        const std::string color = "#3b2f1b";

        for (const auto& j : jobs) {
            auto row = *m_store->append();
            row[m_cols.job_id] = j.job_id;
            row[m_cols.user] = j.user;

            int age_min = 0;
            row[m_cols.age] = fmt_age(j.submitted_at, age_min);
            row[m_cols.age_minutes] = age_min;

            row[m_cols.status] = j.status;
            row[m_cols.file] = j.file;

            bool highlight = (threshold > 0 && age_min >= threshold);
            row[m_cols.bg_set] = highlight;
            row[m_cols.bg_color] = highlight ? color : "";
        }

        m_tree.queue_draw();
    }

    void restart_timer() {
        if (m_timer_conn.connected()) m_timer_conn.disconnect();
        int seconds = (int)m_spin_refresh.get_value();
        if (seconds <= 0) return;

        m_timer_conn = Glib::signal_timeout().connect_seconds([this]() -> bool {
            refresh();
            return true;
        }, seconds);
    }

    bool confirm_action(const Glib::ustring& title, const Glib::ustring& message) {
        Gtk::MessageDialog dlg(*this, message, false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_OK_CANCEL, true);
        dlg.set_title(title);
        int resp = dlg.run();
        return resp == Gtk::RESPONSE_OK;
    }

    Gtk::TreeModel::iterator selected_row() {
        return m_tree.get_selection()->get_selected();
    }

    void cancel_selected() {
        auto sel = selected_row();
        if (!sel) {
            m_log_warn("Queue Manager: No job selected.");
            return;
        }
        std::string job_id = (*sel)[m_cols.job_id];
        std::string user   = (*sel)[m_cols.user];

        if (!confirm_action("Confirm Cancel",
                            "Cancel selected job?\n\nJob: " + job_id + "\nUser: " + user)) return;

        m_log_info("Queue Manager: Cancelling job " + job_id + " ...");
        m_cups.cancel_job(job_id);
        m_log_ok("Queue Manager: Cancel requested for " + job_id);
        refresh();
    }

    void cancel_all_from_user() {
        auto sel = selected_row();
        if (!sel) {
            m_log_warn("Queue Manager: Select a job first to choose a user.");
            return;
        }
        std::string user = (*sel)[m_cols.user];
        if (user.empty()) {
            m_log_warn("Queue Manager: Selected job has no user.");
            return;
        }

        if (!confirm_action("Confirm Cancel",
                            "Cancel ALL jobs owned by this user?\n\nUser: " + user)) return;

        m_log_info("Queue Manager: Cancelling all jobs for user " + user + " ...");
        m_cups.cancel_all_from_user(user);
        m_log_ok("Queue Manager: Cancel requested for all jobs by " + user);
        refresh();
    }

    void cancel_all_jobs() {
        if (!confirm_action("Confirm Cancel",
                            "Cancel ALL jobs in the queue?\n\nThis will cancel every pending job.")) return;

        m_log_info("Queue Manager: Cancelling ALL jobs in queue ...");
        m_cups.cancel_all();
        m_log_ok("Queue Manager: Cancel requested for ALL jobs.");
        refresh();
    }

    void pause_queue() {
        if (!confirm_action("Confirm Pause",
                            "Pause/disable the printer queue?\n\nThis may require sudo privileges.")) return;

        m_log_info("Queue Manager: Pausing queue (cupsdisable) ...");
        m_cups.pause_queue();
        m_log_ok("Queue Manager: Pause requested.");
        refresh();
    }

    void resume_queue() {
        if (!confirm_action("Confirm Resume",
                            "Resume/enable the printer queue?\n\nThis may require sudo privileges.")) return;

        m_log_info("Queue Manager: Resuming queue (cupsenable) ...");
        m_cups.resume_queue();
        m_log_ok("Queue Manager: Resume requested.");
        refresh();
    }
};

// ============================================================
// Main Diagnostic Window
// ============================================================
class PrinterDiagnostic : public Gtk::Window {
public:
    PrinterDiagnostic();
    ~PrinterDiagnostic() override = default;

private:
    // Layout
    Gtk::Box m_vbox{Gtk::ORIENTATION_VERTICAL};
    Gtk::Box m_topbar{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Box m_wakebar{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Box m_hbox{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Box m_leftbox{Gtk::ORIENTATION_VERTICAL};
    Gtk::ScrolledWindow m_scrolled;
    Gtk::TextView m_textview;
    Glib::RefPtr<Gtk::TextBuffer> m_buffer;

    // Output controls
    Gtk::CheckButton m_chk_raw{"Show raw output (no cleanup)"};
    Gtk::CheckButton m_chk_strip_global{"Strip ANSI globally"};
    Gtk::CheckButton m_chk_strip_hplip{"Strip ANSI for HPLIP (hp-info)"};
    Gtk::Button m_btn_export{"Export Output"};

    // Continuous wake controls
    Gtk::CheckButton m_chk_wake_enabled{"Enable Continuous Wake Mode"};
    Gtk::Label m_lbl_wake_interval{"Wake interval (min):"};
    Gtk::SpinButton m_spin_wake_interval;
    Gtk::Label m_lbl_wake_status{"Status: Disabled"};

    // Buttons
    Gtk::Button m_btn_quick_test{"1. Quick Test (ping + port check)"};
    Gtk::Button m_btn_full_diagnostic{"2. Full Diagnostic Scan"};
    Gtk::Button m_btn_cups_status{"3. Check CUPS Status"};
    Gtk::Button m_btn_stuck_jobs{"4. Check for Stuck Jobs"};
    Gtk::Button m_btn_plugin_version{"5. Check Plugin Version"};
    Gtk::Button m_btn_printer_info{"6. Get Printer Info (HPLIP)"};

    Gtk::Button m_btn_clear_jobs{"7. Clear Stuck Jobs"};
    Gtk::Button m_btn_wake_command{"8. Send Wake Command to Printer"};
    Gtk::Button m_btn_restart_cups{"9. Restart CUPS Service"};
    Gtk::Button m_btn_test_page{"10. Print Test Page"};

    Gtk::Button m_btn_view_logs{"11. View Recent CUPS Logs"};
    Gtk::Button m_btn_queue_manager{"12. Manage Print Queue"};
    Gtk::Button m_btn_exit{"0. Exit"};

    // Tags
    Glib::RefPtr<Gtk::TextTag> m_tag_red;
    Glib::RefPtr<Gtk::TextTag> m_tag_green;
    Glib::RefPtr<Gtk::TextTag> m_tag_yellow;
    Glib::RefPtr<Gtk::TextTag> m_tag_blue;
    Glib::RefPtr<Gtk::TextTag> m_tag_white;
    Glib::RefPtr<Gtk::TextTag> m_tag_bold;
    Glib::RefPtr<Gtk::TextTag> m_tag_bold_cyan;

    // State
    bool m_show_raw = false;
    bool m_strip_global = false;
    bool m_strip_hplip = true;
    bool m_wake_enabled = false;
    int m_wake_interval_minutes = 5;
    sigc::connection m_wake_timer_conn;

    // CUPS client
    std::unique_ptr<CupsClient> m_cups;

    // Config
    void load_config();
    void save_config();

    // Output helpers
    void print_header(const std::string& text);
    void scroll_to_end();
    void print_success(const std::string& text);
    void print_error(const std::string& text);
    void print_warning(const std::string& text);
    void print_info(const std::string& text);

    // Command runner with per-command ANSI policy
    std::string execute_command(const std::string& cmd, bool is_hplip=false);

    // Diagnostics
    bool check_ping();
    bool check_port_9100();
    bool check_cups_status();
    bool check_stuck_jobs();
    bool check_plugin_version();
    bool get_printer_info();

    // Fixes
    void clear_stuck_jobs();
    void send_wake_command();
    void restart_cups();
    void print_test_page();

    // Other
    void view_cups_logs();
    void open_queue_manager();
    void export_output();

    // Continuous wake
    void start_wake_timer();
    void stop_wake_timer();
    void send_wake_silent();
    void update_wake_status();

    // Button handlers
    void on_quick_test();
    void on_full_diagnostic();
    void on_cups_status();
    void on_stuck_jobs();
    void on_plugin_version();
    void on_printer_info();
    void on_clear_jobs();
    void on_wake_command();
    void on_restart_cups();
    void on_test_page();
    void on_view_logs();
    void on_queue_manager();
    void on_export();
    void on_exit();
};

PrinterDiagnostic::PrinterDiagnostic() {
    set_title("HP P1102w Printer Diagnostic Tool - Complete Edition");
    set_default_size(1000, 720);

    // Buffer + view
    m_buffer = Gtk::TextBuffer::create();
    m_textview.set_buffer(m_buffer);
    m_textview.set_editable(false);
    m_textview.set_cursor_visible(false);

    // Tags
    m_tag_red = Gtk::TextTag::create(); m_tag_red->property_foreground() = "red"; m_buffer->get_tag_table()->add(m_tag_red);
    m_tag_green = Gtk::TextTag::create(); m_tag_green->property_foreground() = "green"; m_buffer->get_tag_table()->add(m_tag_green);
    m_tag_yellow = Gtk::TextTag::create(); m_tag_yellow->property_foreground() = "yellow"; m_buffer->get_tag_table()->add(m_tag_yellow);
    m_tag_blue = Gtk::TextTag::create(); m_tag_blue->property_foreground() = "blue"; m_buffer->get_tag_table()->add(m_tag_blue);
    m_tag_white = Gtk::TextTag::create(); m_tag_white->property_foreground() = "white"; m_buffer->get_tag_table()->add(m_tag_white);
    m_tag_bold = Gtk::TextTag::create(); m_tag_bold->property_weight() = Pango::WEIGHT_BOLD; m_buffer->get_tag_table()->add(m_tag_bold);
    m_tag_bold_cyan = Gtk::TextTag::create(); m_tag_bold_cyan->property_foreground() = "cyan"; m_tag_bold_cyan->property_weight() = Pango::WEIGHT_BOLD; m_buffer->get_tag_table()->add(m_tag_bold_cyan);

    // Load saved preferences
    load_config();

    // Layout
    add(m_vbox);
    m_vbox.pack_start(m_topbar, false, false, 6);
    m_vbox.pack_start(m_wakebar, false, false, 6);
    m_vbox.pack_start(m_hbox, true, true, 0);

    // Top bar - Output controls
    m_topbar.set_spacing(10);
    m_topbar.set_border_width(6);

    m_chk_raw.set_active(m_show_raw);
    m_chk_strip_global.set_active(m_strip_global);
    m_chk_strip_hplip.set_active(m_strip_hplip);

    m_topbar.pack_start(m_chk_raw, false, false, 0);
    m_topbar.pack_start(m_chk_strip_global, false, false, 0);
    m_topbar.pack_start(m_chk_strip_hplip, false, false, 0);
    m_topbar.pack_end(m_btn_export, false, false, 0);

    // Wake bar - Continuous wake controls
    m_wakebar.set_spacing(10);
    m_wakebar.set_border_width(6);

    m_chk_wake_enabled.set_active(m_wake_enabled);
    m_spin_wake_interval.set_range(1, 60);
    m_spin_wake_interval.set_increments(1, 5);
    m_spin_wake_interval.set_value(m_wake_interval_minutes);
    m_lbl_wake_status.set_xalign(0.0f);

    m_wakebar.pack_start(m_chk_wake_enabled, false, false, 0);
    m_wakebar.pack_start(m_lbl_wake_interval, false, false, 0);
    m_wakebar.pack_start(m_spin_wake_interval, false, false, 0);
    m_wakebar.pack_start(m_lbl_wake_status, true, true, 0);

    // Settings info
    Gtk::Box* settings_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    settings_box->set_spacing(2);

    Gtk::Label* lbl_settings = Gtk::manage(new Gtk::Label("Output Settings:"));
    lbl_settings->set_xalign(0.0f);

    Gtk::Label* lbl_raw = Gtk::manage(new Gtk::Label(
        "• Raw output: show exact command output (includes ANSI color codes)."));
    lbl_raw->set_xalign(0.0f);

    Gtk::Label* lbl_global = Gtk::manage(new Gtk::Label(
        "• Strip ANSI globally: remove terminal color codes from all commands."));
    lbl_global->set_xalign(0.0f);

    Gtk::Label* lbl_hplip = Gtk::manage(new Gtk::Label(
        "• Strip ANSI for HPLIP only: clean hp-info output while leaving others untouched."));
    lbl_hplip->set_xalign(0.0f);

    Gtk::Label* lbl_wake = Gtk::manage(new Gtk::Label(
        "• Continuous Wake: automatically send wake commands at regular intervals to prevent deep sleep."));
    lbl_wake->set_xalign(0.0f);

    settings_box->pack_start(*lbl_settings, false, false, 0);
    settings_box->pack_start(*lbl_raw, false, false, 0);
    settings_box->pack_start(*lbl_global, false, false, 0);
    settings_box->pack_start(*lbl_hplip, false, false, 0);
    settings_box->pack_start(*lbl_wake, false, false, 0);

    m_vbox.pack_start(*settings_box, false, false, 6);

    auto update_toggle_sensitivity = [this]() {
        bool raw = m_chk_raw.get_active();
        m_chk_strip_global.set_sensitive(!raw);
        m_chk_strip_hplip.set_sensitive(!raw && !m_chk_strip_global.get_active());
        
        bool wake_on = m_chk_wake_enabled.get_active();
        m_spin_wake_interval.set_sensitive(wake_on);
    };
    update_toggle_sensitivity();

    m_chk_raw.signal_toggled().connect([this, update_toggle_sensitivity]() {
        m_show_raw = m_chk_raw.get_active();
        update_toggle_sensitivity();
        save_config();
    });

    m_chk_strip_global.signal_toggled().connect([this, update_toggle_sensitivity]() {
        m_strip_global = m_chk_strip_global.get_active();
        update_toggle_sensitivity();
        save_config();
    });

    m_chk_strip_hplip.signal_toggled().connect([this, update_toggle_sensitivity]() {
        m_strip_hplip = m_chk_strip_hplip.get_active();
        update_toggle_sensitivity();
        save_config();
    });

    m_chk_wake_enabled.signal_toggled().connect([this, update_toggle_sensitivity]() {
        m_wake_enabled = m_chk_wake_enabled.get_active();
        if (m_wake_enabled) {
            start_wake_timer();
        } else {
            stop_wake_timer();
        }
        update_toggle_sensitivity();
        save_config();
    });

    m_spin_wake_interval.signal_value_changed().connect([this]() {
        m_wake_interval_minutes = (int)m_spin_wake_interval.get_value();
        if (m_wake_enabled) {
            start_wake_timer();  // Restart with new interval
        }
        save_config();
    });

    m_btn_export.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_export));

    // Left panel
    m_leftbox.set_border_width(10);
    m_leftbox.set_spacing(4);
    m_hbox.pack_start(m_leftbox, false, false, 0);

    // CUPS client for friendly name
    m_cups = std::make_unique<CupsClient>([this](const std::string& cmd) {
        return this->execute_command(cmd, false);
    });

    std::string friendly_name = m_cups->get_printer_friendly_name();

    Gtk::Label lbl_printer("Printer: " + friendly_name); lbl_printer.set_xalign(0.0f);
    Gtk::Label lbl_queue("CUPS Queue: " + PRINTER_NAME); lbl_queue.set_xalign(0.0f);
    Gtk::Label lbl_ip("IP Address: " + PRINTER_IP); lbl_ip.set_xalign(0.0f);
    Gtk::Label lbl_port("Port: " + std::to_string(PRINTER_PORT)); lbl_port.set_xalign(0.0f);

    m_leftbox.pack_start(lbl_printer, false, false, 0);
    m_leftbox.pack_start(lbl_queue, false, false, 0);
    m_leftbox.pack_start(lbl_ip, false, false, 0);
    m_leftbox.pack_start(lbl_port, false, false, 0);

    Gtk::Label lbl_diagnostics("\nDIAGNOSTICS:"); lbl_diagnostics.set_xalign(0.0f);
    m_leftbox.pack_start(lbl_diagnostics, false, false, 0);

    m_leftbox.pack_start(m_btn_quick_test, false, false, 0);
    m_leftbox.pack_start(m_btn_full_diagnostic, false, false, 0);
    m_leftbox.pack_start(m_btn_cups_status, false, false, 0);
    m_leftbox.pack_start(m_btn_stuck_jobs, false, false, 0);
    m_leftbox.pack_start(m_btn_plugin_version, false, false, 0);
    m_leftbox.pack_start(m_btn_printer_info, false, false, 0);

    Gtk::Label lbl_fixes("\nFIXES:"); lbl_fixes.set_xalign(0.0f);
    m_leftbox.pack_start(lbl_fixes, false, false, 0);

    m_leftbox.pack_start(m_btn_clear_jobs, false, false, 0);
    m_leftbox.pack_start(m_btn_wake_command, false, false, 0);
    m_leftbox.pack_start(m_btn_restart_cups, false, false, 0);
    m_leftbox.pack_start(m_btn_test_page, false, false, 0);

    Gtk::Label lbl_other("\nOTHER:"); lbl_other.set_xalign(0.0f);
    m_leftbox.pack_start(lbl_other, false, false, 0);

    m_leftbox.pack_start(m_btn_view_logs, false, false, 0);
    m_leftbox.pack_start(m_btn_queue_manager, false, false, 0);
    m_leftbox.pack_start(m_btn_exit, false, false, 0);

    // Connect signals
    m_btn_quick_test.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_quick_test));
    m_btn_full_diagnostic.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_full_diagnostic));
    m_btn_cups_status.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_cups_status));
    m_btn_stuck_jobs.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_stuck_jobs));
    m_btn_plugin_version.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_plugin_version));
    m_btn_printer_info.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_printer_info));
    m_btn_clear_jobs.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_clear_jobs));
    m_btn_wake_command.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_wake_command));
    m_btn_restart_cups.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_restart_cups));
    m_btn_test_page.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_test_page));
    m_btn_view_logs.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_view_logs));
    m_btn_queue_manager.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_queue_manager));
    m_btn_exit.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_exit));

    // Right output
    m_scrolled.add(m_textview);
    m_scrolled.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_hbox.pack_start(m_scrolled, true, true, 0);

    // Persist on window hide
    signal_hide().connect([this]() { 
        stop_wake_timer();
        save_config(); 
    });

    // Start wake timer if enabled
    if (m_wake_enabled) {
        start_wake_timer();
    }

    show_all_children();
}

// ============================================================
// Config persistence
// ============================================================
void PrinterDiagnostic::load_config() {
    ensure_config_dir_exists();

    Glib::KeyFile kf;
    try {
        kf.load_from_file(config_file_path());
        m_show_raw = kf.get_boolean("output", "raw");
        m_strip_global = kf.get_boolean("output", "strip_global");
        m_strip_hplip = kf.get_boolean("output", "strip_hplip");
        m_wake_enabled = kf.get_boolean("wake", "enabled");
        m_wake_interval_minutes = kf.get_integer("wake", "interval_minutes");
    } catch (...) {
        // Keep defaults
    }
}

void PrinterDiagnostic::save_config() {
    ensure_config_dir_exists();
    Glib::KeyFile kf;
    try {
        kf.set_boolean("output", "raw", m_show_raw);
        kf.set_boolean("output", "strip_global", m_strip_global);
        kf.set_boolean("output", "strip_hplip", m_strip_hplip);
        kf.set_boolean("wake", "enabled", m_wake_enabled);
        kf.set_integer("wake", "interval_minutes", m_wake_interval_minutes);

        std::string data = kf.to_data();
        std::ofstream out(config_file_path(), std::ios::binary);
        if (out) out << data;
    } catch (...) {
        // Ignore
    }
}

// ============================================================
// Continuous wake functions
// ============================================================
void PrinterDiagnostic::start_wake_timer() {
    stop_wake_timer();  // Clear existing timer
    
    if (m_wake_interval_minutes <= 0) return;
    
    // Convert minutes to seconds for the timer
    int interval_seconds = m_wake_interval_minutes * 60;
    
    m_wake_timer_conn = Glib::signal_timeout().connect_seconds([this]() -> bool {
        send_wake_silent();
        return true;  // Keep running
    }, interval_seconds);
    
    update_wake_status();
}

void PrinterDiagnostic::stop_wake_timer() {
    if (m_wake_timer_conn.connected()) {
        m_wake_timer_conn.disconnect();
    }
    update_wake_status();
}

void PrinterDiagnostic::send_wake_silent() {
    // Send wake command without logging to output
    std::string cmd =
        "printf '\\x1B%%-12345X@PJL\\r\\n@PJL INFO STATUS\\r\\n\\x1B%%-12345X\\r\\n' | "
        "nc " + PRINTER_IP + " " + std::to_string(PRINTER_PORT) + " -w 3 2>/dev/null";
    execute_command(cmd, false);
    
    update_wake_status();
}

void PrinterDiagnostic::update_wake_status() {
    if (m_wake_enabled && m_wake_timer_conn.connected()) {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm);
        
        m_lbl_wake_status.set_markup(
            "<span foreground='green'>Status: <b>ACTIVE</b></span>  |  "
            "Interval: " + std::to_string(m_wake_interval_minutes) + " min  |  "
            "Last wake: " + std::string(time_str));
    } else {
        m_lbl_wake_status.set_markup("<span foreground='red'>Status: <b>DISABLED</b></span>");
    }
}

// ============================================================
// Output helpers
// ============================================================
void PrinterDiagnostic::print_header(const std::string& text) {
    auto iter = m_buffer->end();
    iter = m_buffer->insert_with_tag(iter, "\n=======================================\n", m_tag_bold_cyan);
    iter = m_buffer->insert_with_tag(iter, text + "\n", m_tag_bold_cyan);
    iter = m_buffer->insert_with_tag(iter, "=======================================\n\n", m_tag_bold_cyan);
    scroll_to_end();
}

void PrinterDiagnostic::print_success(const std::string& text) {
    m_buffer->insert_with_tag(m_buffer->end(), "✓ " + text + "\n", m_tag_green);
    scroll_to_end();
}
void PrinterDiagnostic::print_error(const std::string& text) {
    m_buffer->insert_with_tag(m_buffer->end(), "✗ " + text + "\n", m_tag_red);
    scroll_to_end();
}
void PrinterDiagnostic::print_warning(const std::string& text) {
    m_buffer->insert_with_tag(m_buffer->end(), "⚠ " + text + "\n", m_tag_yellow);
    scroll_to_end();
}
void PrinterDiagnostic::print_info(const std::string& text) {
    m_buffer->insert_with_tag(m_buffer->end(), "ℹ " + text + "\n", m_tag_blue);
    scroll_to_end();
}

void PrinterDiagnostic::scroll_to_end() {
    auto iter = m_buffer->end();
    m_textview.scroll_to(iter);
}

// ============================================================
// Command runner
// ============================================================
std::string PrinterDiagnostic::execute_command(const std::string& cmd, bool is_hplip) {
    std::array<char, 256> buffer{};
    std::string result;

    auto deleter = [](FILE* f) { if (f) pclose(f); };
    std::unique_ptr<FILE, decltype(deleter)> pipe(popen(cmd.c_str(), "r"), deleter);
    if (!pipe) return "";

    while (fgets(buffer.data(), (int)buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }

    if (m_show_raw) return result;
    if (m_strip_global) return strip_ansi(result);
    if (is_hplip && m_strip_hplip) return strip_ansi(result);

    return result;
}

// ============================================================
// Diagnostics
// ============================================================
bool PrinterDiagnostic::check_ping() {
    print_info("Testing network connectivity (ping)...");
    std::string cmd = "ping -c 3 -W 2 " + PRINTER_IP + " 2>&1";
    std::string result = execute_command(cmd, false);

    if (result.find("0% packet loss") != std::string::npos || result.find("3 received") != std::string::npos) {
        print_success("Printer responds to ping - Network OK");
        return true;
    }
    print_error("Printer does not respond to ping - Network issue");
    print_warning("Check: Printer power, WiFi connection, router/bridge path");
    return false;
}

bool PrinterDiagnostic::check_port_9100() {
    print_info("Testing JetDirect port 9100...");

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        print_error("Socket error creating test socket.");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PRINTER_PORT);
    addr.sin_addr.s_addr = inet_addr(PRINTER_IP.c_str());

    fcntl(sock, F_SETFL, O_NONBLOCK);

    int res = connect(sock, (sockaddr*)&addr, sizeof(addr));
    if (res == 0) {
        ::close(sock);
        print_success("Port 9100 is OPEN - Printer ready to receive jobs");
        return true;
    }

    if (errno != EINPROGRESS) {
        ::close(sock);
        print_error("Port 9100 ERROR: " + std::string(strerror(errno)));
        return false;
    }

    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(sock, &writeSet);

    timeval tv{};
    tv.tv_sec = 3;
    tv.tv_usec = 0;

    res = select(sock + 1, nullptr, &writeSet, nullptr, &tv);
    if (res <= 0) {
        ::close(sock);
        if (res == 0) print_error("Port 9100 TIMEOUT - Printer not responding");
        else print_error("Port 9100 ERROR: " + std::string(strerror(errno)));
        print_warning("Solution: Power cycle the printer (deep sleep / network stack)");
        return false;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
    ::close(sock);

    if (so_error == 0) {
        print_success("Port 9100 is OPEN - Printer ready to receive jobs");
        return true;
    }
    if (so_error == ECONNREFUSED) {
        print_error("Port 9100 REFUSED - Printer is in deep sleep");
        print_warning("Solution: Press printer power button once to wake (or use option 8)");
        return false;
    }

    print_error("Port 9100 ERROR: " + std::string(strerror(so_error)));
    return false;
}

bool PrinterDiagnostic::check_cups_status() {
    print_info("Checking CUPS printer queue...");
    std::string result = m_cups->printer_state_raw();

    if (result.find("idle") != std::string::npos) {
        print_success("CUPS queue is idle and ready");
        return true;
    }
    if (result.find("disabled") != std::string::npos) {
        print_error("CUPS queue is DISABLED");
        print_warning("Run: sudo cupsenable \"" + PRINTER_NAME + "\"");

        // Auto-recovery assessment (from dev version)
        try {
            const auto jobs = m_cups->get_jobs();
            const bool queue_empty = jobs.empty();

            std::string matched_reason;
            const bool recoverable_hint = m_cups->has_recoverable_reason_hint(&matched_reason);

            if (queue_empty && recoverable_hint) {
                print_success("Auto-Recovery eligible: queue is empty and reason looks recoverable (" + matched_reason + ").");
                print_info("Would run: cupsenable + cupsaccept for this queue (not auto-executed).");
            } else {
                if (!queue_empty) {
                    print_warning("Auto-Recovery skipped: queue is not empty (active/pending jobs present).");
                } else {
                    print_warning("Auto-Recovery skipped: reason not recognized as safely recoverable.");
                    print_info("Tip: If this is truly stale (e.g., you added paper), manually re-enable via CUPS.");
                }
            }
        } catch (const std::exception& e) {
            print_warning(std::string("Auto-Recovery assessment failed: ") + e.what());
        }

        return false;
    }

    print_warning("Unknown CUPS status");
    m_buffer->insert(m_buffer->end(), result + "\n");
    return false;
}

bool PrinterDiagnostic::check_stuck_jobs() {
    print_info("Checking for stuck print jobs...");
    std::string result = execute_command("lpstat -o 2>&1", false);

    if (trim_copy(result).empty()) {
        print_success("No stuck jobs in queue");
        return true;
    }

    print_warning("Found jobs in queue:");
    m_buffer->insert(m_buffer->end(), result + "\n");
    return false;
}

bool PrinterDiagnostic::check_plugin_version() {
    print_info("Checking HPLIP plugin version...");
    std::string cmd = "sudo journalctl -u cups --since '5 minutes ago' 2>&1 | grep -i 'plugin.*mismatch'";
    std::string result = execute_command(cmd, false);

    if (trim_copy(result).empty()) {
        print_success("No plugin version errors detected");
        return true;
    }

    print_error("Plugin version mismatch detected!");
    print_warning("Run: yay -S hplip-plugin --rebuild");
    print_warning("Then: sudo systemctl restart cups");
    return false;
}

bool PrinterDiagnostic::get_printer_info() {
    print_info("Getting detailed printer information...");
    std::string cmd = "hp-info -d hp:/net/" + PRINTER_NAME + "?ip=" + PRINTER_IP + " 2>&1";
    std::string result = execute_command(cmd, true);

    if (result.find("Communication status: Good") != std::string::npos || 
        result.find("Device") != std::string::npos) {
        print_success("HPLIP can communicate with printer");
        m_buffer->insert_with_tag(m_buffer->end(), "\n" + result + "\n", m_tag_white);
        return true;
    }

    print_warning("HPLIP returned output (see below)");
    m_buffer->insert_with_tag(m_buffer->end(), "\n" + result + "\n", m_tag_white);
    return false;
}

// ============================================================
// Fix actions
// ============================================================
void PrinterDiagnostic::clear_stuck_jobs() {
    print_info("Clearing all stuck jobs...");
    execute_command("cancel -a 2>&1", false);
    print_success("All jobs cancelled");
}

void PrinterDiagnostic::send_wake_command() {
    print_info("Sending wake command to printer...");
    std::string cmd =
        "printf '\\x1B%%-12345X@PJL\\r\\n@PJL INFO STATUS\\r\\n\\x1B%%-12345X\\r\\n' | "
        "nc " + PRINTER_IP + " " + std::to_string(PRINTER_PORT) + " -w 3 2>/dev/null";
    execute_command(cmd, false);
    sleep(2);
    print_success("Wake command sent - wait 5 seconds then test again");
    
    if (m_wake_enabled) {
        update_wake_status();  // Update status with new timestamp
    }
}

void PrinterDiagnostic::restart_cups() {
    print_info("Restarting CUPS service...");
    execute_command("sudo systemctl restart cups 2>&1", false);
    print_success("CUPS restarted");
}

void PrinterDiagnostic::print_test_page() {
    print_info("Sending test page to printer...");
    std::string ts = now_timestamp_yyyymmdd_hhmmss();
    std::string cmd = "echo \"Diagnostic Test Page - " + ts + "\" | lpr 2>&1";
    execute_command(cmd, false);
    print_success("Test page sent - check printer");
}

// ============================================================
// Other actions
// ============================================================
void PrinterDiagnostic::view_cups_logs() {
    print_header("Recent CUPS Logs (last 50 lines)");
    std::string cmd = "sudo journalctl -u cups -n 50 --no-pager 2>&1";
    std::string result = execute_command(cmd, false);
    m_buffer->insert_with_tag(m_buffer->end(), result + "\n", m_tag_white);
}

void PrinterDiagnostic::open_queue_manager() {
    print_info("Opening print queue manager...");

    QueueDialog dlg(
        *this,
        *m_cups,
        [this](const std::string& s) { this->print_info(s); },
        [this](const std::string& s) { this->print_success(s); },
        [this](const std::string& s) { this->print_warning(s); },
        [this](const std::string& s) { this->print_error(s); }
    );
    dlg.run();
}

void PrinterDiagnostic::export_output() {
    Gtk::FileChooserDialog dlg(*this, "Export Diagnostic Output", Gtk::FILE_CHOOSER_ACTION_SAVE);
    dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dlg.add_button("_Save", Gtk::RESPONSE_OK);

    dlg.set_current_name("printer_diagnostic_" + now_timestamp_yyyymmdd_hhmmss() + ".txt");

    if (dlg.run() == Gtk::RESPONSE_OK) {
        std::ofstream out(dlg.get_filename(), std::ios::binary);
        if (!out) {
            print_error("Failed to write export file.");
            return;
        }
        out << m_buffer->get_text();
        print_success("Exported output to: " + dlg.get_filename());
    }
}

// ============================================================
// Button handlers
// ============================================================
void PrinterDiagnostic::on_quick_test() {
    m_buffer->set_text("");
    print_header("Quick Diagnostic Test");

    bool ping_ok = check_ping();
    bool port_ok = check_port_9100();

    m_buffer->insert_with_tag(m_buffer->end(), "\nSUMMARY:\n", m_tag_bold);

    if (ping_ok && port_ok) {
        print_success("Printer is fully operational!");
        print_info("You can try printing now");
    } else if (ping_ok && !port_ok) {
        print_warning("Printer is online but print service is asleep");
        print_info("Recommended action: Press printer power button or use option 8");
        print_info("Or enable Continuous Wake Mode to prevent future sleep");
    } else {
        print_error("Printer is not reachable on network");
        print_info("Check: Printer power, WiFi status, router/bridge path");
    }
}

void PrinterDiagnostic::on_full_diagnostic() {
    m_buffer->set_text("");
    print_header("Full Diagnostic Scan");

    bool ping = check_ping();
    m_buffer->insert(m_buffer->end(), "\n");

    bool port = check_port_9100();
    m_buffer->insert(m_buffer->end(), "\n");

    bool cups = check_cups_status();
    m_buffer->insert(m_buffer->end(), "\n");

    bool jobs = check_stuck_jobs();
    m_buffer->insert(m_buffer->end(), "\n");

    bool plugin = check_plugin_version();
    m_buffer->insert(m_buffer->end(), "\n");

    print_header("Diagnostic Summary");
    bool all_ok = ping && port && cups && jobs && plugin;

    if (all_ok) {
        print_success("All diagnostics passed! Printer should be working.");
        if (!m_wake_enabled) {
            print_info("Tip: Enable Continuous Wake Mode to prevent printer from sleeping");
        }
    } else {
        print_warning("Some issues detected. Review the results above.");
        print_info("Use the FIX menu options to resolve issues");
    }
}

void PrinterDiagnostic::on_cups_status() {
    m_buffer->set_text("");
    print_header("CUPS Status Check");
    check_cups_status();
}

void PrinterDiagnostic::on_stuck_jobs() {
    m_buffer->set_text("");
    print_header("Stuck Jobs Check");
    check_stuck_jobs();
}

void PrinterDiagnostic::on_plugin_version() {
    m_buffer->set_text("");
    print_header("Plugin Version Check");
    check_plugin_version();
}

void PrinterDiagnostic::on_printer_info() {
    m_buffer->set_text("");
    print_header("Printer Info (HPLIP)");
    get_printer_info();
}

void PrinterDiagnostic::on_clear_jobs() {
    m_buffer->set_text("");
    print_header("Clear Stuck Jobs");
    clear_stuck_jobs();
}

void PrinterDiagnostic::on_wake_command() {
    m_buffer->set_text("");
    print_header("Send Wake Command");
    send_wake_command();
}

void PrinterDiagnostic::on_restart_cups() {
    m_buffer->set_text("");
    print_header("Restart CUPS");
    restart_cups();
}

void PrinterDiagnostic::on_test_page() {
    m_buffer->set_text("");
    print_header("Print Test Page");
    print_test_page();
}

void PrinterDiagnostic::on_view_logs() {
    m_buffer->set_text("");
    view_cups_logs();
}

void PrinterDiagnostic::on_queue_manager() {
    m_buffer->set_text("");
    print_header("Manage Print Queue");
    open_queue_manager();
}

void PrinterDiagnostic::on_export() {
    export_output();
}

void PrinterDiagnostic::on_exit() {
    stop_wake_timer();
    save_config();
    hide();
}

// ============================================================
// main
// ============================================================
int main(int argc, char** argv) {
    auto app = Gtk::Application::create(argc, argv, "org.hp.p1102w.printer_diagnostic");
    PrinterDiagnostic window;
    return app->run(window);
}

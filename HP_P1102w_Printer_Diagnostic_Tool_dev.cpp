#include <gtkmm.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ============================================================
// Configuration
// ============================================================
const std::string PRINTER_IP   = "192.168.4.68";
const int         PRINTER_PORT = 9100;
const std::string PRINTER_NAME = "HP_LaserJet_Professional_P1102w";

// ============================================================
// Data model
// ============================================================
struct PrintJob {
    std::string job_id;      // e.g., "HP_LaserJet_Professional_P1102w-123"
    std::string user;        // e.g., "matt"
    std::string file;        // best-effort display string
    std::string status;      // best-effort display string
    std::optional<std::chrono::system_clock::time_point> submitted_at;
};

// ============================================================
// Small helper: trim
// ============================================================
static inline std::string trim_copy(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c); };
    while (!s.empty() && is_space((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && is_space((unsigned char)s.back())) s.pop_back();
    return s;
}

// ============================================================
// CupsClient abstraction (best-effort using lpstat/cancel)
// ============================================================
class CupsClient {
public:
    explicit CupsClient(std::function<std::string(const std::string&)> exec)
        : m_exec(std::move(exec)) {}

    // Queue state for PRINTER_NAME
    std::string get_printer_state_raw() {
        // Example outputs:
        //   printer HP... is idle. enabled since ...
        //   printer HP... disabled since ...
        return m_exec("lpstat -p \"" + PRINTER_NAME + "\" 2>&1");
    }


    // Long printer details (best effort). Useful for Description/Location and state hints.
    std::string get_printer_long_raw() {
        return m_exec("lpstat -l -p \"" + PRINTER_NAME + "\" 2>&1");
    }

    // Friendly label for UI (best effort). Falls back to PRINTER_NAME if unavailable.
    std::string get_printer_friendly_name() {
        const std::string out = get_printer_long_raw();

        // lpstat -l -p typically includes: "Description: <text>"
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) {
            auto pos = line.find("Description:");
            if (pos != std::string::npos) {
                std::string desc = line.substr(pos + std::string("Description:").size());
                // trim
                auto l = desc.find_first_not_of(" \t");
                auto r = desc.find_last_not_of(" \t\r\n");
                if (l != std::string::npos && r != std::string::npos) desc = desc.substr(l, r - l + 1);
                if (!desc.empty()) return desc;
            }
        }
        return PRINTER_NAME;
    }

    // Best-effort check for common recoverable reasons in lpstat output.
    bool has_recoverable_reason_hint(std::string* matched_reason = nullptr) {
        std::string out = get_printer_long_raw();
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return (char)std::tolower(c); });

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
    bool is_queue_disabled() {
        auto out = get_printer_state_raw();
        return out.find("disabled") != std::string::npos;
    }

    // Get all pending/not-completed jobs (best effort)
    std::vector<PrintJob> get_jobs() {
        // Use -W not-completed when available; fallback gracefully.
        std::string out = m_exec("lpstat -W not-completed -o -l 2>&1");
        if (out.find("Unknown option") != std::string::npos ||
            out.find("invalid option") != std::string::npos) {
            out = m_exec("lpstat -o -l 2>&1");
        }
        return parse_lpstat_jobs(out);
    }

    void cancel_job(const std::string& job_id) {
        // cancel supports job-id or printer-jobid
        m_exec("cancel " + shell_escape(job_id) + " 2>&1");
    }

    void cancel_all() {
        m_exec("cancel -a 2>&1");
    }

    void cancel_all_from_user(const std::string& user) {
        // Use cupsenable/cupsdisable? No. We iterate jobs and cancel matches.
        auto jobs = get_jobs();
        for (const auto& j : jobs) {
            if (j.user == user) cancel_job(j.job_id);
        }
    }

    void pause_queue() {
        // Requires admin on many setups. Keep consistent with the rest of the tool.
        m_exec("sudo cupsdisable \"" + PRINTER_NAME + "\" 2>&1");
    }

    void resume_queue() {
        m_exec("sudo cupsenable \"" + PRINTER_NAME + "\" 2>&1");
    }

private:
    std::function<std::string(const std::string&)> m_exec;

    static std::string shell_escape(const std::string& s) {
        // Minimal shell escaping for job-id (no spaces expected, but still safe)
        std::string out = "'";
        for (char c : s) {
            if (c == '\'') out += "'\"'\"'";
            else out += c;
        }
        out += "'";
        return out;
    }

    // Best-effort parsing.
    // Typical lpstat -o -l (varies by distro/CUPS):
    //   printer-123  user  1024  Tue 20 Dec 2025 10:11:12 AM EST
    //          filename
    //
    // Or single-line variants. We support both.
    static std::vector<PrintJob> parse_lpstat_jobs(const std::string& text) {
        std::vector<PrintJob> jobs;

        std::istringstream ss(text);
        std::string line;

        PrintJob current;
        bool have_current = false;

        auto flush_current = [&]() {
            if (have_current && !current.job_id.empty()) jobs.push_back(current);
            current = PrintJob{};
            have_current = false;
        };

        while (std::getline(ss, line)) {
            if (trim_copy(line).empty()) {
                flush_current();
                continue;
            }

            // Continuation lines are typically indented and contain filename/status text.
            bool is_continuation = !line.empty() && std::isspace((unsigned char)line[0]);

            if (!is_continuation) {
                // New job header line
                flush_current();
                have_current = true;

                std::istringstream ls(line);
                ls >> current.job_id >> current.user;

                // Remaining text (size/date/status/etc.)
                std::string rest;
                std::getline(ls, rest);
                rest = trim_copy(rest);
                current.status = rest;

                // Try to parse submitted date/time from the tail of the line
                current.submitted_at = parse_datetime_from_line(rest);
            } else if (have_current) {
                // Filename/status continuation
                std::string cont = trim_copy(line);

                // If multiple continuation lines exist, append with separator.
                if (!cont.empty()) {
                    if (!current.file.empty()) current.file += " | ";
                    current.file += cont;
                }
            }
        }

        flush_current();
        return jobs;
    }

    static std::optional<std::chrono::system_clock::time_point> parse_datetime_from_line(const std::string& rest) {
        // Best effort: try several common lpstat formats.
        // Example candidates:
        //   "1024 Tue 20 Dec 2025 10:11:12 AM EST"
        //   "Tue 20 Dec 2025 10:11:12"
        //   "20 Dec 2025 10:11:12"
        //
        // We'll search for a substring that looks like: "20 Dec 2025 10:11:12"
        // and then try parse with get_time.

        // Find pattern: d{1,2} <Mon> d{4} d{2}:d{2}:d{2}
        // NOTE: This is English-month dependent; if your locale differs, parsing may fail (age will be "unknown").
        static const std::vector<std::string> months = {
            "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
        };

        // naive scan: locate a month token, then pull surrounding tokens
        std::istringstream iss(rest);
        std::vector<std::string> tokens;
        for (std::string t; iss >> t;) tokens.push_back(t);

        auto is_month = [&](const std::string& t) {
            for (auto& m : months) if (t == m) return true;
            return false;
        };

        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!is_month(tokens[i])) continue;

            // Expect: <day> <Mon> <year> <time>
            // So month at i => day at i-1, year at i+1, time at i+2
            if (i == 0 || i + 2 >= tokens.size()) continue;

            std::string day  = tokens[i - 1];
            std::string mon  = tokens[i];
            std::string year = tokens[i + 1];
            std::string time = tokens[i + 2];

            // Strip commas if any
            if (!day.empty() && day.back() == ',') day.pop_back();
            if (!year.empty() && year.back() == ',') year.pop_back();

            // Basic validation
            if (time.find(':') == std::string::npos) continue;

            std::tm tm{};
            std::istringstream dt(day + " " + mon + " " + year + " " + time);
            dt >> std::get_time(&tm, "%d %b %Y %H:%M:%S");
            if (dt.fail()) {
                // Some systems omit seconds: HH:MM
                std::tm tm2{};
                std::istringstream dt2(day + " " + mon + " " + year + " " + time);
                dt2 >> std::get_time(&tm2, "%d %b %Y %H:%M");
                if (dt2.fail()) continue;
                tm = tm2;
            }

            // Try to handle AM/PM token if present immediately after time
            // (If 12h clock, lpstat might output "10:11:12 AM")
            if (i + 3 < tokens.size()) {
                std::string maybe_ampm = tokens[i + 3];
                if (maybe_ampm == "AM" || maybe_ampm == "PM") {
                    // convert tm_hour from 12h to 24h
                    int hour = tm.tm_hour;
                    if (maybe_ampm == "AM") {
                        if (hour == 12) hour = 0;
                    } else { // PM
                        if (hour != 12) hour += 12;
                    }
                    tm.tm_hour = hour;
                }
            }

            tm.tm_isdst = -1; // let mktime decide
            std::time_t tt = std::mktime(&tm);
            if (tt == (std::time_t)-1) continue;

            return std::chrono::system_clock::from_time_t(tt);
        }

        return std::nullopt;
    }
};

// ============================================================
// Queue management dialog (per-job cancel, auto refresh, etc.)
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

        set_default_size(800, 420);

        // Main layout inside dialog
        m_root.set_orientation(Gtk::ORIENTATION_VERTICAL);
        m_root.set_spacing(8);
        m_root.set_border_width(10);
        get_content_area()->pack_start(m_root);

        // Controls row
        m_controls.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
        m_controls.set_spacing(8);

        m_lbl_refresh.set_text("Auto-refresh (seconds):");
        m_spin_refresh.set_range(0, 3600);
        m_spin_refresh.set_increments(1, 10);
        m_spin_refresh.set_value(5);

        m_lbl_age.set_text("Highlight jobs older than (minutes):");
        m_spin_age.set_range(0, 1440);
        m_spin_age.set_increments(1, 5);
        m_spin_age.set_value(10);

        m_btn_refresh.set_label("Refresh Now");
        m_btn_cancel_selected.set_label("Cancel Selected Job");
        m_btn_cancel_user.set_label("Cancel All From Selected User");
        m_btn_pause.set_label("Pause Queue");
        m_btn_resume.set_label("Resume Queue");

        m_controls.pack_start(m_lbl_refresh, false, false, 0);
        m_controls.pack_start(m_spin_refresh, false, false, 0);
        m_controls.pack_start(m_lbl_age, false, false, 0);
        m_controls.pack_start(m_spin_age, false, false, 0);
        m_controls.pack_end(m_btn_refresh, false, false, 0);

        m_root.pack_start(m_controls, false, false, 0);

        // Status line
        m_status.set_xalign(0.0f);
        m_root.pack_start(m_status, false, false, 0);

        // TreeView
        m_store = Gtk::ListStore::create(m_cols);
        m_tree.set_model(m_store);
        m_tree.get_selection()->set_mode(Gtk::SELECTION_SINGLE);
        m_tree.set_headers_clickable(true);

        add_text_column("Job ID", m_cols.job_id, 170);
        add_text_column("User", m_cols.user, 120);
        add_text_column("Age", m_cols.age, 90);
        add_text_column("Status", m_cols.status, 260);
        add_text_column("File", m_cols.file, 300);

        // Highlight rule: background color if older than X minutes (best effort)
        // NOTE: GTKmm 3 uses "cell background" on CellRendererText.
        apply_age_highlight();

        m_scrolled.add(m_tree);
        m_scrolled.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        m_root.pack_start(m_scrolled, true, true, 0);

        // Button row
        m_actions.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
        m_actions.set_spacing(8);
        m_actions.pack_start(m_btn_cancel_selected, false, false, 0);
        m_actions.pack_start(m_btn_cancel_user, false, false, 0);
        m_actions.pack_end(m_btn_resume, false, false, 0);
        m_actions.pack_end(m_btn_pause, false, false, 0);

        m_root.pack_start(m_actions, false, false, 0);

        add_button("Close", Gtk::RESPONSE_CLOSE);

        // Handlers
        m_btn_refresh.signal_clicked().connect(sigc::mem_fun(*this, &QueueDialog::refresh));
        m_btn_cancel_selected.signal_clicked().connect(sigc::mem_fun(*this, &QueueDialog::cancel_selected));
        m_btn_cancel_user.signal_clicked().connect(sigc::mem_fun(*this, &QueueDialog::cancel_all_from_user));
        m_btn_pause.signal_clicked().connect(sigc::mem_fun(*this, &QueueDialog::pause_queue));
        m_btn_resume.signal_clicked().connect(sigc::mem_fun(*this, &QueueDialog::resume_queue));

        m_spin_refresh.signal_value_changed().connect(sigc::mem_fun(*this, &QueueDialog::restart_timer));
        m_spin_age.signal_value_changed().connect(sigc::mem_fun(*this, &QueueDialog::apply_age_highlight));

        // Initial load + timer
        refresh();
        restart_timer();

        show_all_children();
    }

private:
    // Columns model
    struct Columns : Gtk::TreeModel::ColumnRecord {
        Columns() {
            add(job_id);
            add(user);
            add(age);
            add(status);
            add(file);
            add(age_minutes); // hidden numeric value for highlight
        }
        Gtk::TreeModelColumn<std::string> job_id;
        Gtk::TreeModelColumn<std::string> user;
        Gtk::TreeModelColumn<std::string> age;
        Gtk::TreeModelColumn<std::string> status;
        Gtk::TreeModelColumn<std::string> file;
        Gtk::TreeModelColumn<int> age_minutes;
    };

    CupsClient& m_cups;

    // Logging callbacks (to main window output)
    std::function<void(const std::string&)> m_log_info;
    std::function<void(const std::string&)> m_log_ok;
    std::function<void(const std::string&)> m_log_warn;
    std::function<void(const std::string&)> m_log_err;

    // Widgets
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
    Gtk::Button m_btn_pause;
    Gtk::Button m_btn_resume;

    Columns m_cols;
    Glib::RefPtr<Gtk::ListStore> m_store;

    sigc::connection m_timer_conn;

    void add_text_column(const Glib::ustring& title, const Gtk::TreeModelColumn<std::string>& col, int min_width_px) {
        auto* renderer = Gtk::manage(new Gtk::CellRendererText());
        auto* column   = Gtk::manage(new Gtk::TreeViewColumn(title, *renderer));
        column->add_attribute(renderer->property_text(), col);
        column->set_resizable(true);
        column->set_min_width(min_width_px);
        m_tree.append_column(*column);
    }

    void apply_age_highlight() {
        // Set a cell-data-func on the Age column renderer, which can set background.
        // Column indices: we added 5 columns in order, Age is index 2.
        const int age_col_index = 2;
        auto* tv_col = m_tree.get_column(age_col_index);
        if (!tv_col) return;

        auto* cell = dynamic_cast<Gtk::CellRendererText*>(tv_col->get_first_cell());
        if (!cell) return;

        int threshold = static_cast<int>(m_spin_age.get_value());

        tv_col->set_cell_data_func(*cell, [this, threshold](Gtk::CellRenderer* r, const Gtk::TreeModel::iterator& iter) {
            auto* crt = dynamic_cast<Gtk::CellRendererText*>(r);
            if (!crt || !iter) return;

            int age_min = (*iter)[m_cols.age_minutes];

            // Reset styling
            crt->property_cell_background_set() = false;

            if (threshold > 0 && age_min >= threshold) {
                // Use a subtle warning-ish background.
                crt->property_cell_background_set() = true;
                crt->property_cell_background() = "#3b2f1b"; // dark amber-ish (readable on dark themes)
            }
        });

        // Force redraw
        m_tree.queue_draw();
    }

    void set_status_line() {
        std::string raw = m_cups.get_printer_state_raw();
        raw = trim_copy(raw);
        bool disabled = raw.find("disabled") != std::string::npos;

        std::string summary = disabled ? "Queue Status: DISABLED / PAUSED" : "Queue Status: ENABLED";
        if (!raw.empty()) summary += "   (" + raw + ")";

        m_status.set_text(summary);
    }

    static std::string fmt_age(const std::optional<std::chrono::system_clock::time_point>& submitted_at, int& out_minutes) {
        out_minutes = -1;
        if (!submitted_at.has_value()) return "unknown";

        auto now = std::chrono::system_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::minutes>(now - *submitted_at);
        out_minutes = static_cast<int>(diff.count());

        if (out_minutes < 1) return "<1m";
        if (out_minutes < 60) return std::to_string(out_minutes) + "m";
        int hours = out_minutes / 60;
        int mins = out_minutes % 60;
        if (hours < 24) {
            return std::to_string(hours) + "h " + std::to_string(mins) + "m";
        }
        int days = hours / 24;
        hours = hours % 24;
        return std::to_string(days) + "d " + std::to_string(hours) + "h";
    }

    void refresh() {
        set_status_line();

        m_store->clear();
        auto jobs = m_cups.get_jobs();

        // Filter out completed/noise: if lpstat outputs errors, show nothing but keep status line.
        for (const auto& j : jobs) {
            auto row = *m_store->append();
            row[m_cols.job_id] = j.job_id;
            row[m_cols.user]   = j.user;

            int age_min = -1;
            row[m_cols.age] = fmt_age(j.submitted_at, age_min);
            row[m_cols.age_minutes] = age_min < 0 ? 0 : age_min;

            row[m_cols.status] = j.status.empty() ? "" : j.status;
            row[m_cols.file]   = j.file.empty() ? "" : j.file;
        }

        apply_age_highlight();
    }

    void restart_timer() {
        if (m_timer_conn.connected()) m_timer_conn.disconnect();

        int seconds = static_cast<int>(m_spin_refresh.get_value());
        if (seconds <= 0) return;

        m_timer_conn = Glib::signal_timeout().connect_seconds([this]() -> bool {
            refresh();
            return true; // keep running
        }, seconds);
    }

    bool confirm_action(const Glib::ustring& title, const Glib::ustring& message) {
        Gtk::MessageDialog dlg(*this, message, false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_OK_CANCEL, true);
        dlg.set_title(title);
        int resp = dlg.run();
        return resp == Gtk::RESPONSE_OK;
    }

    void cancel_selected() {
        auto sel = m_tree.get_selection()->get_selected();
        if (!sel) {
            m_log_warn("Queue Manager: No job selected.");
            return;
        }

        std::string job_id = (*sel)[m_cols.job_id];
        std::string user   = (*sel)[m_cols.user];

        if (!confirm_action("Confirm Cancel",
                            "Cancel selected job?\n\nJob: " + job_id + "\nUser: " + user)) {
            return;
        }

        m_log_info("Queue Manager: Cancelling job " + job_id + " ...");
        m_cups.cancel_job(job_id);
        m_log_ok("Queue Manager: Cancel requested for " + job_id);

        refresh();
    }

    void cancel_all_from_user() {
        auto sel = m_tree.get_selection()->get_selected();
        if (!sel) {
            m_log_warn("Queue Manager: Select a job to pick a user first.");
            return;
        }
        std::string user = (*sel)[m_cols.user];
        if (user.empty()) {
            m_log_warn("Queue Manager: Selected job has no user.");
            return;
        }

        if (!confirm_action("Confirm Cancel",
                            "Cancel ALL jobs owned by this user?\n\nUser: " + user)) {
            return;
        }

        m_log_info("Queue Manager: Cancelling all jobs for user " + user + " ...");
        m_cups.cancel_all_from_user(user);
        m_log_ok("Queue Manager: Cancel requested for all jobs by " + user);

        refresh();
    }

    void pause_queue() {
        if (!confirm_action("Confirm Pause", "Pause/disable the printer queue?\n\nThis may require sudo privileges.")) {
            return;
        }
        m_log_info("Queue Manager: Pausing queue (cupsdisable) ...");
        m_cups.pause_queue();
        m_log_ok("Queue Manager: Pause requested.");
        refresh();
    }

    void resume_queue() {
        if (!confirm_action("Confirm Resume", "Resume/enable the printer queue?\n\nThis may require sudo privileges.")) {
            return;
        }
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
    virtual ~PrinterDiagnostic();

protected:
    // Widgets
    Gtk::Box m_vbox{Gtk::ORIENTATION_VERTICAL};
    Gtk::Box m_hbox{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Box m_leftbox{Gtk::ORIENTATION_VERTICAL};
    Gtk::ScrolledWindow m_scrolled;
    Gtk::TextView m_textview;
    Glib::RefPtr<Gtk::TextBuffer> m_buffer;

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

    // Tags for colors
    Glib::RefPtr<Gtk::TextTag> m_tag_red;
    Glib::RefPtr<Gtk::TextTag> m_tag_green;
    Glib::RefPtr<Gtk::TextTag> m_tag_yellow;
    Glib::RefPtr<Gtk::TextTag> m_tag_blue;
    Glib::RefPtr<Gtk::TextTag> m_tag_white;
    Glib::RefPtr<Gtk::TextTag> m_tag_bold;
    Glib::RefPtr<Gtk::TextTag> m_tag_bold_cyan;

    // Utility functions
    void print_header(const std::string& text);
    void print_success(const std::string& text);
    void print_error(const std::string& text);
    void print_warning(const std::string& text);
    void print_info(const std::string& text);
    std::string execute_command(const std::string& cmd);

    // Diagnostic functions
    bool check_ping();
    bool check_port_9100();
    bool check_cups_status();
    bool check_stuck_jobs();
    bool check_plugin_version();
    bool get_printer_info();

    // Fix actions
    void clear_stuck_jobs();
    void send_wake_command();
    void restart_cups();
    void print_test_page();

    // Other
    void view_cups_logs();

    // New: queue manager
    void open_queue_manager();

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
    void on_exit();

private:
    std::unique_ptr<CupsClient> m_cups;
};

PrinterDiagnostic::PrinterDiagnostic() {
    set_title("HP P1102w Printer Diagnostic Tool");
    set_default_size(980, 640);

    // CUPS client (used for queue inspection and friendly name mapping)

    // Setup text buffer and tags
    m_buffer = Gtk::TextBuffer::create();
    m_textview.set_buffer(m_buffer);
    m_textview.set_editable(false);
    m_textview.set_cursor_visible(false);

    m_tag_red = Gtk::TextTag::create();
    m_tag_red->property_foreground() = "red";
    m_buffer->get_tag_table()->add(m_tag_red);

    m_tag_green = Gtk::TextTag::create();
    m_tag_green->property_foreground() = "green";
    m_buffer->get_tag_table()->add(m_tag_green);

    m_tag_yellow = Gtk::TextTag::create();
    m_tag_yellow->property_foreground() = "yellow";
    m_buffer->get_tag_table()->add(m_tag_yellow);

    m_tag_blue = Gtk::TextTag::create();
    m_tag_blue->property_foreground() = "blue";
    m_buffer->get_tag_table()->add(m_tag_blue);

    m_tag_white = Gtk::TextTag::create();
    m_tag_white->property_foreground() = "white";
    m_buffer->get_tag_table()->add(m_tag_white);

    m_tag_bold = Gtk::TextTag::create();
    m_tag_bold->property_weight() = Pango::WEIGHT_BOLD;
    m_buffer->get_tag_table()->add(m_tag_bold);

    m_tag_bold_cyan = Gtk::TextTag::create();
    m_tag_bold_cyan->property_foreground() = "cyan";
    m_tag_bold_cyan->property_weight() = Pango::WEIGHT_BOLD;
    m_buffer->get_tag_table()->add(m_tag_bold_cyan);

    // Setup layout
    add(m_vbox);
    m_vbox.pack_start(m_hbox, true, true, 0);

    m_leftbox.set_border_width(10);
    m_leftbox.set_spacing(4);
    m_hbox.pack_start(m_leftbox, false, false, 0);

    // Header labels
    const std::string friendly_name = m_cups ? m_cups->get_printer_friendly_name() : PRINTER_NAME;
    Gtk::Label lbl_printer("Printer: " + friendly_name);
    lbl_printer.set_xalign(0.0f);
    m_leftbox.pack_start(lbl_printer, false, false, 0);

    Gtk::Label lbl_queue("CUPS Queue: " + PRINTER_NAME);
    lbl_queue.set_xalign(0.0f);
    m_leftbox.pack_start(lbl_queue, false, false, 0);

    Gtk::Label lbl_ip("IP Address: " + PRINTER_IP);
    lbl_ip.set_xalign(0.0f);
    m_leftbox.pack_start(lbl_ip, false, false, 0);

    Gtk::Label lbl_port("Port: " + std::to_string(PRINTER_PORT));
    lbl_port.set_xalign(0.0f);
    m_leftbox.pack_start(lbl_port, false, false, 0);

    Gtk::Label lbl_diagnostics("\nDIAGNOSTICS:");
    lbl_diagnostics.set_xalign(0.0f);
    m_leftbox.pack_start(lbl_diagnostics, false, false, 0);

    // Buttons
    m_leftbox.pack_start(m_btn_quick_test, false, false, 0);
    m_leftbox.pack_start(m_btn_full_diagnostic, false, false, 0);
    m_leftbox.pack_start(m_btn_cups_status, false, false, 0);
    m_leftbox.pack_start(m_btn_stuck_jobs, false, false, 0);
    m_leftbox.pack_start(m_btn_plugin_version, false, false, 0);
    m_leftbox.pack_start(m_btn_printer_info, false, false, 0);

    Gtk::Label lbl_fixes("\nFIXES:");
    lbl_fixes.set_xalign(0.0f);
    m_leftbox.pack_start(lbl_fixes, false, false, 0);

    m_leftbox.pack_start(m_btn_clear_jobs, false, false, 0);
    m_leftbox.pack_start(m_btn_wake_command, false, false, 0);
    m_leftbox.pack_start(m_btn_restart_cups, false, false, 0);
    m_leftbox.pack_start(m_btn_test_page, false, false, 0);

    Gtk::Label lbl_other("\nOTHER:");
    lbl_other.set_xalign(0.0f);
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

    // Scrolled window for textview
    m_scrolled.add(m_textview);
    m_scrolled.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_hbox.pack_start(m_scrolled, true, true, 0);

    // Create reusable CUPS client using our existing command executor
    m_cups = std::make_unique<CupsClient>([this](const std::string& cmd) { return this->execute_command(cmd); });

    show_all_children();
}

PrinterDiagnostic::~PrinterDiagnostic() = default;

// ============================================================
// Output helpers
// ============================================================
void PrinterDiagnostic::print_header(const std::string& text) {
    auto iter = m_buffer->end();
    iter = m_buffer->insert_with_tag(iter, "\n=======================================\n", m_tag_bold_cyan);
    iter = m_buffer->insert_with_tag(iter, text + "\n", m_tag_bold_cyan);
    iter = m_buffer->insert_with_tag(iter, "=======================================\n\n", m_tag_bold_cyan);
    Gtk::TextIter end_iter = m_buffer->end();
    m_textview.scroll_to(end_iter);
}

void PrinterDiagnostic::print_success(const std::string& text) {
    auto iter = m_buffer->end();
    m_buffer->insert_with_tag(iter, "✓ " + text + "\n", m_tag_green);
    Gtk::TextIter end_iter = m_buffer->end();
    m_textview.scroll_to(end_iter);
}

void PrinterDiagnostic::print_error(const std::string& text) {
    auto iter = m_buffer->end();
    m_buffer->insert_with_tag(iter, "✗ " + text + "\n", m_tag_red);
    Gtk::TextIter end_iter = m_buffer->end();
    m_textview.scroll_to(end_iter);
}

void PrinterDiagnostic::print_warning(const std::string& text) {
    auto iter = m_buffer->end();
    m_buffer->insert_with_tag(iter, "⚠ " + text + "\n", m_tag_yellow);
    Gtk::TextIter end_iter = m_buffer->end();
    m_textview.scroll_to(end_iter);
}

void PrinterDiagnostic::print_info(const std::string& text) {
    auto iter = m_buffer->end();
    m_buffer->insert_with_tag(iter, "ℹ " + text + "\n", m_tag_blue);
    Gtk::TextIter end_iter = m_buffer->end();
    m_textview.scroll_to(end_iter);
}

std::string PrinterDiagnostic::execute_command(const std::string& cmd) {
    std::array<char, 256> buffer{};
    std::string result;

    auto deleter = [](FILE* f) { if (f) pclose(f); };
    std::unique_ptr<FILE, decltype(deleter)> pipe(popen(cmd.c_str(), "r"), deleter);
    if (!pipe) throw std::runtime_error("popen() failed");

    while (fgets(buffer.data(), (int)buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

// ============================================================
// Diagnostics
// ============================================================
bool PrinterDiagnostic::check_ping() {
    print_info("Testing network connectivity (ping)...");
    std::string cmd = "ping -c 3 -W 2 " + PRINTER_IP + " 2>&1";
    std::string result = execute_command(cmd);
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
    std::string cmd = "lpstat -p \"" + PRINTER_NAME + "\" 2>&1";
    std::string result = execute_command(cmd);

    if (result.find("idle") != std::string::npos) {
        print_success("CUPS queue is idle and ready");
        return true;
    }
    if (result.find("disabled") != std::string::npos) {
        print_error("CUPS queue is DISABLED");
        print_warning("Run: sudo cupsenable \"" + PRINTER_NAME + "\"");

        // Phase 1 (dry-run): assess whether this looks safely auto-recoverable.
        // No actions are taken yet — we only explain what would happen.
        try {
            const auto jobs = m_cups ? m_cups->get_jobs() : std::vector<PrintJob>{};
            const bool queue_empty = jobs.empty();

            std::string matched_reason;
            const bool recoverable_hint = m_cups ? m_cups->has_recoverable_reason_hint(&matched_reason) : false;

            if (queue_empty && recoverable_hint) {
                print_success("Auto-Recovery eligible (dry-run): queue is empty and reason looks recoverable (" + matched_reason + ").");
                print_info("Would run: cupsenable + cupsaccept for this queue (not executed in this phase).");
            } else {
                if (!queue_empty) {
                    print_warning("Auto-Recovery skipped (dry-run): queue is not empty (active/pending jobs present).");
                } else {
                    print_warning("Auto-Recovery skipped (dry-run): reason not recognized as safely recoverable.");
                    print_info("Tip: If this is truly stale (e.g., you added paper), manually re-enable via CUPS or cupsenable.");
                }
            }
        } catch (const std::exception& e) {
            print_warning(std::string("Auto-Recovery assessment failed (dry-run): ") + e.what());
        }

        return false;
    }

    print_warning("Unknown CUPS status");
    auto iter = m_buffer->end();
    m_buffer->insert(iter, result + "\n");
    return false;
}

bool PrinterDiagnostic::check_stuck_jobs() {
    print_info("Checking for stuck print jobs...");
    std::string cmd = "lpstat -o 2>&1";
    std::string result = execute_command(cmd);

    if (trim_copy(result).empty()) {
        print_success("No stuck jobs in queue");
        return true;
    }

    print_warning("Found jobs in queue:");
    auto iter = m_buffer->end();
    m_buffer->insert(iter, result + "\n");
    return false;
}

bool PrinterDiagnostic::check_plugin_version() {
    print_info("Checking HPLIP plugin version...");
    std::string cmd = "sudo journalctl -u cups --since '5 minutes ago' 2>&1 | grep -i 'plugin.*mismatch'";
    std::string result = execute_command(cmd);

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
    std::string result = execute_command(cmd);

    if (result.find("Communication status: Good") != std::string::npos || result.find("Device") != std::string::npos) {
        print_success("HPLIP can communicate with printer");
        auto iter = m_buffer->end();
        m_buffer->insert_with_tag(iter, "\n" + result + "\n", m_tag_white);
        return true;
    }

    print_error("HPLIP cannot communicate with printer");
    auto iter = m_buffer->end();
    m_buffer->insert(iter, result + "\n");
    return false;
}

// ============================================================
// Fix actions
// ============================================================
void PrinterDiagnostic::clear_stuck_jobs() {
    print_info("Clearing all stuck jobs...");
    execute_command("cancel -a 2>&1");
    print_success("All jobs cancelled");
}

void PrinterDiagnostic::send_wake_command() {
    print_info("Sending wake command to printer...");
    std::string cmd =
        "printf '\\x1B%%-12345X@PJL\\r\\n@PJL INFO STATUS\\r\\n\\x1B%%-12345X\\r\\n' | "
        "nc " + PRINTER_IP + " " + std::to_string(PRINTER_PORT) + " -w 3 2>/dev/null";
    execute_command(cmd);
    sleep(2);
    print_success("Wake command sent - wait 5 seconds then test again");
}

void PrinterDiagnostic::restart_cups() {
    print_info("Restarting CUPS service...");
    execute_command("sudo systemctl restart cups 2>&1");
    print_success("CUPS restarted");
}

void PrinterDiagnostic::print_test_page() {
    print_info("Sending test page to printer...");
    time_t now = time(nullptr);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    std::string cmd = "echo \"Diagnostic Test Page - " + std::string(timestamp) + "\" | lpr 2>&1";
    execute_command(cmd);
    print_success("Test page sent - check printer");
}

// ============================================================
// Logs
// ============================================================
void PrinterDiagnostic::view_cups_logs() {
    print_header("Recent CUPS Logs (last 50 lines)");
    std::string cmd = "sudo journalctl -u cups -n 50 --no-pager 2>&1";
    std::string result = execute_command(cmd);
    auto iter = m_buffer->end();
    m_buffer->insert_with_tag(iter, result + "\n", m_tag_white);
}

// ============================================================
// Queue manager
// ============================================================
void PrinterDiagnostic::open_queue_manager() {
    print_header("Queue Manager");
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

void PrinterDiagnostic::on_queue_manager() {
    m_buffer->set_text("");
    open_queue_manager();
}

// ============================================================
// Button handlers
// ============================================================
void PrinterDiagnostic::on_quick_test() {
    m_buffer->set_text("");
    print_header("Quick Diagnostic Test");
    bool ping_ok = check_ping();
    bool port_ok = check_port_9100();

    auto iter = m_buffer->end();
    m_buffer->insert_with_tag(iter, "\nSUMMARY:\n", m_tag_bold);

    if (ping_ok && port_ok) {
        print_success("Printer is fully operational!");
        print_info("You can try printing now");
    } else if (ping_ok && !port_ok) {
        print_warning("Printer is online but print service is asleep");
        print_info("Recommended action: Press printer power button or use option 8");
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
    print_header("Printer Info");
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

void PrinterDiagnostic::on_exit() {
    hide();
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv) {
    auto app = Gtk::Application::create(argc, argv, "org.xai.printer_diagnostic");
    PrinterDiagnostic window;
    return app->run(window);
}

#include <gtkmm.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sys/select.h>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>
#include <ctime>

// Configuration
const std::string PRINTER_IP = "192.168.4.68";
const int PRINTER_PORT = 9100;
const std::string PRINTER_NAME = "HP_LaserJet_Professional_P1102w";

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
    Gtk::Button m_btn_exit{"0. Exit"};

    // Tags for colors
    Glib::RefPtr<Gtk::TextTag> m_tag_reset;
    Glib::RefPtr<Gtk::TextTag> m_tag_red;
    Glib::RefPtr<Gtk::TextTag> m_tag_green;
    Glib::RefPtr<Gtk::TextTag> m_tag_yellow;
    Glib::RefPtr<Gtk::TextTag> m_tag_blue;
    Glib::RefPtr<Gtk::TextTag> m_tag_cyan;
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
    void on_exit();
};

PrinterDiagnostic::PrinterDiagnostic() {
    set_title("HP P1102w Printer Diagnostic Tool");
    set_default_size(800, 600);

    // Setup text buffer and tags
    m_buffer = Gtk::TextBuffer::create();
    m_textview.set_buffer(m_buffer);
    m_textview.set_editable(false);
    m_textview.set_cursor_visible(false);

    // Create tags
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

    m_tag_cyan = Gtk::TextTag::create();
    m_tag_cyan->property_foreground() = "cyan";
    m_buffer->get_tag_table()->add(m_tag_cyan);

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
    m_hbox.pack_start(m_leftbox, false, false, 0);

    // Add header labels
    Gtk::Label lbl_printer("Printer: " + PRINTER_NAME);
    m_leftbox.pack_start(lbl_printer, false, false, 0);
    Gtk::Label lbl_ip("IP Address: " + PRINTER_IP);
    m_leftbox.pack_start(lbl_ip, false, false, 0);
    Gtk::Label lbl_port("Port: " + std::to_string(PRINTER_PORT));
    m_leftbox.pack_start(lbl_port, false, false, 0);

    Gtk::Label lbl_diagnostics("\nDIAGNOSTICS:");
    m_leftbox.pack_start(lbl_diagnostics, false, false, 0);

    // Add buttons
    m_leftbox.pack_start(m_btn_quick_test, false, false, 0);
    m_leftbox.pack_start(m_btn_full_diagnostic, false, false, 0);
    m_leftbox.pack_start(m_btn_cups_status, false, false, 0);
    m_leftbox.pack_start(m_btn_stuck_jobs, false, false, 0);
    m_leftbox.pack_start(m_btn_plugin_version, false, false, 0);
    m_leftbox.pack_start(m_btn_printer_info, false, false, 0);

    Gtk::Label lbl_fixes("\nFIXES:");
    m_leftbox.pack_start(lbl_fixes, false, false, 0);
    m_leftbox.pack_start(m_btn_clear_jobs, false, false, 0);
    m_leftbox.pack_start(m_btn_wake_command, false, false, 0);
    m_leftbox.pack_start(m_btn_restart_cups, false, false, 0);
    m_leftbox.pack_start(m_btn_test_page, false, false, 0);

    Gtk::Label lbl_other("\nOTHER:");
    m_leftbox.pack_start(lbl_other, false, false, 0);
    m_leftbox.pack_start(m_btn_view_logs, false, false, 0);
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
    m_btn_exit.signal_clicked().connect(sigc::mem_fun(*this, &PrinterDiagnostic::on_exit));

    // Scrolled window for textview
    m_scrolled.add(m_textview);
    m_hbox.pack_start(m_scrolled, true, true, 0);

    show_all_children();
}

PrinterDiagnostic::~PrinterDiagnostic() {}

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
    std::array<char, 128> buffer;
    std::string result;
    auto deleter = [](FILE* f) { pclose(f); };
    std::unique_ptr<FILE, decltype(deleter)> pipe(popen(cmd.c_str(), "r"), deleter);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

bool PrinterDiagnostic::check_ping() {
    print_info("Testing network connectivity (ping)...");
    std::string cmd = "ping -c 3 -W 2 " + PRINTER_IP + " 2>&1";
    std::string result = execute_command(cmd);
    if (result.find("0% packet loss") != std::string::npos || result.find("3 received") != std::string::npos) {
        print_success("Printer responds to ping - Network OK");
        return true;
    } else {
        print_error("Printer does not respond to ping - Network issue");
        print_warning("Check: Printer power, WiFi connection, AX55 bridge");
        return false;
    }
}

bool PrinterDiagnostic::check_port_9100() {
    print_info("Testing JetDirect port 9100...");

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) return false;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PRINTER_PORT);
    addr.sin_addr.s_addr = inet_addr(PRINTER_IP.c_str());

    fcntl(sock, F_SETFL, O_NONBLOCK);

    int res = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
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

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;

    res = select(sock + 1, NULL, &writeSet, NULL, &tv);
    if (res <= 0) {
        ::close(sock);
        if (res == 0) print_error("Port 9100 TIMEOUT - Printer not responding");
        else print_error("Port 9100 ERROR: " + std::string(strerror(errno)));
        print_warning("Solution: Power cycle the printer");
        return false;
    }

    int so_error;
    socklen_t len = sizeof(so_error);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
    ::close(sock);
    if (so_error == 0) {
        print_success("Port 9100 is OPEN - Printer ready to receive jobs");
        return true;
    } else if (so_error == ECONNREFUSED) {
        print_error("Port 9100 REFUSED - Printer is in deep sleep");
        print_warning("Solution: Press printer power button once to wake");
    } else {
        print_error("Port 9100 ERROR: " + std::string(strerror(so_error)));
    }
    return false;
}

bool PrinterDiagnostic::check_cups_status() {
    print_info("Checking CUPS printer queue...");
    std::string cmd = "lpstat -p \"" + PRINTER_NAME + "\" 2>&1";
    std::string result = execute_command(cmd);
    if (result.find("idle") != std::string::npos) {
        print_success("CUPS queue is idle and ready");
        return true;
    } else if (result.find("disabled") != std::string::npos) {
        print_error("CUPS queue is DISABLED");
        print_warning("Run: sudo cupsenable " + PRINTER_NAME);
        return false;
    } else {
        print_warning("Unknown CUPS status");
        auto iter = m_buffer->end();
        m_buffer->insert(iter, result + "\n");
        return false;
    }
}

bool PrinterDiagnostic::check_stuck_jobs() {
    print_info("Checking for stuck print jobs...");
    std::string cmd = "lpstat -o 2>&1";
    std::string result = execute_command(cmd);
    if (result.empty() || result.find_first_not_of(" \t\n\r") == std::string::npos) {
        print_success("No stuck jobs in queue");
        return true;
    } else {
        print_warning("Found jobs in queue:");
        auto iter = m_buffer->end();
        m_buffer->insert(iter, result + "\n");
        return false;
    }
}

bool PrinterDiagnostic::check_plugin_version() {
    print_info("Checking HPLIP plugin version...");
    std::string cmd = "sudo journalctl -u cups --since '5 minutes ago' 2>&1 | grep -i 'plugin.*mismatch'";
    std::string result = execute_command(cmd);
    if (result.empty() || result.find_first_not_of(" \t\n\r") == std::string::npos) {
        print_success("No plugin version errors detected");
        return true;
    } else {
        print_error("Plugin version mismatch detected!");
        print_warning("Run: yay -S hplip-plugin --rebuild");
        print_warning("Then: sudo systemctl restart cups");
        return false;
    }
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
    } else {
        print_error("HPLIP cannot communicate with printer");
        auto iter = m_buffer->end();
        m_buffer->insert(iter, result + "\n");
        return false;
    }
}

void PrinterDiagnostic::clear_stuck_jobs() {
    print_info("Clearing all stuck jobs...");
    execute_command("cancel -a 2>&1");
    print_success("All jobs cancelled");
}

void PrinterDiagnostic::send_wake_command() {
    print_info("Sending wake command to printer...");
    std::string cmd = "printf '\\x1B%%-12345X@PJL\\r\\n@PJL INFO STATUS\\r\\n\\x1B%%-12345X\\r\\n' | nc " + PRINTER_IP + " " + std::to_string(PRINTER_PORT) + " -w 3 2>/dev/null";
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
    time_t now = time(0);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    std::string cmd = "echo \"Diagnostic Test Page - " + std::string(timestamp) + "\" | lpr 2>&1";
    execute_command(cmd);
    print_success("Test page sent - check printer");
}

void PrinterDiagnostic::view_cups_logs() {
    print_header("Recent CUPS Logs (last 50 lines)");
    std::string cmd = "sudo journalctl -u cups -n 50 --no-pager 2>&1";
    std::string result = execute_command(cmd);
    auto iter = m_buffer->end();
    m_buffer->insert_with_tag(iter, result + "\n", m_tag_white);
}

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
        print_info("Check: Printer power, WiFi status, AX55 bridge");
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

int main(int argc, char* argv[]) {
    auto app = Gtk::Application::create(argc, argv, "org.xai.printer_diagnostic");
    PrinterDiagnostic window;
    return app->run(window);
}

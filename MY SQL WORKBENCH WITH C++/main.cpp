// android_flash_tool.cpp
// Enhanced Pixel Android Flash Tool (console UI, colored, progress, MySQL logging, flashing)
// Build (MSVC):
//   cl /EHsc /std:c++17 android_flash_tool.cpp /I"path_to_mysqlcppconn_include" /link mysqlcppconn.lib libcurl.lib
// Build (g++):
//   g++ android_flash_tool.cpp -o android_flash_tool -lmysqlcppconn -lcurl -std=c++17
//
// Requirements:
// - libcurl and MySQL Connector/C++ available
// - adb and fastboot in PATH
// - unzip/tar preferred for extraction (tool falls back to instruction if absent)
// - put config.ini next to exe (sample provided in comments above)
//
// Note: this file avoids <filesystem> so it compiles even if <filesystem> isn't available.

#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <tuple>
#include <memory>
#include <thread>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cerrno>

#include <mysql_driver.h>
#include <mysql_connection.h>
#include <cppconn/statement.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>

#include <curl/curl.h>

#ifdef _WIN32
#include <direct.h>   // _mkdir
#include <conio.h>
#include <windows.h>
#define popen _popen
#define pclose _pclose
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace std;
using namespace sql;

// --------------------------- Utilities & Small helpers ---------------------------

static const char* SYMBOL_OK = "✔";
static const char* SYMBOL_FAIL = "✘";
static const char* SYMBOL_WARN = "⚠";

struct Config {
    string db_host = "tcp://127.0.0.1:3306";
    string db_user = "root";
    string db_pass = "";
    string downloads_dir = "downloads";
    string user_agent = "PixelFlashTool/3.0";
};

static Config g_config;

// Simple INI parser (very small)
void load_config_from_file(const string& path) {
    ifstream f(path);
    if (!f.is_open()) return;
    string line;
    string section;
    while (getline(f, line)) {
        // trim
        auto trim = [](string& s) {
            while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
            while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
            };
        trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == string::npos) continue;
        string key = line.substr(0, eq);
        string val = line.substr(eq + 1);
        trim(key); trim(val);
        // simple mapping
        if (section == "mysql") {
            if (key == "host") g_config.db_host = val;
            else if (key == "user") g_config.db_user = val;
            else if (key == "pass") g_config.db_pass = val;
        }
        else if (section == "tool") {
            if (key == "downloads_dir") g_config.downloads_dir = val;
            else if (key == "user_agent") g_config.user_agent = val;
        }
    }
}

// portable mkdir (no <filesystem>)
bool make_dir(const string& path) {
#ifdef _WIN32
    int r = _mkdir(path.c_str());
    return (r == 0) || (errno == EEXIST);
#else
    int r = mkdir(path.c_str(), 0755);
    return (r == 0) || (errno == EEXIST);
#endif
}

bool file_exists(const string& path) {
    ifstream f(path.c_str());
    return f.good();
}

// sanitize simple filenames
string sanitize_filename(string s) {
    for (auto& c : s) {
        if (c == ' ' || c == '/' || c == ':' || c == '\\' || c == '\t') c = '_';
    }
    return s;
}

// cross-platform getch
int getch_portable() {
#ifdef _WIN32
    return _getch();
#else
    char ch = 0;
    struct termios oldt {};
    if (tcgetattr(STDIN_FILENO, &oldt) != 0) return 0;
    struct termios newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    if (read(STDIN_FILENO, &ch, 1) < 0) ch = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return (int)ch;
#endif
}

// run a system command and capture output (stdout only)
string run_command_capture(const string& cmd) {
    string full = cmd + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) return string();
    char buffer[512];
    string result;
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    pclose(pipe);
    return result;
}

// --------------------------- Console coloring ---------------------------
#ifdef _WIN32
void set_console_color(int attr) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), attr);
}
void reset_console_color() { set_console_color(7); }
#else
void set_console_color(int code) {
    // Accept some simple codes for convenience
    // 31 = red, 32 = green, 33 = yellow, 36 = cyan, 0 = reset
    cout << "\033[" << code << "m";
}
void reset_console_color() { cout << "\033[0m"; }
#endif

void color_print_ok(const string& s) {
#ifdef _WIN32
    set_console_color(10); cout << s; reset_console_color();
#else
    set_console_color(32); cout << s; reset_console_color();
#endif
}
void color_print_fail(const string& s) {
#ifdef _WIN32
    set_console_color(12); cout << s; reset_console_color();
#else
    set_console_color(31); cout << s; reset_console_color();
#endif
}
void color_print_warn(const string& s) {
#ifdef _WIN32
    set_console_color(14); cout << s; reset_console_color();
#else
    set_console_color(33); cout << s; reset_console_color();
#endif
}
void color_print_info(const string& s) {
#ifdef _WIN32
    set_console_color(11); cout << s; reset_console_color();
#else
    set_console_color(36); cout << s; reset_console_color();
#endif
}

// --------------------------- MySQL Logging ---------------------------
void ensure_logging_db(sql::Connection* con) {
    try {
        unique_ptr<sql::Statement> s(con->createStatement());
        s->execute("CREATE DATABASE IF NOT EXISTS pixel_data");
        con->setSchema("pixel_data");
        s->execute(
            "CREATE TABLE IF NOT EXISTS user_logs ("
            "log_id INT AUTO_INCREMENT PRIMARY KEY, "
            "device_name VARCHAR(100), "
            "action VARCHAR(255), "
            "result VARCHAR(255), "
            "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)"
        );
    }
    catch (sql::SQLException& e) {
        color_print_warn(string("[DB WARN] ") + e.what() + "\n");
    }
}

void log_action(sql::Connection* con, const string& device, const string& action, const string& result) {
    try {
        con->setSchema("pixel_data");
        unique_ptr<sql::PreparedStatement> p(con->prepareStatement(
            "INSERT INTO user_logs (device_name, action, result) VALUES (?, ?, ?)"
        ));
        p->setString(1, device);
        p->setString(2, action);
        p->setString(3, result);
        p->execute();
    }
    catch (sql::SQLException& e) {
        color_print_warn(string("[DB WARN] ") + e.what() + "\n");
    }
}

// --------------------------- CURL Download w/ progress ---------------------------
struct CurlProgress {
    curl_off_t last_total = 0;
    curl_off_t last_now = 0;
    chrono::steady_clock::time_point last_time;
};

static int curl_progress_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    CurlProgress* st = reinterpret_cast<CurlProgress*>(clientp);
    if (dltotal <= 0) return 0;
    int percent = static_cast<int>((dlnow * 100) / dltotal);
    const int width = 50;
    int pos = (percent * width) / 100;
    cout << "\r[";
    // green bar
#ifdef _WIN32
    set_console_color(10);
#else
    cout << "\033[32m";
#endif
    for (int i = 0; i < width; ++i) {
        if (i < pos) cout << "#";
        else cout << " ";
    }
#ifdef _WIN32
    reset_console_color();
#else
    cout << "\033[0m";
#endif
    cout << "] " << percent << "% ";
    cout << (dlnow / 1024) << "KB / " << (dltotal / 1024) << "KB   ";
    cout.flush();
    return 0;
}

static size_t curl_write_file(void* ptr, size_t size, size_t nmemb, void* stream) {
    FILE* fp = reinterpret_cast<FILE*>(stream);
    return fwrite(ptr, size, nmemb, fp);
}

bool download_with_progress(const string& url, const string& out_path, const string& user_agent) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    FILE* fp = fopen(out_path.c_str(), "wb");
    if (!fp) { curl_easy_cleanup(curl); return false; }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
#ifdef CURLOPT_XFERINFOFUNCTION
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_callback);
    CurlProgress prog;
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &prog);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#else
    // older libcurl fallback: no progress
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
#endif
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);
    cout << "\n";
    fclose(fp);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        color_print_fail(string(SYMBOL_FAIL) + " Download failed: " + curl_easy_strerror(res) + "\n");
        return false;
    }
    return true;
}

// --------------------------- Device / Fastboot helpers ---------------------------
bool adb_has_device() {
    string out = run_command_capture("adb devices");
    // skip header line
    return out.find("\tdevice") != string::npos;
}

vector<string> fastboot_list_devices() {
    string out = run_command_capture("fastboot devices");
    vector<string> ids;
    istringstream iss(out);
    string line;
    while (getline(iss, line)) {
        // line typically: <serial>\tfastboot
        if (line.empty()) continue;
        // split by whitespace
        istringstream ls(line);
        string id, tag;
        ls >> id >> tag;
        if (!id.empty()) ids.push_back(id);
    }
    return ids;
}

bool fastboot_flash_partition(const string& serial, const string& part, const string& image_path) {
    // run: fastboot -s <serial> flash <part> "<image_path>"
    string cmd = "fastboot";
    if (!serial.empty()) cmd += " -s " + serial;
    cmd += " flash " + part + " \"" + image_path + "\"";
    color_print_info("[CMD] " + cmd + "\n");
    string out = run_command_capture(cmd);
    if (out.find("OKAY") != string::npos || out.find("Flashing") != string::npos) {
        color_print_ok(string(SYMBOL_OK) + " fastboot flash " + part + " -> OK\n");
        return true;
    }
    else {
        color_print_fail(string(SYMBOL_FAIL) + " fastboot flash " + part + " -> Failed\n");
        color_print_warn("fastboot output:\n" + out + "\n");
        return false;
    }
}

bool fastboot_reboot(const string& serial) {
    string cmd = "fastboot";
    if (!serial.empty()) cmd += " -s " + serial;
    cmd += " reboot";
    run_command_capture(cmd);
    return true;
}

// --------------------------- Image heuristics ---------------------------

string guess_partition_from_filename(const string& fname) {
    string s = fname;
    for (auto& c : s) c = tolower((unsigned char)c);
    if (s.find("boot.img") != string::npos) return "boot";
    if (s.find("vendor_boot") != string::npos) return "vendor_boot";
    if (s.find("system.img") != string::npos) return "system";
    if (s.find("vendor.img") != string::npos) return "vendor";
    if (s.find("vbmeta.img") != string::npos) return "vbmeta";
    if (s.find("recovery.img") != string::npos) return "recovery";
    if (s.find("product.img") != string::npos) return "product";
    if (s.find("userdata.img") != string::npos) return "userdata";
    // fallback: look for common token
    if (s.find("boot") != string::npos) return "boot";
    if (s.find("system") != string::npos) return "system";
    if (s.find("vendor") != string::npos) return "vendor";
    return "";
}

// scan directory (non-recursive) for *.img files - simple port without filesystem
vector<string> list_img_files_in_dir(const string& dir) {
    vector<string> found;
    // try using 'dir' (Windows) or 'ls' (POSIX) to list files - simple approach
#ifdef _WIN32
    string cmd = "dir /b \"" + dir + "\\*.img\" 2>nul";
#else
    string cmd = "ls \"" + dir + "\"/*.img 2>/dev/null";
#endif
    string out = run_command_capture(cmd);
    if (out.empty()) return found;
    istringstream iss(out);
    string line;
    while (getline(iss, line)) {
        // Trim
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
#ifdef _WIN32
        found.push_back(dir + "\\" + line);
#else
        found.push_back(dir + "/" + line);
#endif
    }
    return found;
}

// try to extract archive using available system tools (unzip/tar)
bool extract_archive_simple(const string& archive_path, const string& target_dir) {
    // create target dir
    make_dir(target_dir);
#ifdef _WIN32
    // Try PowerShell Expand-Archive if available
    string cmd = "powershell -Command \"Expand-Archive -Force -Path \\\"" + archive_path + "\\\" -DestinationPath \\\"" + target_dir + "\\\"\"";
    string out = run_command_capture(cmd);
    // If PowerShell not present or failed, try 7z if in PATH
    if (file_exists(target_dir) && !list_img_files_in_dir(target_dir).empty()) return true;
    cmd = "7z x \"" + archive_path + "\" -o\"" + target_dir + "\" -y";
    out = run_command_capture(cmd);
    if (!list_img_files_in_dir(target_dir).empty()) return true;
    // fallback: ask user to extract manually
    return false;
#else
    // POSIX: try unzip or tar
    string cmd = "unzip -o \"" + archive_path + "\" -d \"" + target_dir + "\" 2>/dev/null";
    run_command_capture(cmd);
    if (!list_img_files_in_dir(target_dir).empty()) return true;
    // try tar
    cmd = "tar -xf \"" + archive_path + "\" -C \"" + target_dir + "\" 2>/dev/null";
    run_command_capture(cmd);
    if (!list_img_files_in_dir(target_dir).empty()) return true;
    return false;
#endif
}

// --------------------------- UI & Flow ---------------------------

void print_banner() {
#ifdef _WIN32
    set_console_color(14);
    cout << "=====================================================\n";
    set_console_color(11);
    cout << "           PIXEL ANDROID FLASH TOOL v3.0\n";
    set_console_color(14);
    cout << "=====================================================\n";
    reset_console_color();
#else
    cout << "\033[33m=====================================================\n";
    cout << "\033[36m           PIXEL ANDROID FLASH TOOL v3.0\n";
    cout << "\033[33m=====================================================\n";
    cout << "\033[0m";
#endif
}

int prompt_menu() {
    cout << "\nMain Menu:\n";
    cout << "  1) Download Firmware\n";
    cout << "  2) Check Device (ADB / Fastboot)\n";
    cout << "  3) Flash Firmware (auto)\n";
    cout << "  4) Reboot Device / Fastboot Reboot\n";
    cout << "  5) View Recent Logs (DB)\n";
    cout << "  0) Exit\n";
    cout << "Choose: ";
    int sel = -1;
    if (!(cin >> sel)) {
        cin.clear();
        string tmp; getline(cin, tmp);
        return -1;
    }
    return sel;
}

void show_recent_logs(sql::Connection* con, int limit = 10) {
    try {
        con->setSchema("pixel_data");
        unique_ptr<sql::PreparedStatement> p(con->prepareStatement(
            "SELECT device_name, action, result, created_at FROM user_logs ORDER BY created_at DESC LIMIT ?"
        ));
        p->setInt(1, limit);
        unique_ptr<sql::ResultSet> rs(p->executeQuery());
        cout << "\nRecent logs:\n";
        while (rs->next()) {
            cout << rs->getString("created_at") << " | " << rs->getString("device_name")
                << " | " << rs->getString("action") << " | " << rs->getString("result") << "\n";
        }
    }
    catch (sql::SQLException& e) {
        color_print_warn(string("[DB WARN] ") + e.what() + "\n");
    }
}

// The main "automatic flash" flow:
// - expects a downloaded / extracted folder path with .img files
// - asks user to confirm, attempts flashing images matching heuristics
void auto_flash_flow(sql::Connection* con, const string& serial, const string& extracted_dir, const string& device_name) {
    vector<string> imgs = list_img_files_in_dir(extracted_dir);
    if (imgs.empty()) {
        color_print_warn(SYMBOL_WARN " No image files found in: " + extracted_dir + "\n");
        color_print_info("If your factory archive contains images in nested subfolders, extract manually and provide the folder path.\n");
        return;
    }

    cout << "Found images:\n";
    for (size_t i = 0; i < imgs.size(); ++i) cout << "  " << (i + 1) << ") " << imgs[i] << "\n";

    cout << "\nThis will attempt to flash recognized images (best-effort). This can wipe data. Type 'YES' to proceed: ";
    string confirm;
    cin >> confirm;
    if (confirm != "YES") {
        cout << "Aborted by user.\n";
        log_action(con, device_name, "auto_flash", "aborted_by_user");
        return;
    }

    // Iterate images and flash
    for (size_t i = 0; i < imgs.size(); ++i) {
        string img = imgs[i];
        // extract filename portion
        string fname = img;
        auto pos1 = fname.find_last_of("/\\");
        if (pos1 != string::npos) fname = fname.substr(pos1 + 1);
        string partition = guess_partition_from_filename(fname);
        if (partition.empty()) {
            color_print_warn(string(SYMBOL_WARN) + " Skipping (unknown partition): " + fname + "\n");
            continue;
        }
        color_print_info("Flashing " + fname + " -> " + partition + "\n");
        bool ok = fastboot_flash_partition(serial, partition, img);
        log_action(con, device_name, string("flash_") + partition, ok ? "OK" : "FAIL");
        if (!ok) {
            color_print_fail(string(SYMBOL_FAIL) + " Flash failed for " + partition + ". Aborting.\n");
            return;
        }
        // small delay between flashes
        this_thread::sleep_for(chrono::milliseconds(600));
    }

    color_print_ok(string(SYMBOL_OK) + " All recognized images flashed. Attempting fastboot reboot.\n");
    fastboot_reboot(serial);
    log_action(con, device_name, "auto_flash", "completed");
}

// --------------------------- Main program ---------------------------

int main() {
    // load config if exists
    load_config_from_file("config.ini");

    print_banner();
    color_print_info("Loading configuration...\n");
    cout << "DB host: " << g_config.db_host << "  user: " << g_config.db_user << "\n";

    // initialize curl global
    curl_global_init(CURL_GLOBAL_DEFAULT);

    try {
        // Connect to DB
        mysql::MySQL_Driver* driver = mysql::get_mysql_driver_instance();
        unique_ptr<sql::Connection> con(driver->connect(g_config.db_host, g_config.db_user, g_config.db_pass));
        ensure_logging_db(con.get());

        // main loop
        bool running = true;
        while (running) {
            print_banner();
            int sel = prompt_menu();
            if (sel == 0) { running = false; break; }
            else if (sel == 1) {
                // Download firmware
                cout << "\nEnter device (friendly) name (for DB): ";
                string device_name; cin >> ws; getline(cin, device_name);
                cout << "Enter direct download URL: ";
                string url; cin >> ws; getline(cin, url);
                if (url.empty()) { color_print_warn("No URL provided\n"); continue; }
                // prepare downloads dir
                make_dir(g_config.downloads_dir);
                string fn = sanitize_filename(device_name + "_" + to_string(time(nullptr)));
                // naive extension detection
                string ext = ".zip";
                auto p = url.find_last_of('.');
                if (p != string::npos) ext = url.substr(p);
                string outpath = g_config.downloads_dir + "/" + fn + ext;
                color_print_info("Downloading to: " + outpath + "\n");
                bool ok = download_with_progress(url, outpath, g_config.user_agent);
                log_action(con.get(), device_name, "download", ok ? "OK" : "FAIL");
                if (!ok) continue;
                color_print_ok(string(SYMBOL_OK) + " Download finished: " + outpath + "\n");

                // try to auto-extract
                string extract_dir = g_config.downloads_dir + "/" + fn + "_extracted";
                color_print_info("Attempting to extract archive...\n");
                bool extracted = extract_archive_simple(outpath, extract_dir);
                if (!extracted) {
                    color_print_warn(string(SYMBOL_WARN) + " Automatic extraction failed or produced no images.\n");
                    color_print_info("Please manually extract the archive into: " + extract_dir + " and then use Flash Firmware option.\n");
                    continue;
                }
                else {
                    color_print_ok(string(SYMBOL_OK) + " Extracted to: " + extract_dir + "\n");
                }
                // offer to flash immediately
                cout << "Do you want to flash now? (y/N): ";
                string ans; cin >> ans;
                if (!ans.empty() && (ans[0] == 'y' || ans[0] == 'Y')) {
                    // identify fastboot device(s)
                    auto devices = fastboot_list_devices();
                    if (devices.empty()) {
                        color_print_warn("No fastboot devices detected. Put device in bootloader and retry.\n");
                        continue;
                    }
                    // if multiple, ask user to choose
                    string chosen_serial = devices[0];
                    if (devices.size() > 1) {
                        cout << "Multiple fastboot devices detected:\n";
                        for (size_t i = 0; i < devices.size(); ++i) cout << (i + 1) << ") " << devices[i] << "\n";
                        cout << "Choose device (1-" << devices.size() << "): ";
                        int idx = 1; cin >> idx;
                        if (idx < 1 || (size_t)idx > devices.size()) idx = 1;
                        chosen_serial = devices[idx - 1];
                    }
                    auto_flash_flow(con.get(), chosen_serial, extract_dir, device_name);
                }

            }
            else if (sel == 2) {
                // check device
                color_print_info("\nChecking ADB devices...\n");
                string adb_out = run_command_capture("adb devices");
                cout << adb_out;
                color_print_info("\nChecking fastboot devices...\n");
                auto fdevs = fastboot_list_devices();
                if (fdevs.empty()) color_print_warn("No fastboot devices\n");
                else {
                    color_print_ok("Fastboot devices:\n");
                    for (auto& d : fdevs) cout << "  " << d << "\n";
                }
                cout << "Press any key to continue...\n";
                getch_portable();
            }
            else if (sel == 3) {
                // Flash firmware by asking for extracted folder
                cout << "\nEnter device friendly name (for DB): ";
                string device_name; cin >> ws; getline(cin, device_name);
                cout << "Enter path to extracted folder containing .img files: ";
                string folder; cin >> ws; getline(cin, folder);
                if (!file_exists(folder) && folder.back() == '/') {
                    // ignore, we rely on listing to confirm
                }
                auto imgs = list_img_files_in_dir(folder);
                if (imgs.empty()) {
                    color_print_fail(string(SYMBOL_FAIL) + " No .img files found in that directory.\n");
                    continue;
                }
                auto devices = fastboot_list_devices();
                if (devices.empty()) {
                    color_print_warn("No fastboot devices detected. Ensure device is in bootloader and connected.\n");
                    continue;
                }
                string serial = devices[0];
                if (devices.size() > 1) {
                    cout << "Multiple fastboot devices detected. Choose 1-" << devices.size() << ": ";
                    int idx = 1; cin >> idx;
                    if (idx < 1 || (size_t)idx > devices.size()) idx = 1;
                    serial = devices[idx - 1];
                }
                auto_flash_flow(con.get(), serial, folder, device_name);
            }
            else if (sel == 4) {
                // reboot
                cout << "Reboot mode:\n1) adb reboot\n2) fastboot reboot\nChoose: ";
                int r; cin >> r;
                if (r == 1) {
                    color_print_info("Running: adb reboot\n");
                    run_command_capture("adb reboot");
                }
                else {
                    auto fdevs = fastboot_list_devices();
                    if (fdevs.empty()) color_print_warn("No fastboot devices detected.\n");
                    else {
                        string serial = fdevs[0];
                        if (fdevs.size() > 1) {
                            cout << "Multiple fastboot devices found. Choose index (1-" << fdevs.size() << "): ";
                            int idx; cin >> idx;
                            if (idx < 1 || (size_t)idx > fdevs.size()) idx = 1;
                            serial = fdevs[idx - 1];
                        }
                        fastboot_reboot(serial);
                        color_print_ok("fastboot reboot issued.\n");
                    }
                }
            }
            else if (sel == 5) {
                show_recent_logs(con.get(), 20);
                cout << "Press any key to continue...\n";
                getch_portable();
            }
            else {
                color_print_warn("Unknown selection\n");
            }

            // small pause
            this_thread::sleep_for(chrono::milliseconds(200));
        }

        color_print_info("\nExiting. Goodbye.\n");
    }
    catch (sql::SQLException& e) {
        color_print_fail(string("[DB ERROR] ") + e.what() + "\n");
        return 1;
    }
    catch (exception& ex) {
        color_print_fail(string("[ERROR] ") + ex.what() + "\n");
        return 1;
    }

    curl_global_cleanup();
    return 0;
}

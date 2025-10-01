#include <iostream>
#include <fstream>
#include <string>
#include <sstream>     // <-- fix istringstream
#include <thread>
#include <chrono>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif


#ifdef _WIN32
#include <windows.h>
#endif

#include <cppconn/driver.h>
#include <cppconn/connection.h>
#include <cppconn/prepared_statement.h>
#include <mysql_driver.h>

using namespace std;

// ===== Console Colors =====
#ifdef _WIN32
void setColor(int color) { SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color); }
void resetColor() { SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 7); }
#else
void setColor(int color) {}
void resetColor() {}
#endif

// ===== Banner =====
void printBanner() {
    setColor(11);
    cout << R"(

    ___    ____   ______  __  __  __  _______
   /   |  / __ \ / ____/ |  \/  | \ \/ / ____|
  / /| | / / / // /      | \  / |  \  /| |
 / ___ |/ /_/ // /___    | |\/| |  /  \| |___
/_/  |_|\____/ \____/    |_|  |_| /_/\_\\_____| 

        ANDROID FLASH TOOL XT

)";
    resetColor();
}

// ===== Generic Progress Bar =====
void showProgressBar(const string& task, int duration) {
    cout << task << endl;
    cout << "[";
    int barWidth = 40;
    for (int i = 0; i < barWidth; i++) cout << " ";
    cout << "]\r[";

    for (int i = 0; i < barWidth; i++) {
        this_thread::sleep_for(chrono::milliseconds(duration / barWidth));
        cout << "#";
        cout.flush();
    }
    cout << "] 100%\n";
}

// ===== Run shell command and capture output =====
string runCommand(const string& cmd) {
    char buffer[256];
    string result;
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    _pclose(pipe);
    if (!result.empty())
        result.erase(result.find_last_not_of(" \n\r\t") + 1);
    return result;
}

// ===== Detect device =====
bool detectDevice(string& serial) {
    string devicesOutput = runCommand("adb devices");
    istringstream ss(devicesOutput);
    string line;
    while (getline(ss, line)) {
        line.erase(line.find_last_not_of(" \n\r\t") + 1);
        if (line.find("\tdevice") != string::npos) {
            serial = line.substr(0, line.find("\tdevice"));
            return true;
        }
    }
    return false;
}

// ===== Fetch Android property =====
string getProp(const string& prop) {
    return runCommand("adb shell getprop " + prop);
}

// ===== Save to MySQL =====
void saveToDatabase(const string& serial, const string& model, const string& brand,
    const string& device, const string& androidV, const string& sdk) {
    try {
        sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
        sql::Connection* con = driver->connect("tcp://localhost:3306", "root", "mapcgx678"); // <-- set your MySQL password
        con->setSchema("pixel_db");

        sql::PreparedStatement* pstmt = con->prepareStatement(
            "INSERT INTO devices (serial, model, brand, device, android_version, sdk_version) VALUES (?, ?, ?, ?, ?, ?)"
        );

        pstmt->setString(1, serial);
        pstmt->setString(2, model);
        pstmt->setString(3, brand);
        pstmt->setString(4, device);
        pstmt->setString(5, androidV);
        pstmt->setString(6, sdk);

        pstmt->execute();
        delete pstmt;
        delete con;

        setColor(10);
        cout << "[OK] Device info saved to MySQL database.\n";
        resetColor();
    }
    catch (const sql::SQLException& e) {
        setColor(12);
        cerr << "[FAIL] SQL Error: " << e.what() << "\n";
        resetColor();
    }
}

// ===== Save to details.txt =====
void saveToTextFile(const string& serial, const string& model, const string& brand,
    const string& device, const string& androidV, const string& sdk) {
    ofstream file("details.txt");
    if (file.is_open()) {
        file << "Serial: " << serial << "\n";
        file << "Model: " << model << "\n";
        file << "Brand: " << brand << "\n";
        file << "Device: " << device << "\n";
        file << "Android Version: " << androidV << "\n";
        file << "SDK Version: " << sdk << "\n";
        file.close();
        setColor(10);
        cout << "[OK] Device info saved to details.txt\n";
        resetColor();
    }
    else {
        setColor(12);
        cerr << "[FAIL] Unable to open details.txt for writing\n";
        resetColor();
    }
}

int main() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif

    printBanner();

    string serial;
    showProgressBar("[Step 1] Detecting Pixel Device", 2000);

    if (!detectDevice(serial)) {
        setColor(12);
        cout << "[FAIL] No Pixel device detected. Make sure USB Debugging is enabled.\n";
        resetColor();
        cout << "\nPress any key to exit...";
        cin.get();
        return 0;
    }

    setColor(10);
    cout << "[OK] Pixel device detected!\n";
    cout << " Serial: " << serial << "\n";
    resetColor();

    // Fetch each property with progress bar
    showProgressBar("[Step 2] Fetching Model", 800);
    string model = getProp("ro.product.model");

    showProgressBar("[Step 3] Fetching Brand", 800);
    string brand = getProp("ro.product.brand");

    showProgressBar("[Step 4] Fetching Device", 800);
    string device = getProp("ro.product.device");

    showProgressBar("[Step 5] Fetching Android Version", 800);
    string androidV = getProp("ro.build.version.release");

    showProgressBar("[Step 6] Fetching SDK Version", 800);
    string sdk = getProp("ro.build.version.sdk");

    // Save info
    showProgressBar("[Step 7] Saving to MySQL Database", 1200);
    saveToDatabase(serial, model, brand, device, androidV, sdk);

    showProgressBar("[Step 8] Saving to details.txt", 800);
    saveToTextFile(serial, model, brand, device, androidV, sdk);

    // ===== Display all details on console =====
    setColor(14); // Yellow
    cout << "\n========== Pixel Device Details ==========\n";
    cout << "Serial Number      : " << serial << "\n";
    cout << "Model              : " << model << "\n";
    cout << "Brand              : " << brand << "\n";
    cout << "Device             : " << device << "\n";
    cout << "Android Version    : " << androidV << "\n";
    cout << "SDK Version        : " << sdk << "\n";
    cout << "=========================================\n";
    resetColor();

    setColor(11);
    cout << "\nAll steps completed successfully!\n";
    resetColor();

    cout << "\nPress any key to exit...";
    cin.get();
    return 0;
}
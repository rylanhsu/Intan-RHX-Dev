#include <windows.h>

#include <filesystem>

#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/settings.h"


// Directory containing XDAQ-RHX.exe. The handler must be located relative to
// the executable, not the current working directory, which differs depending
// on how the app is launched (shortcut, Explorer, command line).
inline std::filesystem::path get_executable_dir()
{
    wchar_t buf[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return std::filesystem::current_path();
    return std::filesystem::path(buf).parent_path();
}

inline std::filesystem::path get_app_data_dir()
{
    const char *userprofile = std::getenv("USERPROFILE");
    std::filesystem::path dir = userprofile
                                    ? std::filesystem::path(userprofile) / "Documents" / "XDAQ-RHX"
                                    : std::filesystem::path("XDAQ-RHX");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}
void InitializeCrashpad()
{
    base::FilePath handler_path((get_executable_dir() / L"crashpad_handler.exe").wstring());

    base::FilePath db_path(get_app_data_dir() / "CrashDB");

    std::map<std::string, std::string> annotations;
    annotations["version"] = "1.3.4";
    annotations["enviroment"] = "production";

    // Empty URL: run local-only, storing minidumps in db_path without uploading.
    const std::string url;

    crashpad::CrashpadClient client;
    bool success = client.StartHandler(
        handler_path, db_path, db_path, url, annotations, {"--no-rate-limit"}, true, true
    );
}
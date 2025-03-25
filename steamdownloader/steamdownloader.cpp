#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <regex>
#include <wininet.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_set>

#pragma comment(lib, "wininet.lib")

namespace fs = std::filesystem;

const std::wstring STEAMCMD_ZIP = L"steamcmd.zip";
const std::wstring STEAMCMD_EXE = L"steamcmd.exe";
const std::wstring STEAMCMD_URL = L"https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip";
const int MAX_RETRY_ATTEMPTS = 5;

std::wstring GetExePath() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    return fs::path(buffer).parent_path().wstring();
}

bool DownloadFileWithProgress(const std::wstring& url, const std::wstring& outputPath) {
    HINTERNET hInternet = InternetOpenW(L"SteamWorkshopDL", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) return false;

    HINTERNET hUrl = InternetOpenUrlW(hInternet, url.c_str(), NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return false;
    }

    DWORD fileSize = 0;
    DWORD bufferSize = sizeof(fileSize);
    HttpQueryInfoW(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &fileSize, &bufferSize, NULL);

    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open()) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        return false;
    }

    char buffer[4096];
    DWORD bytesRead = 0;
    DWORD totalRead = 0;
    auto startTime = std::chrono::steady_clock::now();

    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        file.write(buffer, bytesRead);
        totalRead += bytesRead;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count() > 500) {
            std::wcout << L"\rЗагрузка SteamCMD: " << totalRead / 1024 << "KB / "
                << (fileSize / 1024) << "KB ("
                << (totalRead * 100 / fileSize) << "%)";
            startTime = now;
        }
    }

    file.close();
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);

    if (fs::exists(outputPath)) {
        if (fileSize == 0 || fs::file_size(outputPath) == fileSize) {
            return true;
        }
    }
    fs::remove(outputPath);
        return false;
}

bool UnzipFile(const std::wstring& zipPath, const std::wstring& outputDir) {
    std::wstring command = L"powershell -Command \"Expand-Archive -Path '" + zipPath
        + L"' -DestinationPath '" + outputDir + L"' -Force\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    if (!CreateProcessW(NULL, (LPWSTR)command.c_str(), NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitCode == 0;
}

void DownloadAndExtractSteamCMD() {
    const std::wstring exePath = GetExePath();
    const std::wstring zipPath = exePath + L"\\" + STEAMCMD_ZIP;
    const std::wstring steamcmdPath = exePath + L"\\" + STEAMCMD_EXE;

    if (!fs::exists(steamcmdPath)) {
        for (int attempt = 1; attempt <= 3; attempt++) {
            std::wcout << L"Попытка загрузки SteamCMD (" << attempt << "/3)...\n";
            if (DownloadFileWithProgress(STEAMCMD_URL, zipPath)) {
                std::wcout << L"\nРаспаковка архива...\n";
                if (UnzipFile(zipPath, exePath) && fs::exists(steamcmdPath)) {
                    fs::remove(zipPath);
                    std::wcout << L"SteamCMD успешно установлен\n";
                    return;
                }
                fs::remove(zipPath);
            }
            Sleep(5000 * attempt);
        }
        throw std::runtime_error("Не удалось установить SteamCMD");
    }
}

struct SteamCMDResult {
    bool success;
    std::string output;
};

SteamCMDResult RunSteamCMDWithTimeout(const std::string& command, int timeoutSeconds) {
    SECURITY_ATTRIBUTES saAttr = { sizeof(saAttr) };
    saAttr.bInheritHandle = TRUE;

    HANDLE hStdoutRd, hStdoutWr;
    CreatePipe(&hStdoutRd, &hStdoutWr, &saAttr, 0);
    SetHandleInformation(hStdoutRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hStdoutWr;
    si.hStdError = hStdoutWr;

    PROCESS_INFORMATION pi;
    SteamCMDResult result{ false, "" };

    std::wstring exeDir = GetExePath();
    std::string currentDir(exeDir.begin(), exeDir.end());

    if (CreateProcessA(NULL, (LPSTR)command.c_str(), NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, currentDir.c_str(), &si, &pi)) {
        CloseHandle(hStdoutWr);

        DWORD timeoutMs = (timeoutSeconds <= 0) ? INFINITE : (timeoutSeconds * 1000);
        DWORD waitResult = WaitForSingleObject(pi.hProcess, timeoutMs);

        char buffer[4096];
        DWORD bytesRead;
        while (true) {
            if (!ReadFile(hStdoutRd, buffer, sizeof(buffer) - 1, &bytesRead, NULL) || bytesRead == 0) {
                if (GetLastError() == ERROR_BROKEN_PIPE) break;
                else break;
            }
            buffer[bytesRead] = '\0';
            std::cout << buffer;
            result.output += buffer;
        }

        if (waitResult == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            result.output += "\nТаймаут выполнения";
        }
        else {
            DWORD exitCode;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            result.success = (exitCode == 0);
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    CloseHandle(hStdoutRd);
    return result;
}

void InitializeSteamCMD() {
    const std::wstring steamcmdPath = GetExePath() + L"\\" + STEAMCMD_EXE;
    const std::string initCommand = "\"" + std::string(steamcmdPath.begin(), steamcmdPath.end()) + "\" +login anonymous +quit";

    std::cout << "Инициализация SteamCMD...\n";
    auto result = RunSteamCMDWithTimeout(initCommand, 0);

    if (!result.success) {
        throw std::runtime_error("Ошибка инициализации SteamCMD: " + result.output);
    }
}

void DownloadMod(const std::string& appId, const std::string& modId) {
    const std::wstring steamcmdPath = GetExePath() + L"\\" + STEAMCMD_EXE;
    const std::string command = "\"" + std::string(steamcmdPath.begin(), steamcmdPath.end()) + "\" "
        "+login anonymous "
        "+workshop_download_item " + appId + " " + modId + " "
        "+quit";

    for (int attempt = 1; attempt <= MAX_RETRY_ATTEMPTS; attempt++) {
        std::cout << "[Попытка " << attempt << "/" << MAX_RETRY_ATTEMPTS << "] "
            << "Мод " << modId << std::endl;

        auto result = RunSteamCMDWithTimeout(command, 600);

        if (result.success && result.output.find("Success. Downloaded item") != std::string::npos) {
            std::cout << "├── [УСПЕХ] Мод успешно загружен\n";

            fs::path modPath = fs::path(GetExePath()) / L"steamapps" / L"workshop" / L"content" / appId / modId;
            if (fs::exists(modPath)) {
                return;
            }
            std::cerr << "├── [ПРЕДУПРЕЖДЕНИЕ] Директория мода не найдена\n";
        }

        std::cerr << "├── [ОШИБКА] " << result.output.substr(0, 200) << "\n";

            if (attempt < MAX_RETRY_ATTEMPTS) {
                const int delay = 10000 * attempt;
                std::cout << "└── Повтор через " << delay / 1000 << " сек...\n\n";
                Sleep(delay);
            }
    }

    throw std::runtime_error("Не удалось загрузить мод " + modId);
}

std::string DownloadPage(const std::wstring& url) {
    HINTERNET hInternet = InternetOpenW(L"SteamWorkshopDL", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) throw std::runtime_error("Ошибка инициализации сети");

    HINTERNET hUrl = InternetOpenUrlW(hInternet, url.c_str(), NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        throw std::runtime_error("Ошибка открытия URL");
    }

    std::string content;
    char buffer[4096];
    DWORD bytesRead = 0;

    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        content.append(buffer, bytesRead);
    }

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);
    return content;
}

std::vector<std::string> GetModIds(const std::string& html, const std::string& collectionId) {
    std::unordered_set<std::string> ids;
    std::regex pattern(R"(filedetails/\?id=(\d+))");

    for (std::sregex_iterator it(html.begin(), html.end(), pattern), end; it != end; ++it) {
        std::string foundId = (*it)[1].str();
        if (foundId != collectionId) {
            ids.insert(foundId);
        }
    }

    return { ids.begin(), ids.end() };
}

std::string GetAppIdFromHtml(const std::string& html) {
    std::regex appIdPattern(R"(https://steamcommunity.com/app/(\d+))");
    std::smatch match;
    if (std::regex_search(html, match, appIdPattern) && match.size() > 1) {
        return match[1].str();
    }
    throw std::runtime_error("App ID не найден");
}


int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    try {
        DownloadAndExtractSteamCMD();
        InitializeSteamCMD();

        std::string inputId;
        std::cout << "Введите ID мода или коллекции: ";
        std::cin >> inputId;

        auto html = DownloadPage(L"https://steamcommunity.com/sharedfiles/filedetails/?id=" +
            std::wstring(inputId.begin(), inputId.end()));
        std::string appId = GetAppIdFromHtml(html);

        std::vector<std::string> modIds;
        if (html.find("collectionItemDetails") != std::string::npos) {
            modIds = GetModIds(html, inputId);
            std::cout << "\nНайдено модов в коллекции: " << modIds.size() << "\n";
        }
        else {
            modIds.push_back(inputId);
        }

        std::cout << "\nНачало загрузки...\n";
        for (const auto& modId : modIds) {
            try {
                DownloadMod(appId, modId);
            }
            catch (const std::exception& e) {
                std::cerr << "\n[ОШИБКА] " << e.what() << "\n";
            }
        }

        std::cout << "\nЗавершено. Нажмите Enter для выхода...";
        std::cin.ignore();
        std::cin.get();
    }
    catch (const std::exception& e) {
        std::cerr << "\n[ФАТАЛЬНАЯ ОШИБКА] " << e.what() << "\n";
        return 1;
    }
    return 0;
  
}
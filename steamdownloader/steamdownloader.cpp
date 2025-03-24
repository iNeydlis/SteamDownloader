#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <regex>
#include <wininet.h>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <atomic>
#include <queue>
#include "steamcmd_data.h"

#pragma comment(lib, "wininet.lib")

namespace fs = std::filesystem;

extern "C" const BYTE steamcmd_data[];
extern "C" const DWORD steamcmd_size;

const std::wstring STEAMCMD_EXE = L"steamcmd.exe";
const int MAX_PARALLEL_DOWNLOADS = 5;     // Уменьшено для стабильности до 5
const int MAX_RETRY_ATTEMPTS = 10;
const int FILE_OPS_DELAY_MS = 2000;

std::mutex g_consoleMutex;
std::mutex g_fileSysMutex;
std::atomic<int> g_activeDownloads(0);
std::atomic<bool> g_stopFlag(false);

std::wstring GetExePath() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    return fs::path(buffer).parent_path().wstring();
}

void CreateDirectorySafe(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_fileSysMutex);
    if (!fs::exists(path)) {
        fs::create_directories(path);
    }
}

std::string DownloadPage(const std::wstring& url) {
    HINTERNET hInternet = InternetOpenW(L"SteamWorkshopDL", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) throw std::runtime_error("InternetOpen failed");

    HINTERNET hUrl = InternetOpenUrlW(hInternet, url.c_str(), NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        throw std::runtime_error("InternetOpenUrl failed");
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

    std::cout << "Filtered " << ids.size() << " mods (excluding collection)\n";
    return { ids.begin(), ids.end() };
}

void ExtractSteamCMD() {
    const std::wstring exePath = GetExePath();
    const std::wstring steamcmdPath = exePath + L"\\" + STEAMCMD_EXE;

    std::lock_guard<std::mutex> lock(g_fileSysMutex);
    if (!fs::exists(steamcmdPath)) {
        std::ofstream(steamcmdPath, std::ios::binary).write(
            reinterpret_cast<const char*>(steamcmd_data),
            steamcmd_size
        );
    }
}
bool HasFilesRecursive(const fs::path& path) {
    try {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_regular_file()) {
                // Нашли файл — значит, директория не пуста
                return true;
            }
            else if (entry.is_directory()) {
                // Если это поддиректория, проверяем её рекурсивно
                if (HasFilesRecursive(entry.path())) {
                    return true;
                }
            }
        }
        // Если ничего не нашли, возвращаем false
        return false;
    }
    catch (const fs::filesystem_error& e) {
        // Если возникла ошибка (например, нет доступа), считаем директорию пустой
        std::cerr << "Ошибка файловой системы: " << e.what() << std::endl;
        return false;
    }
}
bool ValidateModFolder(const fs::path& modPath) {
    std::lock_guard<std::mutex> lock(g_fileSysMutex); // Блокировка для потокобезопасности
    if (!fs::exists(modPath)) {
        return false; // Директория не существует
    }
    return HasFilesRecursive(modPath); // Проверяем наличие файлов
}

void DownloadMod(const std::string& appId, const std::string& modId) {
    const std::wstring steamcmdPath = GetExePath() + L"\\" + STEAMCMD_EXE;
    const fs::path modPath = fs::path(GetExePath()) / L"steamapps" / L"workshop" / L"content" / appId / modId;

    for (int attempt = 1; attempt <= MAX_RETRY_ATTEMPTS; ++attempt) {
        if (g_stopFlag) return;

        {
            std::lock_guard<std::mutex> lock(g_fileSysMutex);
            if (fs::exists(modPath)) {
                fs::remove_all(modPath);
            }
        }

        std::string cmd = "\"" + std::string(steamcmdPath.begin(), steamcmdPath.end()) + "\" " +
            "+login anonymous " +
            "+workshop_download_item " + appId + " " + modId + " " +
            "+quit";

        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        DWORD exitCode = 1;

        if (CreateProcessA(NULL, cmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }

        bool downloadSuccess = false;
        for (int validateAttempt = 0; validateAttempt < 3; ++validateAttempt) {
            if (exitCode == 0 && ValidateModFolder(modPath)) {
                downloadSuccess = true;
                break;
            }
            if (validateAttempt < 2) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }

        if (downloadSuccess) {
            std::lock_guard<std::mutex> lock(g_consoleMutex);
            std::cout << "[OK] " << modId << std::endl;
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_consoleMutex);
            std::cerr << "[Retry " << attempt << "/" << MAX_RETRY_ATTEMPTS << "] " << modId << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(attempt * 2));
    }

    std::lock_guard<std::mutex> lock(g_consoleMutex);
    std::cerr << "[FAILED] " << modId << std::endl;
    g_stopFlag = true;
}

void DownloadManager(const std::string& appId, std::queue<std::string>& modQueue) {
    while (!modQueue.empty() && !g_stopFlag) {
        if (g_activeDownloads >= MAX_PARALLEL_DOWNLOADS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        g_activeDownloads++;
        std::string modId = modQueue.front();
        modQueue.pop();
        std::thread([appId, modId]() {
            try {
                DownloadMod(appId, modId);
            }
            catch (...) {
                std::lock_guard<std::mutex> lock(g_consoleMutex);
                std::cerr << "[ERROR] Unhandled exception in download thread" << std::endl;
            }
            g_activeDownloads--;
            }).detach();
    }
}
std::string GetAppIdFromHtml(const std::string& html) {
    std::regex appIdPattern(R"(https://steamcommunity.com/app/(\d+))");
    std::smatch match;
    if (std::regex_search(html, match, appIdPattern) && match.size() > 1) {
        return match[1].str(); // Возвращаем App ID
    }
    throw std::runtime_error("Не удалось найти App ID на странице коллекции");
}

std::string GetPageType(const std::string& html) {
    if (html.find("collectionItemDetails") != std::string::npos) {
        return "collection";
    }
    else if (html.find("workshopItem") != std::string::npos) {
        return "mod";
    }
    throw std::runtime_error("Не удалось определить тип страницы");
}
int main() {
    SetConsoleOutputCP(1251);

    try {
        ExtractSteamCMD();
        const std::wstring exePath = GetExePath();

        std::string inputId;
        std::cout << "Введите ID мода или коллекции: ";
        std::cin >> inputId;

        // Загрузка HTML страницы
        const auto html = DownloadPage(
            L"https://steamcommunity.com/sharedfiles/filedetails/?id=" +
            std::wstring(inputId.begin(), inputId.end())
        );

        // Определение типа страницы
        std::string pageType = GetPageType(html);

        // Автоматическое определение App ID
        std::string appId = GetAppIdFromHtml(html);
        std::cout << "Обнаруженный App ID: " << appId << std::endl;

        // Создание очереди для модов
        std::queue<std::string> modQueue;

        if (pageType == "mod") {
            modQueue.push(inputId);
            std::cout << "\n=== Скачивание одиночного мода ===\n";
        }
        else if (pageType == "collection") {
            auto modIds = GetModIds(html, inputId);
            if (modIds.empty()) {
                std::cout << "В коллекции не найдено модов" << std::endl;
                return 0;
            }
            for (const auto& id : modIds) {
                modQueue.push(id);
            }
            std::cout << "\n=== Скачивание коллекции ===\n";
        }
        else {
            throw std::runtime_error("Неизвестный тип страницы");
        }

        // Инициализация SteamCMD
        std::cout << "\n=== Инициализация SteamCMD ===\n";
        if (!modQueue.empty()) {
            std::string firstMod = modQueue.front();
            modQueue.pop();
            DownloadMod(appId, firstMod);
            std::cout << "Завершение инициализации...\n";
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }

        // Запуск параллельного скачивания
        std::cout << "\n=== Запуск параллельного скачивания ===\n";
        DownloadManager(appId, modQueue);

        // Ожидание завершения
        while (g_activeDownloads > 0 && !g_stopFlag) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (g_stopFlag) {
            std::cerr << "\nПроцесс скачивания прерван!" << std::endl;
            return 1;
        }

        std::cout << "\n=== Все загрузки завершены! ===\n";
        std::wcout << L"Расположение модов: " << exePath << L"\\steamapps\\workshop\\content\\" << appId.c_str() << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "\nОШИБКА: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
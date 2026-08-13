// bot_auto_find.cpp
// Бот с автоматическим поиском адресов через паттерны

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <thread>
#include <random>
#include <string>
#include <algorithm>

struct Vector3 {
    float x, y, z;
};

struct Marker {
    Vector3 pos;
    int type;
    bool active;
};

struct Obstacle {
    Vector3 pos;
    float radius;
};

class AutoMemoryBot {
private:
    HANDLE processHandle;
    DWORD processId;
    DWORD baseAddress;
    DWORD playerAddress;
    DWORD markerPoolAddress;
    
    std::vector<Marker> markers;
    std::vector<Obstacle> obstacles;
    
    bool active;
    bool carryingBox;
    bool learningMode;
    bool emergency;
    bool addressesFound;
    bool botRunning;

public:
    AutoMemoryBot() : processHandle(NULL), processId(0), baseAddress(0),
                      playerAddress(0), markerPoolAddress(0),
                      active(false), carryingBox(false), 
                      learningMode(false), emergency(false), 
                      addressesFound(false), botRunning(false) {
        FindMTAProcess();
        if (processHandle) {
            AutoFindAddresses();
        }
    }

    ~AutoMemoryBot() {
        if (processHandle) {
            CloseHandle(processHandle);
        }
    }

    void FindMTAProcess() {
        // Поиск процесса MTA через окно
        HWND hwnd = FindWindowW(L"GTA:SA", NULL);
        if (!hwnd) {
            hwnd = FindWindowW(NULL, L"MTA: Province");
        }
        if (!hwnd) {
            hwnd = FindWindowW(NULL, L"MTA: San Andreas");
        }
        
        if (hwnd) {
            GetWindowThreadProcessId(hwnd, &processId);
            if (processId) {
                processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
                std::cout << "Найден процесс через окно. PID: " << processId << std::endl;
                return;
            }
        }
        
        // Поиск по имени процесса
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            std::cout << "Ошибка создания снимка процессов" << std::endl;
            return;
        }
        
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);
        
        if (Process32FirstW(snapshot, &pe32)) {
            do {
                std::wstring name(pe32.szExeFile);
                std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                
                if (name.find(L"gta_sa") != std::wstring::npos ||
                    name.find(L"gta") != std::wstring::npos ||
                    name.find(L"mta") != std::wstring::npos) {
                    processId = pe32.th32ProcessID;
                    processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
                    std::cout << "Найден процесс через имя. PID: " << processId << std::endl;
                    break;
                }
            } while (Process32NextW(snapshot, &pe32));
        }
        CloseHandle(snapshot);
    }

    DWORD FindPattern(BYTE* pattern, size_t patternSize, DWORD startOffset = 0, DWORD searchSize = 0) {
        if (!processHandle) return 0;
        
        // Получаем базовый адрес модуля
        HMODULE hMods[1024];
        DWORD cbNeeded;
        if (!EnumProcessModules(processHandle, hMods, sizeof(hMods), &cbNeeded)) {
            return 0;
        }
        
        HMODULE hModule = hMods[0];
        MODULEINFO moduleInfo;
        if (!GetModuleInformation(processHandle, hModule, &moduleInfo, sizeof(moduleInfo))) {
            return 0;
        }
        
        baseAddress = (DWORD)moduleInfo.lpBaseOfDll;
        DWORD moduleSize = searchSize > 0 ? searchSize : moduleInfo.SizeOfImage;
        
        // Проверяем доступность памяти
        std::vector<BYTE> buffer(moduleSize);
        SIZE_T bytesRead;
        
        if (!ReadProcessMemory(processHandle, (LPCVOID)(baseAddress + startOffset), 
                               buffer.data(), moduleSize, &bytesRead)) {
            std::cout << "Ошибка чтения памяти" << std::endl;
            return 0;
        }
        
        // Поиск паттерна
        for (DWORD i = 0; i < bytesRead - patternSize; i++) {
            bool found = true;
            for (size_t j = 0; j < patternSize; j++) {
                if (buffer[i + j] != pattern[j]) {
                    found = false;
                    break;
                }
            }
            if (found) {
                return baseAddress + startOffset + i;
            }
        }
        
        return 0;
    }

    void AutoFindAddresses() {
        std::cout << "Автоматический поиск адресов..." << std::endl;
        
        // Паттерн для позиции игрока (GTA SA)
        BYTE playerPattern[] = {0x8B, 0x0D, 0xF0, 0xF5, 0xB6, 0x00};
        playerAddress = FindPattern(playerPattern, sizeof(playerPattern));
        
        if (playerAddress) {
            std::cout << "Адрес игрока найден: 0x" << std::hex << playerAddress << std::endl;
        } else {
            // Альтернативный паттерн
            BYTE altPattern[] = {0xA1, 0xF0, 0xF5, 0xB6, 0x00};
            playerAddress = FindPattern(altPattern, sizeof(altPattern));
            if (playerAddress) {
                std::cout << "Адрес игрока найден (альт): 0x" << std::hex << playerAddress << std::endl;
            }
        }
        
        // Поиск пула маркеров
        BYTE markerPattern[] = {0x58, 0xDD, 0xC7, 0x00};
        markerPoolAddress = FindPattern(markerPattern, sizeof(markerPattern));
        
        if (markerPoolAddress) {
            std::cout << "Пул маркеров найден: 0x" << std::hex << markerPoolAddress << std::endl;
        }
        
        addressesFound = (playerAddress != 0);
        if (!addressesFound) {
            std::cout << "Не удалось найти адреса! Попробуйте обновить паттерны." << std::endl;
        }
    }

    template<typename T>
    T ReadMemory(DWORD address) {
        T value = {};
        if (processHandle && address) {
            SIZE_T bytesRead;
            ReadProcessMemory(processHandle, (LPCVOID)address, &value, sizeof(T), &bytesRead);
        }
        return value;
    }

    template<typename T>
    void WriteMemory(DWORD address, T value) {
        if (processHandle && address) {
            SIZE_T bytesWritten;
            WriteProcessMemory(processHandle, (LPVOID)address, &value, sizeof(T), &bytesWritten);
        }
    }

    Vector3 GetPlayerPosition() {
        Vector3 pos = {0, 0, 0};
        if (playerAddress && processHandle) {
            DWORD pedAddr = ReadMemory<DWORD>(playerAddress);
            if (pedAddr) {
                pos.x = ReadMemory<float>(pedAddr + 0x14);
                pos.y = ReadMemory<float>(pedAddr + 0x18);
                pos.z = ReadMemory<float>(pedAddr + 0x1C);
            }
        }
        return pos;
    }

    void SetPlayerPosition(Vector3 pos) {
        if (playerAddress && processHandle) {
            DWORD pedAddr = ReadMemory<DWORD>(playerAddress);
            if (pedAddr) {
                WriteMemory<float>(pedAddr + 0x14, pos.x);
                WriteMemory<float>(pedAddr + 0x18, pos.y);
                WriteMemory<float>(pedAddr + 0x1C, pos.z);
            }
        }
    }

    void SetPlayerAngle(float angle) {
        if (playerAddress && processHandle) {
            DWORD pedAddr = ReadMemory<DWORD>(playerAddress);
            if (pedAddr) {
                WriteMemory<float>(pedAddr + 0x20, angle);
            }
        }
    }

    void ScanForMarkers() {
        markers.clear();
        
        if (markerPoolAddress && processHandle) {
            for (int i = 0; i < 32; i++) {
                DWORD addr = markerPoolAddress + i * 0x28;
                
                Marker marker;
                marker.pos.x = ReadMemory<float>(addr);
                marker.pos.y = ReadMemory<float>(addr + 0x04);
                marker.pos.z = ReadMemory<float>(addr + 0x08);
                marker.type = ReadMemory<int>(addr + 0x0C);
                marker.active = ReadMemory<bool>(addr + 0x10);
                
                if (marker.active && marker.pos.x != 0 && marker.pos.y != 0) {
                    if (marker.pos.x > -10000 && marker.pos.x < 10000 &&
                        marker.pos.y > -10000 && marker.pos.y < 10000) {
                        markers.push_back(marker);
                    }
                }
            }
        }
        
        if (markers.empty()) {
            std::cout << "Маркеры не найдены. Попробуйте обновить паттерны." << std::endl;
        }
    }

    DWORD FindMarkerColor(float x, float y) {
        // Упрощенная версия - в реальном боте нужны конкретные адреса
        return 0;
    }

    Marker* FindNearestMarker(int type) {
        Vector3 playerPos = GetPlayerPosition();
        Marker* nearest = nullptr;
        float nearestDist = 999999.0f;
        
        for (auto& marker : markers) {
            if (marker.type == type && marker.active) {
                float dist = sqrt(pow(marker.pos.x - playerPos.x, 2) + 
                                 pow(marker.pos.y - playerPos.y, 2));
                
                if (dist < nearestDist) {
                    nearest = &marker;
                    nearestDist = dist;
                }
            }
        }
        
        return nearest;
    }

    void DetectObstacles() {
        // Упрощенная версия для обучения
        obstacles.clear();
        Vector3 playerPos = GetPlayerPosition();
        
        // Добавляем несколько тестовых препятствий
        for (int i = 0; i < 5; i++) {
            Obstacle obs;
            obs.pos.x = playerPos.x + (i * 10.0f) + 20.0f;
            obs.pos.y = playerPos.y + 20.0f;
            obs.pos.z = playerPos.z;
            obs.radius = 5.0f;
            obstacles.push_back(obs);
        }
    }

    Obstacle* CheckObstacles() {
        Vector3 playerPos = GetPlayerPosition();
        
        for (auto& obs : obstacles) {
            float dist = sqrt(pow(obs.pos.x - playerPos.x, 2) + 
                             pow(obs.pos.y - playerPos.y, 2));
            
            if (dist < obs.radius + 5) {
                return &obs;
            }
        }
        
        return nullptr;
    }

    void AvoidObstacle(Obstacle* obs) {
        Vector3 playerPos = GetPlayerPosition();
        
        float angle = atan2(playerPos.y - obs->pos.y, playerPos.x - obs->pos.x);
        Vector3 avoidPos;
        avoidPos.x = obs->pos.x + cos(angle) * (obs->radius + 10);
        avoidPos.y = obs->pos.y + sin(angle) * (obs->radius + 10);
        avoidPos.z = playerPos.z;
        
        float targetAngle = atan2(avoidPos.y - playerPos.y, avoidPos.x - playerPos.x);
        SetPlayerAngle(targetAngle * 180.0f / 3.14159f - 90.0f);
        
        // Двигаемся в сторону обхода
        MoveTo(avoidPos, 0.2f);
    }

    void MoveTo(Vector3 target, float speed) {
        Vector3 playerPos = GetPlayerPosition();
        
        float dist = sqrt(pow(target.x - playerPos.x, 2) + 
                         pow(target.y - playerPos.y, 2));
        
        if (dist < 0.5f) return;
        
        float angle = atan2(target.y - playerPos.y, target.x - playerPos.x);
        SetPlayerAngle(angle * 180.0f / 3.14159f - 90.0f);
        
        Vector3 newPos;
        newPos.x = playerPos.x + cos(angle) * speed;
        newPos.y = playerPos.y + sin(angle) * speed;
        newPos.z = playerPos.z;
        
        SetPlayerPosition(newPos);
    }

    void BotLoop() {
        botRunning = true;
        while (botRunning) {
            if (!active || emergency || !addressesFound) {
                Sleep(100);
                continue;
            }
            
            // Автоматическое сканирование маркеров
            ScanForMarkers();
            
            // Обучение
            if (learningMode) {
                DetectObstacles();
            }
            
            // Выбор типа маркера
            int targetType = carryingBox ? 2 : 1;
            
            // Поиск ближайшего маркера
            Marker* target = FindNearestMarker(targetType);
            
            if (target) {
                Vector3 playerPos = GetPlayerPosition();
                float dist = sqrt(pow(target->pos.x - playerPos.x, 2) + 
                                 pow(target->pos.y - playerPos.y, 2));
                
                if (dist < 2.0f) {
                    if (target->type == 1) {
                        carryingBox = true;
                        std::cout << "Взял коробку!" << std::endl;
                    } else {
                        carryingBox = false;
                        std::cout << "Доставил коробку!" << std::endl;
                    }
                } else {
                    Obstacle* obs = CheckObstacles();
                    
                    if (obs) {
                        AvoidObstacle(obs);
                    } else {
                        float speed = carryingBox ? 0.15f : 0.3f;
                        MoveTo(target->pos, speed);
                    }
                }
            } else {
                if (markers.empty()) {
                    std::cout << "Маркеры не обнаружены. Сканирование..." << std::endl;
                }
            }
            
            Sleep(50);
        }
    }

    void Start() {
        if (botRunning) {
            std::cout << "Бот уже запущен!" << std::endl;
            return;
        }
        active = true;
        emergency = false;
        std::cout << "Запуск бота..." << std::endl;
        std::thread(&AutoMemoryBot::BotLoop, this).detach();
    }

    void Stop() { 
        active = false; 
        std::cout << "Бот остановлен." << std::endl;
    }
    
    void EmergencyStop() { 
        emergency = true; 
        active = false;
        botRunning = false;
        std::cout << "ЭКСТРЕННАЯ ОСТАНОВКА!" << std::endl;
    }
    
    void ToggleLearning() { 
        learningMode = !learningMode;
        std::cout << "Режим обучения: " << (learningMode ? "ВКЛ" : "ВЫКЛ") << std::endl;
    }
    
    bool IsReady() { return addressesFound && processHandle != NULL; }
};

int main() {
    SetConsoleCP(1251);
    SetConsoleOutputCP(1251);
    
    std::cout << "=== БОТ ДЛЯ MTA PROVINCE (АВТО ПОИСК) ===" << std::endl;
    std::cout << "Версия 2.0" << std::endl << std::endl;
    
    AutoMemoryBot bot;
    
    if (!bot.IsReady()) {
        std::cout << "Ошибка: MTA Province не найден или не запущен!" << std::endl;
        std::cout << "Убедитесь, что MTA запущен и вы в игре." << std::endl;
        system("pause");
        return 1;
    }
    
    std::cout << "Бот готов к работе!" << std::endl;
    std::cout << std::endl;
    std::cout << "1. Запустить" << std::endl;
    std::cout << "2. Остановить" << std::endl;
    std::cout << "3. Экстренная остановка" << std::endl;
    std::cout << "4. Режим обучения" << std::endl;
    std::cout << "0. Выход" << std::endl;
    std::cout << std::endl;
    
    int choice;
    while (true) {
        std::cout << "Выберите действие: ";
        std::cin >> choice;
        
        switch (choice) {
            case 1: 
                bot.Start(); 
                break;
            case 2: 
                bot.Stop(); 
                break;
            case 3: 
                bot.EmergencyStop(); 
                break;
            case 4: 
                bot.ToggleLearning(); 
                break;
            case 0: 
                bot.EmergencyStop();
                std::cout << "Выход..." << std::endl;
                Sleep(500);
                return 0;
            default:
                std::cout << "Неверный выбор!" << std::endl;
                break;
        }
    }
    
    return 0;
}
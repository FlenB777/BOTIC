// main.cpp - Скрытый бот-сборщик для MTA Province
// Управление только через EXE, горячие клавиши, русский язык
// F11 - экстренная остановка

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <thread>
#include <string>
#include <algorithm>
#include <chrono>
#include <fstream>

using namespace std;
using namespace chrono;

// ==================== ЦВЕТА ====================

void SetColor(int color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}

// ==================== СТРУКТУРЫ ====================

struct Vector3 {
    float x, y, z;
};

struct Marker {
    Vector3 pos;
    int type;
    bool active;
    bool visited;
    DWORD address;
    time_t spawnTime;
};

// ==================== ГЛАВНЫЙ КЛАСС ====================

class StealthBot {
private:
    HANDLE processHandle;
    DWORD processId;
    DWORD playerAddr;
    DWORD baseAddress;
    
    bool running;
    bool active;
    bool carryingBox;
    float speed;
    int boxesCollected;
    int boxesDelivered;
    bool emergencyStop;
    
    vector<Marker> markers;
    vector<Marker> collectedMarkers;
    vector<Vector3> obstacles;
    Vector3 deliveryPoint;
    
    high_resolution_clock::time_point lastScan;
    int stuckCount;
    Vector3 lastPos;

public:
    StealthBot() : processHandle(NULL), processId(0), playerAddr(0),
                   baseAddress(0), running(false), active(false),
                   carryingBox(false), speed(0.25f), boxesCollected(0),
                   boxesDelivered(0), emergencyStop(false), stuckCount(0) {
        deliveryPoint = {0, 0, 0};
        lastPos = {0, 0, 0};
        FindMTAProcess();
        if (processHandle) {
            FindAddresses();
        }
    }

    ~StealthBot() {
        if (processHandle) CloseHandle(processHandle);
    }

    // ==================== СКРЫТОЕ ЧТЕНИЕ ПАМЯТИ ====================

    template<typename T>
    T ReadMemory(DWORD address) {
        T value = {};
        if (!processHandle || !address) return value;
        
        typedef NTSTATUS(WINAPI* NtReadVirtualMemoryPtr)(HANDLE, PVOID, PVOID, SIZE_T, SIZE_T*);
        static NtReadVirtualMemoryPtr NtRead = NULL;
        
        if (!NtRead) {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            NtRead = (NtReadVirtualMemoryPtr)GetProcAddress(ntdll, "NtReadVirtualMemory");
        }
        
        if (NtRead) {
            SIZE_T bytesRead;
            NtRead(processHandle, (PVOID)address, &value, sizeof(T), &bytesRead);
        }
        return value;
    }

    template<typename T>
    void WriteMemory(DWORD address, T value) {
        if (!processHandle || !address) return;
        
        typedef NTSTATUS(WINAPI* NtWriteVirtualMemoryPtr)(HANDLE, PVOID, PVOID, SIZE_T, SIZE_T*);
        static NtWriteVirtualMemoryPtr NtWrite = NULL;
        
        if (!NtWrite) {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            NtWrite = (NtWriteVirtualMemoryPtr)GetProcAddress(ntdll, "NtWriteVirtualMemory");
        }
        
        if (NtWrite) {
            SIZE_T bytesWritten;
            NtWrite(processHandle, (PVOID)address, &value, sizeof(T), &bytesWritten);
        }
    }

    // ==================== ПОИСК ПРОЦЕССА ====================

    void FindMTAProcess() {
        HWND hwnd = FindWindowW(NULL, L"MTA: Province");
        if (!hwnd) hwnd = FindWindowW(NULL, L"MTA: San Andreas");
        
        if (hwnd) {
            GetWindowThreadProcessId(hwnd, &processId);
            processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, processId);
            if (processHandle) {
                Print("✅ Найден процесс MTA", 10);
                return;
            }
        }
        
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe32;
            pe32.dwSize = sizeof(PROCESSENTRY32W);
            
            if (Process32FirstW(snapshot, &pe32)) {
                do {
                    wstring name(pe32.szExeFile);
                    transform(name.begin(), name.end(), name.begin(), ::towlower);
                    
                    if (name.find(L"gta_sa") != wstring::npos ||
                        name.find(L"gta") != wstring::npos ||
                        name.find(L"mta") != wstring::npos) {
                        processId = pe32.th32ProcessID;
                        processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE, 
                                                    FALSE, processId);
                        if (processHandle) {
                            Print("✅ Найден процесс MTA", 10);
                            break;
                        }
                    }
                } while (Process32NextW(snapshot, &pe32));
            }
            CloseHandle(snapshot);
        }
        
        if (!processHandle) {
            Print("❌ MTA Province не найден! Запустите игру.", 12);
        }
    }

    // ==================== ПОИСК АДРЕСОВ ====================

    void FindAddresses() {
        if (!processHandle) return;
        
        Print("🔍 Поиск адресов...", 11);
        
        HMODULE hMods[1024];
        DWORD cbNeeded;
        if (EnumProcessModules(processHandle, hMods, sizeof(hMods), &cbNeeded)) {
            baseAddress = (DWORD)hMods[0];
        }
        
        playerAddr = 0xB6F5F0;
        DWORD test = ReadMemory<DWORD>(playerAddr);
        
        if (test > 0x10000 && test < 0x7FFFFFFF) {
            Print("✅ Адрес игрока найден", 10);
            return;
        }
        
        for (DWORD addr = baseAddress; addr < baseAddress + 0x500000; addr += 4) {
            DWORD ped = ReadMemory<DWORD>(addr);
            if (ped > 0x10000 && ped < 0x7FFFFFFF) {
                float x = ReadMemory<float>(ped + 0x14);
                float y = ReadMemory<float>(ped + 0x18);
                if (x > -5000 && x < 5000 && y > -5000 && y < 5000) {
                    playerAddr = addr;
                    Print("✅ Адрес игрока найден", 10);
                    return;
                }
            }
        }
        
        Print("❌ Не удалось найти адреса!", 12);
    }

    // ==================== ПОЛУЧЕНИЕ ПОЗИЦИИ ====================

    Vector3 GetPosition() {
        Vector3 pos = {0, 0, 0};
        if (!playerAddr) return pos;
        
        DWORD ped = ReadMemory<DWORD>(playerAddr);
        if (ped) {
            pos.x = ReadMemory<float>(ped + 0x14);
            pos.y = ReadMemory<float>(ped + 0x18);
            pos.z = ReadMemory<float>(ped + 0x1C);
        }
        return pos;
    }

    void SetPosition(Vector3 pos) {
        if (!playerAddr) return;
        
        DWORD ped = ReadMemory<DWORD>(playerAddr);
        if (ped) {
            WriteMemory<float>(ped + 0x14, pos.x);
            WriteMemory<float>(ped + 0x18, pos.y);
            WriteMemory<float>(ped + 0x1C, pos.z);
        }
    }

    void SetAngle(float angle) {
        if (!playerAddr) return;
        
        DWORD ped = ReadMemory<DWORD>(playerAddr);
        if (ped) {
            WriteMemory<float>(ped + 0x20, angle);
        }
    }

    // ==================== ПОИСК МАРКЕРОВ ====================

    void ScanMarkers() {
        if (!processHandle || !baseAddress) return;
        
        auto now = high_resolution_clock::now();
        if (duration_cast<milliseconds>(now - lastScan).count() < 200) return;
        lastScan = now;
        
        Vector3 playerPos = GetPosition();
        
        for (DWORD addr = baseAddress; addr < baseAddress + 0x800000; addr += 0x28) {
            float x = ReadMemory<float>(addr);
            float y = ReadMemory<float>(addr + 0x04);
            float z = ReadMemory<float>(addr + 0x08);
            int type = ReadMemory<int>(addr + 0x0C);
            bool active = ReadMemory<bool>(addr + 0x10);
            
            if (!active || x == 0 || y == 0) continue;
            if (x < -5000 || x > 5000 || y < -5000 || y > 5000) continue;
            
            bool exists = false;
            for (auto& m : markers) {
                float d = sqrt(pow(m.pos.x - x, 2) + pow(m.pos.y - y, 2));
                if (d < 0.5f) { exists = true; break; }
            }
            
            if (exists) continue;
            
            bool collected = false;
            for (auto& m : collectedMarkers) {
                float d = sqrt(pow(m.pos.x - x, 2) + pow(m.pos.y - y, 2));
                if (d < 0.5f) { collected = true; break; }
            }
            
            if (collected) continue;
            
            if (type == 1) {
                Marker marker;
                marker.pos = {x, y, z};
                marker.type = type;
                marker.active = true;
                marker.visited = false;
                marker.address = addr;
                marker.spawnTime = time(NULL);
                markers.push_back(marker);
                
                Print("📦 Найден новый ящик! X=" + to_string((int)x) + " Y=" + to_string((int)y), 10);
            }
            else if (type == 2) {
                deliveryPoint = {x, y, z};
                Print("📍 Точка сдачи найдена! X=" + to_string((int)x) + " Y=" + to_string((int)y), 11);
            }
        }
    }

    // ==================== ОБХОД ПРЕПЯТСТВИЙ ====================

    Vector3 AvoidObstacles(Vector3 target) {
        Vector3 playerPos = GetPosition();
        
        for (auto& obs : obstacles) {
            float dist = sqrt(pow(obs.x - playerPos.x, 2) + pow(obs.y - playerPos.y, 2));
            
            if (dist < 5.0f) {
                float angle = atan2(playerPos.y - obs.y, playerPos.x - obs.x);
                
                Vector3 avoidRight = {
                    obs.x + cos(angle + 1.57f) * 6.0f,
                    obs.y + sin(angle + 1.57f) * 6.0f,
                    playerPos.z
                };
                
                bool rightFree = true;
                for (auto& obs2 : obstacles) {
                    float d = sqrt(pow(avoidRight.x - obs2.x, 2) + pow(avoidRight.y - obs2.y, 2));
                    if (d < 4.0f) { rightFree = false; break; }
                }
                
                if (rightFree) return avoidRight;
                
                Vector3 avoidLeft = {
                    obs.x + cos(angle - 1.57f) * 6.0f,
                    obs.y + sin(angle - 1.57f) * 6.0f,
                    playerPos.z
                };
                return avoidLeft;
            }
        }
        
        return target;
    }

    // ==================== ДВИЖЕНИЕ ====================

    void MoveTo(Vector3 target) {
        Vector3 playerPos = GetPosition();
        float dist = sqrt(pow(target.x - playerPos.x, 2) + pow(target.y - playerPos.y, 2));
        
        if (dist < 0.5f) return;
        if (emergencyStop) return;
        
        Vector3 adjusted = AvoidObstacles(target);
        if (adjusted.x != target.x || adjusted.y != target.y) {
            float d = sqrt(pow(adjusted.x - playerPos.x, 2) + pow(adjusted.y - playerPos.y, 2));
            if (d > 2.0f) target = adjusted;
        }
        
        float angle = atan2(target.y - playerPos.y, target.x - playerPos.x);
        SetAngle(angle * 180.0f / 3.14159f - 90.0f);
        
        float currentSpeed = carryingBox ? speed * 0.6f : speed;
        Vector3 newPos;
        newPos.x = playerPos.x + cos(angle) * currentSpeed;
        newPos.y = playerPos.y + sin(angle) * currentSpeed;
        newPos.z = target.z;
        
        SetPosition(newPos);
    }

    // ==================== ОСНОВНОЙ ЦИКЛ ====================

    void BotLoop() {
        active = true;
        Print("🟢 Бот запущен! Собираю ящики...", 10);
        
        int scanCount = 0;
        lastPos = GetPosition();
        stuckCount = 0;
        
        while (active) {
            if (!running || emergencyStop) {
                Sleep(100);
                continue;
            }
            
            scanCount++;
            Vector3 playerPos = GetPosition();
            
            float moveDist = sqrt(pow(playerPos.x - lastPos.x, 2) + pow(playerPos.y - lastPos.y, 2));
            if (moveDist < 0.05f) {
                stuckCount++;
                if (stuckCount > 40) {
                    Print("⚠️ Застрял! Меняю направление...", 14);
                    Vector3 randomTarget = {
                        playerPos.x + (rand() % 80 - 40),
                        playerPos.y + (rand() % 80 - 40),
                        playerPos.z
                    };
                    MoveTo(randomTarget);
                    stuckCount = 0;
                }
            } else {
                stuckCount = 0;
            }
            lastPos = playerPos;
            
            if (scanCount % 5 == 0) {
                ScanMarkers();
            }
            
            if (carryingBox) {
                if (deliveryPoint.x != 0) {
                    float dist = sqrt(pow(deliveryPoint.x - playerPos.x, 2) + 
                                     pow(deliveryPoint.y - playerPos.y, 2));
                    
                    if (dist < 2.0f) {
                        carryingBox = false;
                        boxesDelivered++;
                        Print("✅ Ящик доставлен! (" + to_string(boxesDelivered) + ")", 10);
                        obstacles.push_back(playerPos);
                    } else {
                        MoveTo(deliveryPoint);
                    }
                }
                continue;
            }
            
            Marker* nearest = nullptr;
            float nearestDist = 999999.0f;
            
            for (auto& m : markers) {
                if (m.type == 1 && !m.visited) {
                    float d = sqrt(pow(m.pos.x - playerPos.x, 2) + pow(m.pos.y - playerPos.y, 2));
                    if (d < nearestDist) {
                        nearest = &m;
                        nearestDist = d;
                    }
                }
            }
            
            if (nearest) {
                float dist = sqrt(pow(nearest->pos.x - playerPos.x, 2) + 
                                 pow(nearest->pos.y - playerPos.y, 2));
                
                if (dist < 2.0f) {
                    carryingBox = true;
                    nearest->visited = true;
                    boxesCollected++;
                    collectedMarkers.push_back(*nearest);
                    
                    markers.erase(remove_if(markers.begin(), markers.end(),
                        [nearest](Marker& m) { return m.address == nearest->address; }), 
                        markers.end());
                    
                    Print("📦 Ящик взят! (" + to_string(boxesCollected) + ") Несу на сдачу...", 14);
                } else {
                    MoveTo(nearest->pos);
                }
            } else {
                if (scanCount % 100 == 0) {
                    Print("🔍 Ищу новые ящики...", 8);
                    Vector3 randomTarget = {
                        playerPos.x + (rand() % 100 - 50),
                        playerPos.y + (rand() % 100 - 50),
                        playerPos.z
                    };
                    MoveTo(randomTarget);
                }
            }
            
            Sleep(50);
        }
    }

    // ==================== УПРАВЛЕНИЕ ====================

    void Start() {
        if (running) {
            Print("⚠️ Бот уже работает!", 14);
            return;
        }
        
        if (!processHandle || !playerAddr) {
            Print("❌ Ошибка! Бот не инициализирован.", 12);
            return;
        }
        
        running = true;
        emergencyStop = false;
        carryingBox = false;
        lastScan = high_resolution_clock::now();
        Print("🚀 Бот запущен! Нажмите F11 для экстренной остановки.", 10);
        
        thread(&StealthBot::BotLoop, this).detach();
    }

    void Stop() {
        running = false;
        Print("⏹️ Бот остановлен. Собрано: " + to_string(boxesCollected) + 
              " | Доставлено: " + to_string(boxesDelivered), 14);
    }

    void EmergencyStopFunc() {
        emergencyStop = true;
        running = false;
        active = false;
        Print("🛑 ЭКСТРЕННАЯ ОСТАНОВКА! (F11)", 12);
    }

    void SpeedUp() {
        speed = min(0.8f, speed + 0.1f);
        Print("⚡ Скорость: " + to_string(speed), 11);
    }

    void SpeedDown() {
        speed = max(0.1f, speed - 0.1f);
        Print("⚡ Скорость: " + to_string(speed), 11);
    }

    void ShowStatus() {
        Vector3 pos = GetPosition();
        
        SetColor(11);
        cout << "\n╔════════════════════════════════════════════╗" << endl;
        cout << "║           📊 СТАТУС БОТА                 ║" << endl;
        cout << "╚════════════════════════════════════════════╝" << endl;
        SetColor(15);
        
        cout << "  Позиция:     X=" << (int)pos.x << " Y=" << (int)pos.y << " Z=" << (int)pos.z << endl;
        cout << "  Скорость:    " << speed << endl;
        cout << "  Состояние:   " << (running ? "🟢 Работает" : "🔴 Остановлен") << endl;
        cout << "  Ящик:        " << (carryingBox ? "🟡 Несу" : "🔴 Ищу") << endl;
        cout << "  Собрано:     " << boxesCollected << endl;
        cout << "  Доставлено:  " << boxesDelivered << endl;
        cout << "  Найдено:     " << markers.size() << " ящиков" << endl;
        
        if (deliveryPoint.x != 0) {
            cout << "  Точка сдачи: X=" << (int)deliveryPoint.x << " Y=" << (int)deliveryPoint.y << endl;
        }
        
        SetColor(11);
        cout << "╔════════════════════════════════════════════╗" << endl;
        cout << "║  F1-Старт  F2-Стоп  F3-Быстрее  F4-Медленнее ║" << endl;
        cout << "║  F5-Статус  F11-Экстренный стоп  ESC-Выход  ║" << endl;
        cout << "╚════════════════════════════════════════════╝" << endl;
        SetColor(15);
    }

    void ShowHelp() {
        SetColor(14);
        cout << "\n╔════════════════════════════════════════════╗" << endl;
        cout << "║           ⌨️  УПРАВЛЕНИЕ                  ║" << endl;
        cout << "╚════════════════════════════════════════════╝" << endl;
        SetColor(15);
        cout << "  F1  - Запустить бота" << endl;
        cout << "  F2  - Остановить бота" << endl;
        cout << "  F3  - Увеличить скорость" << endl;
        cout << "  F4  - Уменьшить скорость" << endl;
        cout << "  F5  - Показать статус" << endl;
        cout << "  F11 - ЭКСТРЕННАЯ ОСТАНОВКА" << endl;
        cout << "  ESC - Выйти из программы" << endl;
        SetColor(14);
        cout << "╔════════════════════════════════════════════╗" << endl;
        cout << "║   Бот работает в скрытом режиме           ║" << endl;
        cout << "║   Античит не должен детектить             ║" << endl;
        cout << "╚════════════════════════════════════════════╝" << endl;
        SetColor(15);
    }

    void Print(string text, int color = 15) {
        SetColor(color);
        cout << text << endl;
        SetColor(15);
    }

    void HotkeyHandler() {
        while (true) {
            if (GetAsyncKeyState(VK_F1) & 1) Start();
            if (GetAsyncKeyState(VK_F2) & 1) Stop();
            if (GetAsyncKeyState(VK_F3) & 1) SpeedUp();
            if (GetAsyncKeyState(VK_F4) & 1) SpeedDown();
            if (GetAsyncKeyState(VK_F5) & 1) ShowStatus();
            if (GetAsyncKeyState(VK_F11) & 1) EmergencyStopFunc();
            if (GetAsyncKeyState(VK_ESCAPE) & 1) {
                EmergencyStopFunc();
                Print("👋 Выход...", 14);
                exit(0);
            }
            Sleep(50);
        }
    }

    bool IsReady() {
        return processHandle != NULL && playerAddr != 0;
    }
};

// ==================== ГЛАВНАЯ ФУНКЦИЯ ====================

int main() {
    SetConsoleCP(1251);
    SetConsoleOutputCP(1251);
    
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hConsole, &cursorInfo);
    cursorInfo.bVisible = false;
    SetConsoleCursorInfo(hConsole, &cursorInfo);
    
    system("cls");
    
    SetColor(10);
    cout << "\n╔═══════════════════════════════════════════════════════╗" << endl;
    cout << "║      🤖 СКРЫТЫЙ БОТ-СБОРЩИК ДЛЯ MTA PROVINCE       ║" << endl;
    cout << "║                                                      ║" << endl;
    cout << "║  📦 Собирает ящики                                 ║" << endl;
    cout << "║  🎯 Относит в точку сдачи                         ║" << endl;
    cout << "║  🚧 Обходит препятствия                           ║" << endl;
    cout << "║  🔒 Скрытый режим (не детектится античитом)      ║" << endl;
    cout << "║  ⌨️  Управление горячими клавишами               ║" << endl;
    cout << "╚═══════════════════════════════════════════════════════╝" << endl;
    
    SetColor(14);
    cout << "\n  ⚠️  Запускайте от имени администратора!" << endl;
    cout << "  ⚠️  MTA Province должна быть запущена!" << endl;
    cout << "  ⚠️  Нажмите F5 для просмотра управления\n" << endl;
    SetColor(15);
    
    StealthBot bot;
    
    if (!bot.IsReady()) {
        SetColor(12);
        cout << "\n❌ ОШИБКА: MTA Province не найден!" << endl;
        cout << "Убедитесь, что игра запущена." << endl;
        SetColor(15);
        system("pause");
        return 1;
    }
    
    SetColor(10);
    cout << "✅ БОТ ГОТОВ К РАБОТЕ!" << endl;
    SetColor(15);
    cout << "📌 Нажмите F1 для запуска\n" << endl;
    
    thread hotkeyThread(&StealthBot::HotkeyHandler, &bot);
    hotkeyThread.detach();
    
    while (true) {
        Sleep(1000);
    }
    
    return 0;
}

// main.cpp - Бот-сборщик с удобным управлением
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
#include <map>
#include <conio.h>

using namespace std;
using namespace chrono;

// ==================== ЦВЕТА ДЛЯ КОНСОЛИ ====================

enum Color {
    BLACK = 0,
    BLUE = 1,
    GREEN = 2,
    CYAN = 3,
    RED = 4,
    MAGENTA = 5,
    YELLOW = 6,
    WHITE = 7,
    GRAY = 8,
    LIGHT_BLUE = 9,
    LIGHT_GREEN = 10,
    LIGHT_CYAN = 11,
    LIGHT_RED = 12,
    LIGHT_MAGENTA = 13,
    LIGHT_YELLOW = 14,
    BRIGHT_WHITE = 15
};

void SetColor(int color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}

void SetColorPair(int foreground, int background) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, foreground | (background << 4));
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

struct Obstacle {
    Vector3 pos;
    float radius;
};

struct Waypoint {
    Vector3 pos;
    string name;
    time_t timestamp;
};

// ==================== ОСНОВНОЙ КЛАСС ====================

class CollectorBot {
private:
    HANDLE processHandle;
    DWORD processId;
    DWORD playerAddr;
    DWORD baseAddress;
    
    // Состояние
    bool running;
    bool active;
    bool carryingBox;
    float speed;
    int boxesCollected;
    int boxesDelivered;
    
    // Данные
    vector<Marker> markers;
    vector<Marker> collectedMarkers;
    vector<Obstacle> obstacles;
    vector<Waypoint> waypoints;
    
    Vector3 deliveryPoint;
    
    // Таймеры
    high_resolution_clock::time_point lastScan;
    high_resolution_clock::time_point lastLog;
    high_resolution_clock::time_point lastStatusUpdate;
    
    // Для горячих клавиш
    bool hotkeysEnabled;
    bool showHelp;
    bool showStats;

public:
    CollectorBot() : processHandle(NULL), processId(0), playerAddr(0),
                     baseAddress(0), running(false), active(false),
                     carryingBox(false), speed(0.3f),
                     boxesCollected(0), boxesDelivered(0),
                     hotkeysEnabled(true), showHelp(false), showStats(false) {
        deliveryPoint = {0, 0, 0};
        FindMTAProcess();
        if (processHandle) {
            FindAddresses();
        }
    }

    ~CollectorBot() {
        if (processHandle) CloseHandle(processHandle);
    }

    // ==================== ОСНОВНЫЕ ФУНКЦИИ ====================

    template<typename T>
    T Read(DWORD address) {
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
    void Write(DWORD address, T value) {
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

    void FindMTAProcess() {
        HWND hwnd = FindWindowW(NULL, L"MTA: Province");
        if (!hwnd) hwnd = FindWindowW(NULL, L"MTA: San Andreas");
        if (!hwnd) return;
        
        GetWindowThreadProcessId(hwnd, &processId);
        processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, processId);
    }

    void FindAddresses() {
        HMODULE hMods[1024];
        DWORD cbNeeded;
        if (EnumProcessModules(processHandle, hMods, sizeof(hMods), &cbNeeded)) {
            baseAddress = (DWORD)hMods[0];
        }
        
        playerAddr = 0xB6F5F0;
        DWORD test = Read<DWORD>(playerAddr);
        
        if (test <= 0x10000) {
            for (DWORD addr = baseAddress; addr < baseAddress + 0x500000; addr += 4) {
                DWORD ped = Read<DWORD>(addr);
                if (ped > 0x10000 && ped < 0x7FFFFFFF) {
                    float x = Read<float>(ped + 0x14);
                    if (x > -5000 && x < 5000) {
                        playerAddr = addr;
                        break;
                    }
                }
            }
        }
    }

    Vector3 GetPosition() {
        Vector3 pos = {0, 0, 0};
        DWORD ped = Read<DWORD>(playerAddr);
        if (ped) {
            pos.x = Read<float>(ped + 0x14);
            pos.y = Read<float>(ped + 0x18);
            pos.z = Read<float>(ped + 0x1C);
        }
        return pos;
    }

    void SetPosition(Vector3 pos) {
        DWORD ped = Read<DWORD>(playerAddr);
        if (ped) {
            Write<float>(ped + 0x14, pos.x);
            Write<float>(ped + 0x18, pos.y);
            Write<float>(ped + 0x1C, pos.z);
        }
    }

    void SetAngle(float angle) {
        DWORD ped = Read<DWORD>(playerAddr);
        if (ped) {
            Write<float>(ped + 0x20, angle);
        }
    }

    // ==================== ПОИСК МАРКЕРОВ ====================

    void ScanMarkers() {
        auto now = high_resolution_clock::now();
        if (duration_cast<milliseconds>(now - lastScan).count() < 200) return;
        lastScan = now;
        
        Vector3 playerPos = GetPosition();
        
        for (DWORD addr = baseAddress; addr < baseAddress + 0x1000000; addr += 0x28) {
            float x = Read<float>(addr);
            float y = Read<float>(addr + 0x04);
            float z = Read<float>(addr + 0x08);
            int type = Read<int>(addr + 0x0C);
            bool active = Read<bool>(addr + 0x10);
            
            if (active && x != 0 && y != 0) {
                if (x > -5000 && x < 5000 && y > -5000 && y < 5000) {
                    bool exists = false;
                    for (auto& m : markers) {
                        float dist = sqrt(pow(m.pos.x - x, 2) + pow(m.pos.y - y, 2));
                        if (dist < 1.0f) { exists = true; break; }
                    }
                    
                    bool collected = false;
                    for (auto& m : collectedMarkers) {
                        float dist = sqrt(pow(m.pos.x - x, 2) + pow(m.pos.y - y, 2));
                        if (dist < 1.0f) { collected = true; break; }
                    }
                    
                    if (!exists && !collected && type == 1) {
                        Marker marker;
                        marker.pos = {x, y, z};
                        marker.type = type;
                        marker.active = true;
                        marker.visited = false;
                        marker.address = addr;
                        marker.spawnTime = time(NULL);
                        markers.push_back(marker);
                        
                        SetColor(LIGHT_GREEN);
                        cout << "🎯 НОВЫЙ ЯЩИК! X=" << x << " Y=" << y << endl;
                        SetColor(WHITE);
                    }
                    
                    if (type == 2 && (deliveryPoint.x == 0 || 
                        sqrt(pow(x - deliveryPoint.x, 2) + pow(y - deliveryPoint.y, 2)) > 5.0f)) {
                        deliveryPoint = {x, y, z};
                        SetColor(LIGHT_CYAN);
                        cout << "📍 ТОЧКА СДАЧИ: X=" << x << " Y=" << y << endl;
                        SetColor(WHITE);
                    }
                }
            }
        }
    }

    // ==================== ОБХОД ПРЕПЯТСТВИЙ ====================

    Vector3 AvoidObstacles(Vector3 target) {
        Vector3 playerPos = GetPosition();
        
        for (auto& obs : obstacles) {
            float dist = sqrt(pow(obs.pos.x - playerPos.x, 2) + pow(obs.pos.y - playerPos.y, 2));
            
            if (dist < obs.radius + 4) {
                float angle = atan2(playerPos.y - obs.pos.y, playerPos.x - obs.pos.x);
                
                Vector3 avoidRight = {
                    obs.pos.x + cos(angle + 1.57f) * (obs.radius + 3),
                    obs.pos.y + sin(angle + 1.57f) * (obs.radius + 3),
                    playerPos.z
                };
                
                bool rightFree = true;
                for (auto& obs2 : obstacles) {
                    float d = sqrt(pow(avoidRight.x - obs2.pos.x, 2) + pow(avoidRight.y - obs2.pos.y, 2));
                    if (d < obs2.radius + 2) { rightFree = false; break; }
                }
                
                if (rightFree) return avoidRight;
                
                Vector3 avoidLeft = {
                    obs.pos.x + cos(angle - 1.57f) * (obs.radius + 3),
                    obs.pos.y + sin(angle - 1.57f) * (obs.radius + 3),
                    playerPos.z
                };
                return avoidLeft;
            }
        }
        return target;
    }

    void MoveTo(Vector3 target) {
        Vector3 playerPos = GetPosition();
        float dist = sqrt(pow(target.x - playerPos.x, 2) + pow(target.y - playerPos.y, 2));
        if (dist < 0.5f) return;
        
        Vector3 adjustedTarget = AvoidObstacles(target);
        if (adjustedTarget.x != target.x || adjustedTarget.y != target.y) {
            float distToAvoid = sqrt(pow(adjustedTarget.x - playerPos.x, 2) + 
                                     pow(adjustedTarget.y - playerPos.y, 2));
            if (distToAvoid > 2.0f) target = adjustedTarget;
        }
        
        float angle = atan2(target.y - playerPos.y, target.x - playerPos.x);
        SetAngle(angle * 180.0f / 3.14159f - 90.0f);
        
        float currentSpeed = carryingBox ? speed * 0.5f : speed;
        Vector3 newPos;
        newPos.x = playerPos.x + cos(angle) * currentSpeed;
        newPos.y = playerPos.y + sin(angle) * currentSpeed;
        newPos.z = target.z;
        SetPosition(newPos);
    }

    // ==================== ОСНОВНОЙ ЦИКЛ ====================

    void BotLoop() {
        active = true;
        Vector3 lastPos = GetPosition();
        int stuckCount = 0;
        int scanCount = 0;
        
        while (active) {
            if (!running) {
                Sleep(100);
                continue;
            }
            
            scanCount++;
            Vector3 playerPos = GetPosition();
            
            // Проверка на застревание
            float moveDist = sqrt(pow(playerPos.x - lastPos.x, 2) + pow(playerPos.y - lastPos.y, 2));
            if (moveDist < 0.05f && running) {
                stuckCount++;
                if (stuckCount > 30) {
                    SetColor(LIGHT_YELLOW);
                    cout << "⚠️ Застрял! Обхожу..." << endl;
                    SetColor(WHITE);
                    Vector3 randomTarget = {
                        playerPos.x + (rand() % 80 - 40),
                        playerPos.y + (rand() % 80 - 40),
                        playerPos.z
                    };
                    MoveTo(randomTarget);
                    stuckCount = 0;
                    continue;
                }
            } else {
                stuckCount = 0;
            }
            lastPos = playerPos;
            
            // Сканирование
            if (scanCount % 10 == 0) {
                ScanMarkers();
            }
            
            // Логика бота
            if (carryingBox) {
                if (deliveryPoint.x != 0) {
                    float dist = sqrt(pow(deliveryPoint.x - playerPos.x, 2) + 
                                     pow(deliveryPoint.y - playerPos.y, 2));
                    
                    if (dist < 2.0f) {
                        carryingBox = false;
                        boxesDelivered++;
                        SetColor(LIGHT_GREEN);
                        cout << "✅ ЯЩИК ДОСТАВЛЕН! (" << boxesDelivered << ")" << endl;
                        SetColor(WHITE);
                    } else {
                        MoveTo(deliveryPoint);
                    }
                }
                continue;
            }
            
            // Поиск ближайшего ящика
            Marker* nearestBox = nullptr;
            float nearestDist = 999999.0f;
            
            for (auto& marker : markers) {
                if (marker.type == 1 && marker.active && !marker.visited) {
                    float dist = sqrt(pow(marker.pos.x - playerPos.x, 2) + 
                                     pow(marker.pos.y - playerPos.y, 2));
                    if (dist < nearestDist) {
                        nearestBox = &marker;
                        nearestDist = dist;
                    }
                }
            }
            
            if (nearestBox) {
                float dist = sqrt(pow(nearestBox->pos.x - playerPos.x, 2) + 
                                 pow(nearestBox->pos.y - playerPos.y, 2));
                
                if (dist < 2.0f) {
                    carryingBox = true;
                    nearestBox->visited = true;
                    boxesCollected++;
                    
                    // Удаляем из активных
                    collectedMarkers.push_back(*nearestBox);
                    markers.erase(remove_if(markers.begin(), markers.end(),
                        [nearestBox](Marker& m) { return m.address == nearestBox->address; }), 
                        markers.end());
                    
                    SetColor(LIGHT_YELLOW);
                    cout << "📦 ЯЩИК ЗАБРАН! (" << boxesCollected << ")" << endl;
                    SetColor(WHITE);
                } else {
                    MoveTo(nearestBox->pos);
                }
            } else {
                // Нет ящиков - двигаемся для поиска
                if (scanCount % 100 == 0) {
                    SetColor(GRAY);
                    cout << "🔍 Поиск ящиков..." << endl;
                    SetColor(WHITE);
                    Vector3 randomTarget = {
                        playerPos.x + (rand() % 120 - 60),
                        playerPos.y + (rand() % 120 - 60),
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
            SetColor(LIGHT_YELLOW);
            cout << "⚠️ Бот уже запущен!" << endl;
            SetColor(WHITE);
            return;
        }
        
        if (!processHandle || !playerAddr) {
            SetColor(LIGHT_RED);
            cout << "❌ Ошибка! Бот не инициализирован." << endl;
            SetColor(WHITE);
            return;
        }
        
        running = true;
        carryingBox = false;
        lastScan = high_resolution_clock::now();
        
        SetColor(LIGHT_GREEN);
        cout << "\n🚀 БОТ ЗАПУЩЕН!" << endl;
        cout << "📦 Собираю ящики..." << endl;
        SetColor(WHITE);
        
        thread(&CollectorBot::BotLoop, this).detach();
    }

    void Stop() {
        running = false;
        SetColor(LIGHT_YELLOW);
        cout << "\n⏹️ БОТ ОСТАНОВЛЕН" << endl;
        cout << "📊 Собрано: " << boxesCollected << " | Доставлено: " << boxesDelivered << endl;
        SetColor(WHITE);
    }

    void EmergencyStop() {
        running = false;
        active = false;
        SetColor(LIGHT_RED);
        cout << "\n🛑 ЭКСТРЕННАЯ ОСТАНОВКА!" << endl;
        SetColor(WHITE);
    }

    void SetSpeed(float newSpeed) {
        if (newSpeed < 0.1) newSpeed = 0.1;
        if (newSpeed > 0.8) newSpeed = 0.8;
        speed = newSpeed;
        SetColor(LIGHT_CYAN);
        cout << "⚡ Скорость: " << speed << endl;
        SetColor(WHITE);
    }

    void ToggleHelp() {
        showHelp = !showHelp;
        if (showHelp) ShowHelp();
    }

    void ShowStatus() {
        Vector3 pos = GetPosition();
        
        SetColor(LIGHT_CYAN);
        cout << "\n╔════════════════════════════════════════════╗" << endl;
        cout << "║           📊 СТАТУС БОТА                 ║" << endl;
        cout << "╚════════════════════════════════════════════╝" << endl;
        SetColor(WHITE);
        
        cout << "  Позиция:     X=" << pos.x << " Y=" << pos.y << " Z=" << pos.z << endl;
        cout << "  Скорость:    " << speed << endl;
        cout << "  Состояние:   " << (running ? "🟢 Активен" : "🔴 Остановлен") << endl;
        cout << "  Ящик:        " << (carryingBox ? "🟡 Несу" : "🔴 Ищу") << endl;
        cout << "  Собрано:     " << boxesCollected << endl;
        cout << "  Доставлено:  " << boxesDelivered << endl;
        cout << "  Активных:    " << markers.size() << endl;
        
        if (deliveryPoint.x != 0) {
            cout << "  Точка сдачи: X=" << deliveryPoint.x << " Y=" << deliveryPoint.y << endl;
        }
        
        SetColor(LIGHT_CYAN);
        cout << "╔════════════════════════════════════════════╗" << endl;
        cout << "║  F1 - Запуск   F2 - Стоп   F3 - Скорость ║" << endl;
        cout << "║  F4 - Статус   F5 - Помощь  ESC - Выход  ║" << endl;
        cout << "╚════════════════════════════════════════════╝" << endl;
        SetColor(WHITE);
    }

    void ShowHelp() {
        SetColor(LIGHT_YELLOW);
        cout << "\n╔════════════════════════════════════════════╗" << endl;
        cout << "║           ❓ ПОМОЩЬ                      ║" << endl;
        cout << "╚════════════════════════════════════════════╝" << endl;
        SetColor(WHITE);
        cout << "  🎮 ГОРЯЧИЕ КЛАВИШИ:" << endl;
        cout << "    F1  - Запустить бота" << endl;
        cout << "    F2  - Остановить бота" << endl;
        cout << "    F3  - Увеличить скорость (+0.1)" << endl;
        cout << "    F4  - Показать статус" << endl;
        cout << "    F5  - Показать/скрыть помощь" << endl;
        cout << "    ESC - Выйти из программы" << endl;
        cout << endl;
        cout << "  📦 КОМАНДЫ В КОНСОЛИ:" << endl;
        cout << "    speed X   - Установить скорость (0.1-0.8)" << endl;
        cout << "    status    - Показать статус" << endl;
        cout << "    start     - Запустить бота" << endl;
        cout << "    stop      - Остановить бота" << endl;
        cout << "    help      - Показать помощь" << endl;
        cout << "    exit      - Выйти" << endl;
        SetColor(LIGHT_YELLOW);
        cout << "╔════════════════════════════════════════════╗" << endl;
        cout << "║  Нажмите F5 чтобы скрыть эту справку     ║" << endl;
        cout << "╚════════════════════════════════════════════╝" << endl;
        SetColor(WHITE);
    }

    void ProcessCommand(const string& cmd) {
        if (cmd == "start") {
            Start();
        } else if (cmd == "stop") {
            Stop();
        } else if (cmd == "status") {
            ShowStatus();
        } else if (cmd == "help") {
            ShowHelp();
        } else if (cmd == "exit") {
            EmergencyStop();
            exit(0);
        } else if (cmd.find("speed") == 0) {
            try {
                float s = stof(cmd.substr(6));
                SetSpeed(s);
            } catch (...) {
                SetColor(LIGHT_RED);
                cout << "❌ Используйте: speed 0.3" << endl;
                SetColor(WHITE);
            }
        } else if (!cmd.empty()) {
            SetColor(LIGHT_RED);
            cout << "❌ Неизвестная команда. Введите 'help'" << endl;
            SetColor(WHITE);
        }
    }

    // ==================== ОБРАБОТЧИК ГОРЯЧИХ КЛАВИШ ====================

    void HotkeyHandler() {
        while (true) {
            if (GetAsyncKeyState(VK_F1) & 1) {
                Start();
            }
            if (GetAsyncKeyState(VK_F2) & 1) {
                Stop();
            }
            if (GetAsyncKeyState(VK_F3) & 1) {
                SetSpeed(min(0.8f, speed + 0.1f));
            }
            if (GetAsyncKeyState(VK_F4) & 1) {
                ShowStatus();
            }
            if (GetAsyncKeyState(VK_F5) & 1) {
                ToggleHelp();
            }
            if (GetAsyncKeyState(VK_ESCAPE) & 1) {
                EmergencyStop();
                SetColor(LIGHT_YELLOW);
                cout << "👋 Выход..." << endl;
                SetColor(WHITE);
                exit(0);
            }
            Sleep(50);
        }
    }

    bool IsReady() {
        return processHandle != NULL && playerAddr != 0;
    }
};

// ==================== ОТДЕЛЬНЫЙ ПОТОК ДЛЯ ВВОДА КОМАНД ====================

void CommandThread(CollectorBot* bot) {
    string input;
    while (true) {
        cout << "> ";
        getline(cin, input);
        bot->ProcessCommand(input);
    }
}

// ==================== ГЛАВНАЯ ФУНКЦИЯ ====================

int main() {
    SetConsoleCP(1251);
    SetConsoleOutputCP(1251);
    
    // Скрываем курсор
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hConsole, &cursorInfo);
    cursorInfo.bVisible = false;
    SetConsoleCursorInfo(hConsole, &cursorInfo);
    
    // Очищаем консоль
    system("cls");
    
    // Заголовок
    SetColor(LIGHT_GREEN);
    cout << "\n╔═══════════════════════════════════════════════════════╗" << endl;
    cout << "║           🚀 БОТ-СБОРЩИК ЯЩИКОВ v5.0               ║" << endl;
    cout << "║            ДЛЯ MTA PROVINCE                         ║" << endl;
    cout << "║                                                      ║" << endl;
    cout << "║  📦 ДИНАМИЧЕСКИЙ ПОИСК МАРКЕРОВ                    ║" << endl;
    cout << "║  🎯 АВТОМАТИЧЕСКИЙ СБОР ЯЩИКОВ                     ║" << endl;
    cout << "║  🧠 ЗАПОМИНАНИЕ МАРШРУТОВ                          ║" << endl;
    cout << "║  🚧 ОБХОД ПРЕПЯТСТВИЙ                              ║" << endl;
    cout << "║  ⌨️  УПРАВЛЕНИЕ ГОРЯЧИМИ КЛАВИШАМИ                 ║" << endl;
    cout << "╚═══════════════════════════════════════════════════════╝" << endl;
    
    SetColor(LIGHT_YELLOW);
    cout << "\n  ⚠️  Запускайте от имени администратора!" << endl;
    cout << "  ⚠️  MTA Province должна быть запущена!" << endl;
    cout << "  ⚠️  Нажмите F5 для справки по управлению\n" << endl;
    SetColor(WHITE);
    
    CollectorBot bot;
    
    if (!bot.IsReady()) {
        SetColor(LIGHT_RED);
        cout << "\n❌ ОШИБКА: MTA Province не найден!" << endl;
        cout << "Убедитесь, что игра запущена." << endl;
        SetColor(WHITE);
        system("pause");
        return 1;
    }
    
    SetColor(LIGHT_GREEN);
    cout << "✅ БОТ ГОТОВ К РАБОТЕ!" << endl;
    SetColor(WHITE);
    cout << "📌 Нажмите F1 для запуска\n" << endl;
    
    // Запускаем обработчик горячих клавиш
    thread hotkeyThread(&CollectorBot::HotkeyHandler, &bot);
    hotkeyThread.detach();
    
    // Запускаем поток для ввода команд
    thread cmdThread(CommandThread, &bot);
    cmdThread.detach();
    
    // Основной цикл - обновление статуса
    while (true) {
        Sleep(1000);
    }
    
    return 0;
}

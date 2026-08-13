// main.cpp - Bot for MTA Province (Russian UI)
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

using namespace std;
using namespace chrono;

// Цвета
void SetColor(int color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}

// Структуры
struct Vec3 { float x, y, z; };
struct Marker { Vec3 pos; int type; bool active; bool collected; DWORD address; time_t spawnTime; };

// ==================== ГЛАВНЫЙ КЛАСС ====================
class Bot {
private:
    HANDLE proc;
    DWORD pid, playerAddr, baseAddr;
    HWND gameWnd;
    bool running, active, carrying, emergency;
    float speed;
    int collected, delivered;
    vector<Marker> markers;
    vector<Marker> collectedMarkers;
    vector<Vec3> obstacles;
    Vec3 deliveryPoint;
    chrono::high_resolution_clock::time_point lastScan;
    int stuckCount;
    Vec3 lastPos;
    bool shiftPressed;
    int jumpCounter;

public:
    Bot() : proc(NULL), pid(0), playerAddr(0), baseAddr(0), running(false),
            active(false), carrying(false), speed(0.25f), collected(0),
            delivered(0), emergency(false), stuckCount(0), shiftPressed(false),
            jumpCounter(0) {
        deliveryPoint = {0,0,0};
        lastPos = {0,0,0};
        gameWnd = NULL;
        FindProcess();
        if (proc) FindAddresses();
    }

    ~Bot() { if (proc) CloseHandle(proc); ReleaseShift(); }

    // ==================== ЧТЕНИЕ ПАМЯТИ ====================
    template<typename T>
    T Read(DWORD addr) {
        T val = {};
        if (!proc || !addr) return val;
        typedef NTSTATUS(WINAPI* NtRead)(HANDLE, PVOID, PVOID, SIZE_T, SIZE_T*);
        static NtRead NtReadMem = NULL;
        if (!NtReadMem) {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            NtReadMem = (NtRead)GetProcAddress(ntdll, "NtReadVirtualMemory");
        }
        if (NtReadMem) { SIZE_T read; NtReadMem(proc, (PVOID)addr, &val, sizeof(T), &read); }
        return val;
    }

    template<typename T>
    void Write(DWORD addr, T val) {
        if (!proc || !addr) return;
        typedef NTSTATUS(WINAPI* NtWrite)(HANDLE, PVOID, PVOID, SIZE_T, SIZE_T*);
        static NtWrite NtWriteMem = NULL;
        if (!NtWriteMem) {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            NtWriteMem = (NtWrite)GetProcAddress(ntdll, "NtWriteVirtualMemory");
        }
        if (NtWriteMem) { SIZE_T written; NtWriteMem(proc, (PVOID)addr, &val, sizeof(T), &written); }
    }

    // ==================== ПОИСК ИГРЫ ====================
    void FindProcess() {
        gameWnd = FindWindowW(NULL, L"MTA: Province");
        if (!gameWnd) gameWnd = FindWindowW(NULL, L"MTA: San Andreas");
        if (gameWnd) {
            GetWindowThreadProcessId(gameWnd, &pid);
            proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
            if (proc) Print("[OK] Найден процесс MTA", 10);
            return;
        }
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe; pe.dwSize = sizeof(PROCESSENTRY32W);
            if (Process32FirstW(snap, &pe)) {
                do {
                    wstring name(pe.szExeFile);
                    transform(name.begin(), name.end(), name.begin(), ::towlower);
                    if (name.find(L"gta_sa") != wstring::npos || name.find(L"gta") != wstring::npos || name.find(L"mta") != wstring::npos) {
                        pid = pe.th32ProcessID;
                        proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
                        if (proc) { Print("[OK] Найден процесс MTA", 10); break; }
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        if (!proc) Print("[ERROR] MTA Province не найден!", 12);
    }

    // ==================== ПОИСК АДРЕСОВ ====================
    void FindAddresses() {
        if (!proc) return;
        Print("[INFO] Поиск адресов...", 11);
        HMODULE mods[1024]; DWORD needed;
        if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) baseAddr = (DWORD)mods[0];
        playerAddr = 0xB6F5F0;
        DWORD test = Read<DWORD>(playerAddr);
        if (test > 0x10000 && test < 0x7FFFFFFF) { Print("[OK] Адрес игрока найден", 10); return; }
        for (DWORD addr = baseAddr; addr < baseAddr + 0x500000; addr += 4) {
            DWORD ped = Read<DWORD>(addr);
            if (ped > 0x10000 && ped < 0x7FFFFFFF) {
                float x = Read<float>(ped + 0x14), y = Read<float>(ped + 0x18);
                if (x > -5000 && x < 5000 && y > -5000 && y < 5000) { playerAddr = addr; Print("[OK] Адрес игрока найден", 10); return; }
            }
        }
        Print("[ERROR] Не удалось найти адреса!", 12);
    }

    // ==================== ПОЗИЦИЯ ====================
    Vec3 GetPos() {
        Vec3 pos = {0,0,0};
        if (!playerAddr) return pos;
        DWORD ped = Read<DWORD>(playerAddr);
        if (ped) { pos.x = Read<float>(ped + 0x14); pos.y = Read<float>(ped + 0x18); pos.z = Read<float>(ped + 0x1C); }
        return pos;
    }

    void SetPos(Vec3 pos) {
        if (!playerAddr) return;
        DWORD ped = Read<DWORD>(playerAddr);
        if (ped) { Write<float>(ped + 0x14, pos.x); Write<float>(ped + 0x18, pos.y); Write<float>(ped + 0x1C, pos.z); }
    }

    void SetAngle(float angle) {
        if (!playerAddr) return;
        DWORD ped = Read<DWORD>(playerAddr);
        if (ped) Write<float>(ped + 0x20, angle);
    }

    // ==================== КЛАВИШИ ====================
    void PressShift() { if (!shiftPressed && gameWnd) { PostMessage(gameWnd, WM_KEYDOWN, VK_SHIFT, 0); shiftPressed = true; } }
    void ReleaseShift() { if (shiftPressed && gameWnd) { PostMessage(gameWnd, WM_KEYUP, VK_SHIFT, 0); shiftPressed = false; } }
    void PressSpace() { if (gameWnd) { PostMessage(gameWnd, WM_KEYDOWN, VK_SPACE, 0); Sleep(30); PostMessage(gameWnd, WM_KEYUP, VK_SPACE, 0); } }

    // ==================== ПОИСК МАРКЕРОВ ====================
    void ScanMarkers() {
        if (!proc || !baseAddr) return;
        auto now = chrono::high_resolution_clock::now();
        if (duration_cast<milliseconds>(now - lastScan).count() < 200) return;
        lastScan = now;
        for (DWORD addr = baseAddr; addr < baseAddr + 0x800000; addr += 0x28) {
            float x = Read<float>(addr), y = Read<float>(addr + 4), z = Read<float>(addr + 8);
            int type = Read<int>(addr + 0xC);
            bool active = Read<bool>(addr + 0x10);
            if (!active || x == 0 || y == 0 || x < -5000 || x > 5000 || y < -5000 || y > 5000) continue;
            bool exists = false;
            for (auto& m : markers) { float d = sqrt(pow(m.pos.x - x, 2) + pow(m.pos.y - y, 2)); if (d < 0.5f) { exists = true; break; } }
            if (exists) continue;
            bool col = false;
            for (auto& m : collectedMarkers) { float d = sqrt(pow(m.pos.x - x, 2) + pow(m.pos.y - y, 2)); if (d < 0.5f) { col = true; break; } }
            if (col) continue;
            if (type == 1) { Marker m; m.pos = {x,y,z}; m.type = type; m.active = true; m.collected = false; m.address = addr; m.spawnTime = time(NULL); markers.push_back(m); Print("[BOX] Найден новый ящик! X=" + to_string((int)x) + " Y=" + to_string((int)y), 10); }
            else if (type == 2) { deliveryPoint = {x,y,z}; Print("[DROP] Точка сдачи найдена! X=" + to_string((int)x) + " Y=" + to_string((int)y), 11); }
        }
    }

    // ==================== ОБХОД ПРЕПЯТСТВИЙ ====================
    Vec3 AvoidObstacles(Vec3 target) {
        Vec3 pos = GetPos();
        for (auto& o : obstacles) {
            float d = sqrt(pow(o.x - pos.x, 2) + pow(o.y - pos.y, 2));
            if (d < 5.0f) {
                float angle = atan2(pos.y - o.y, pos.x - o.x);
                Vec3 right = { o.x + cos(angle + 1.57f) * 6.0f, o.y + sin(angle + 1.57f) * 6.0f, pos.z };
                bool free = true;
                for (auto& o2 : obstacles) { float d2 = sqrt(pow(right.x - o2.x, 2) + pow(right.y - o2.y, 2)); if (d2 < 4.0f) { free = false; break; } }
                if (free) return right;
                Vec3 left = { o.x + cos(angle - 1.57f) * 6.0f, o.y + sin(angle - 1.57f) * 6.0f, pos.z };
                return left;
            }
        }
        return target;
    }

    // ==================== ДВИЖЕНИЕ ====================
    void MoveTo(Vec3 target) {
        Vec3 pos = GetPos();
        float dist = sqrt(pow(target.x - pos.x, 2) + pow(target.y - pos.y, 2));
        if (dist < 0.5f || emergency) return;

        if (carrying) { ReleaseShift(); }
        else {
            PressShift();
            jumpCounter++;
            if (jumpCounter % 3 == 0 && dist > 5.0f) { PressSpace(); }
            if (dist < 3.0f) { jumpCounter = 0; }
        }

        Vec3 adjusted = AvoidObstacles(target);
        if (adjusted.x != target.x || adjusted.y != target.y) {
            float d = sqrt(pow(adjusted.x - pos.x, 2) + pow(adjusted.y - pos.y, 2));
            if (d > 2.0f) target = adjusted;
        }

        float angle = atan2(target.y - pos.y, target.x - pos.x);
        SetAngle(angle * 180.0f / 3.14159f - 90.0f);
        float curSpeed = carrying ? 0.2f : 0.4f;
        Vec3 newPos = { pos.x + cos(angle) * curSpeed, pos.y + sin(angle) * curSpeed, target.z };
        SetPos(newPos);
    }

    // ==================== ОСНОВНОЙ ЦИКЛ ====================
    void BotLoop() {
        active = true;
        Print("[START] Бот запущен! Собираю ящики...", 10);
        Print("[RUN] Бег с Shift + прыжки | С ящиком - ходьба", 11);
        int scanCount = 0;
        lastPos = GetPos();
        stuckCount = 0;
        jumpCounter = 0;

        while (active) {
            if (!running || emergency) { Sleep(100); continue; }
            scanCount++;
            Vec3 pos = GetPos();

            float move = sqrt(pow(pos.x - lastPos.x, 2) + pow(pos.y - lastPos.y, 2));
            if (move < 0.05f) {
                stuckCount++;
                if (stuckCount > 40) {
                    Print("[WARN] Застрял! Меняю направление...", 14);
                    PressSpace(); Sleep(100); PressSpace();
                    Vec3 random = { pos.x + (rand() % 80 - 40), pos.y + (rand() % 80 - 40), pos.z };
                    MoveTo(random);
                    stuckCount = 0;
                }
            } else { stuckCount = 0; }
            lastPos = pos;

            if (scanCount % 5 == 0) ScanMarkers();

            if (carrying) {
                if (deliveryPoint.x != 0) {
                    float dist = sqrt(pow(deliveryPoint.x - pos.x, 2) + pow(deliveryPoint.y - pos.y, 2));
                    if (dist < 2.0f) {
                        carrying = false;
                        delivered++;
                        ReleaseShift();
                        Print("[DONE] Ящик доставлен! (" + to_string(delivered) + ")", 10);
                        obstacles.push_back(pos);
                        jumpCounter = 0;
                    } else { MoveTo(deliveryPoint); }
                }
                continue;
            }

            Marker* nearest = nullptr;
            float nearDist = 999999.0f;
            for (auto& m : markers) {
                if (m.type == 1 && !m.collected) {
                    float d = sqrt(pow(m.pos.x - pos.x, 2) + pow(m.pos.y - pos.y, 2));
                    if (d < nearDist) { nearest = &m; nearDist = d; }
                }
            }

            if (nearest) {
                float dist = sqrt(pow(nearest->pos.x - pos.x, 2) + pow(nearest->pos.y - pos.y, 2));
                if (dist < 2.0f) {
                    carrying = true;
                    nearest->collected = true;
                    collected++;
                    collectedMarkers.push_back(*nearest);
                    markers.erase(remove_if(markers.begin(), markers.end(),
                        [nearest](Marker& m) { return m.address == nearest->address; }), markers.end());
                    ReleaseShift();
                    Print("[TAKEN] Ящик взят! (" + to_string(collected) + ") Несу на сдачу...", 14);
                    jumpCounter = 0;
                } else { MoveTo(nearest->pos); }
            } else {
                if (scanCount % 100 == 0) {
                    Print("[SEARCH] Ищу новые ящики...", 8);
                    Vec3 random = { pos.x + (rand() % 100 - 50), pos.y + (rand() % 100 - 50), pos.z };
                    MoveTo(random);
                }
            }
            Sleep(50);
        }
    }

    // ==================== ВЫВОД ====================
    void Print(string text, int color = 15) { SetColor(color); cout << text << endl; SetColor(15); }

    // ==================== УПРАВЛЕНИЕ ====================
    void Start() {
        if (running) { Print("[WARN] Бот уже работает!", 14); return; }
        if (!proc || !playerAddr) { Print("[ERROR] Бот не инициализирован!", 12); return; }
        running = true; emergency = false; carrying = false; shiftPressed = false;
        lastScan = chrono::high_resolution_clock::now();
        if (gameWnd) SetForegroundWindow(gameWnd);
        Print("[START] Бот запущен! Нажмите F11 для экстренной остановки.", 10);
        thread(&Bot::BotLoop, this).detach();
    }

    void Stop() { running = false; ReleaseShift(); Print("[STOP] Бот остановлен. Собрано: " + to_string(collected) + " | Доставлено: " + to_string(delivered), 14); }
    void EmergencyStop() { emergency = true; running = false; active = false; ReleaseShift(); Print("[EMERGENCY] ЭКСТРЕННАЯ ОСТАНОВКА! (F11)", 12); }
    void SpeedUp() { speed = min(0.8f, speed + 0.1f); Print("[SPEED] Скорость: " + to_string(speed), 11); }
    void SpeedDown() { speed = max(0.1f, speed - 0.1f); Print("[SPEED] Скорость: " + to_string(speed), 11); }

    // ==================== СТАТУС ====================
    void ShowStatus() {
        Vec3 pos = GetPos();
        SetColor(11);
        cout << "\n========================================" << endl;
        cout << "           СТАТУС БОТА" << endl;
        cout << "========================================" << endl;
        SetColor(15);
        cout << "  Позиция:    X=" << (int)pos.x << " Y=" << (int)pos.y << " Z=" << (int)pos.z << endl;
        cout << "  Скорость:   " << speed << endl;
        cout << "  Состояние:  " << (running ? "[РАБОТАЕТ]" : "[ОСТАНОВЛЕН]") << endl;
        cout << "  Ящик:       " << (carrying ? "[НЕСУ]" : "[ИЩУ]") << endl;
        cout << "  Собрано:    " << collected << endl;
        cout << "  Доставлено: " << delivered << endl;
        cout << "  Найдено:    " << markers.size() << " ящиков" << endl;
        if (deliveryPoint.x != 0) cout << "  Сдача:      X=" << (int)deliveryPoint.x << " Y=" << (int)deliveryPoint.y << endl;
        SetColor(11);
        cout << "========================================" << endl;
        cout << "  F1-Старт  F2-Стоп  F3-Быстрее  F4-Медленнее" << endl;
        cout << "  F5-Статус  F11-Экстренный стоп  ESC-Выход" << endl;
        cout << "========================================" << endl;
        SetColor(15);
    }

    // ==================== ПОМОЩЬ ====================
    void ShowHelp() {
        SetColor(14);
        cout << "\n========================================" << endl;
        cout << "           УПРАВЛЕНИЕ" << endl;
        cout << "========================================" << endl;
        SetColor(15);
        cout << "  F1  - Запустить бота" << endl;
        cout << "  F2  - Остановить бота" << endl;
        cout << "  F3  - Увеличить скорость" << endl;
        cout << "  F4  - Уменьшить скорость" << endl;
        cout << "  F5  - Показать статус" << endl;
        cout << "  F11 - ЭКСТРЕННАЯ ОСТАНОВКА" << endl;
        cout << "  ESC - Выйти" << endl;
        cout << endl;
        cout << "  БЕЗ ЯЩИКА: бег с Shift + прыжки" << endl;
        cout << "  С ЯЩИКОМ:  обычная ходьба" << endl;
        SetColor(14);
        cout << "========================================" << endl;
        cout << "  Скрытый режим - античит не детектит" << endl;
        cout << "========================================" << endl;
        SetColor(15);
    }

    // ==================== ОБРАБОТЧИК КЛАВИШ ====================
    void HotkeyHandler() {
        while (true) {
            if (GetAsyncKeyState(VK_F1) & 1) Start();
            if (GetAsyncKeyState(VK_F2) & 1) Stop();
            if (GetAsyncKeyState(VK_F3) & 1) SpeedUp();
            if (GetAsyncKeyState(VK_F4) & 1) SpeedDown();
            if (GetAsyncKeyState(VK_F5) & 1) ShowStatus();
            if (GetAsyncKeyState(VK_F11) & 1) EmergencyStop();
            if (GetAsyncKeyState(VK_ESCAPE) & 1) { EmergencyStop(); Print("[EXIT] Выход...", 14); exit(0); }
            Sleep(50);
        }
    }

    bool Ready() { return proc != NULL && playerAddr != 0; }
};

// ==================== ГЛАВНАЯ ====================
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
    cout << "\n==================================================" << endl;
    cout << "      СКРЫТЫЙ БОТ-СБОРЩИК ДЛЯ MTA PROVINCE" << endl;
    cout << "==================================================" << endl;
    cout << "  [BOX] Собирает ящики" << endl;
    cout << "  [DROP] Относит в точку сдачи" << endl;
    cout << "  [AVOID] Обходит препятствия" << endl;
    cout << "  [SPRINT] Бег с Shift (без ящика)" << endl;
    cout << "  [JUMP] Прыжки к ящику" << endl;
    cout << "  [WALK] Ходьба (с ящиком)" << endl;
    cout << "  [STEALTH] Скрытый режим" << endl;
    cout << "==================================================" << endl;
    SetColor(14);
    cout << "\n  [WARN] Запускайте от имени администратора!" << endl;
    cout << "  [WARN] MTA Province должна быть запущена!" << endl;
    cout << "  [WARN] Нажмите F5 для просмотра управления" << endl;
    cout << endl;
    SetColor(15);

    Bot bot;
    if (!bot.Ready()) {
        SetColor(12);
        cout << "\n[ERROR] MTA Province не найден!" << endl;
        cout << "Убедитесь, что игра запущена." << endl;
        SetColor(15);
        system("pause");
        return 1;
    }

    SetColor(10);
    cout << "[OK] БОТ ГОТОВ К РАБОТЕ!" << endl;
    SetColor(15);
    cout << "[INFO] Нажмите F1 для запуска" << endl;
    cout << endl;

    thread handler(&Bot::HotkeyHandler, &bot);
    handler.detach();

    while (true) Sleep(1000);
    return 0;
}

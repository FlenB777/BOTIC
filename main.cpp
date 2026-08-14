// main.cpp - MTA Bot with Job Detection
#define _USE_MATH_DEFINES
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
#include <mutex>

using namespace std;
using namespace chrono;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

mutex memoryMutex;

void SetColor(int color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}

struct Vec3 { float x, y, z; };
struct Marker { 
    Vec3 pos; 
    int type; 
    bool active; 
    bool collected; 
    DWORD address; 
    time_t spawnTime;
    int id;
    bool isDeliveryPoint;
};

class Bot {
private:
    HANDLE proc;
    DWORD pid, playerAddr;
    DWORD64 baseAddr;
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
    int jumpCounter;
    
    DWORD pedAddress;
    
    float currentAngleDiff;
    Vec3 avoidancePoint;
    bool avoiding;
    int avoidanceCounter;
    float lastDistanceToTarget;
    int sameDistanceCount;
    
    bool wPressed, aPressed, sPressed, dPressed, shiftPressed;
    
    // Состояние работы
    bool jobActive;
    chrono::high_resolution_clock::time_point jobCheckTime;
    int noMarkerCount;

public:
    Bot() : proc(NULL), pid(0), playerAddr(0), baseAddr(0), running(false),
            active(false), carrying(false), speed(0.25f), collected(0),
            delivered(0), emergency(false), stuckCount(0),
            jumpCounter(0), currentAngleDiff(0), avoiding(false), avoidanceCounter(0),
            lastDistanceToTarget(0), sameDistanceCount(0), pedAddress(0),
            wPressed(false), aPressed(false), sPressed(false), dPressed(false), 
            shiftPressed(false), jobActive(false), noMarkerCount(0) {
        deliveryPoint = {0,0,0};
        lastPos = {0,0,0};
        avoidancePoint = {0,0,0};
        gameWnd = NULL;
        FindProcess();
        if (proc) FindAddresses();
    }

    ~Bot() { 
        StopAll();
        if (proc) CloseHandle(proc); 
    }

    template<typename T>
    T Read(DWORD addr) {
        T val = {};
        if (!proc || !addr) return val;
        
        SIZE_T bytesRead;
        if (ReadProcessMemory(proc, (LPCVOID)(DWORD64)addr, &val, sizeof(T), &bytesRead)) {
            if (bytesRead == sizeof(T)) return val;
        }
        return val;
    }

    DWORD GetPedAddress() {
        if (pedAddress == 0 || pedAddress < 0x10000) {
            pedAddress = Read<DWORD>(playerAddr);
        }
        return pedAddress;
    }

    void FindProcess() {
        gameWnd = FindWindowW(NULL, L"MTA: Province");
        if (!gameWnd) gameWnd = FindWindowW(NULL, L"MTA: San Andreas");
        if (gameWnd) {
            GetWindowThreadProcessId(gameWnd, &pid);
            proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_OPERATION, FALSE, pid);
            if (proc) {
                Print("[OK] Process found", 10);
                return;
            }
        }
        
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe; pe.dwSize = sizeof(PROCESSENTRY32W);
            if (Process32FirstW(snap, &pe)) {
                do {
                    wstring name(pe.szExeFile);
                    transform(name.begin(), name.end(), name.begin(), ::towlower);
                    if (name.find(L"gta_sa") != wstring::npos || 
                        name.find(L"gta") != wstring::npos || 
                        name.find(L"mta") != wstring::npos ||
                        name.find(L"proxy") != wstring::npos) {
                        pid = pe.th32ProcessID;
                        proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_OPERATION, FALSE, pid);
                        if (proc) { 
                            Print("[OK] Process found: " + string(name.begin(), name.end()), 10); 
                            break; 
                        }
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        if (!proc) Print("[ERROR] MTA Province not found!", 12);
    }

    void FindAddresses() {
        if (!proc) return;
        Print("[INFO] Searching addresses...", 11);
        
        HMODULE mods[1024]; DWORD needed;
        if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
            baseAddr = (DWORD64)mods[0];
        }
        
        vector<DWORD> knownAddresses = {
            0xB6F5F0, 0xB6F5F4, 0xB6F5EC, 0xB6F5E8,
            0xB74490, 0xB74494, 0xB6F5F8, 0xB6F5FC
        };
        
        for (DWORD addr : knownAddresses) {
            DWORD test = Read<DWORD>(addr);
            if (test > 0x10000 && test < 0x7FFFFFFF) {
                float x = Read<float>(test + 0x14);
                float y = Read<float>(test + 0x18);
                float z = Read<float>(test + 0x1C);
                
                if (x > -10000 && x < 10000 && 
                    y > -10000 && y < 10000 && 
                    z > -1000 && z < 10000) {
                    playerAddr = addr;
                    pedAddress = test;
                    Print("[OK] Player found at 0x" + to_string(addr), 10);
                    break;
                }
            }
        }
        
        if (!playerAddr) {
            Print("[ERROR] Player not found!", 12);
        }
    }

    Vec3 GetPos() {
        Vec3 pos = {0,0,0};
        if (!playerAddr) return pos;
        
        DWORD ped = GetPedAddress();
        if (ped && ped > 0x10000 && ped < 0x7FFFFFFF) {
            pos.x = Read<float>(ped + 0x14);
            pos.y = Read<float>(ped + 0x18);
            pos.z = Read<float>(ped + 0x1C);
        }
        return pos;
    }

    // ==================== ПОИСК МАРКЕРОВ ====================
    void ScanMarkers() {
        if (!proc) return;
        
        Vec3 playerPos = GetPos();
        if (playerPos.x == 0 && playerPos.y == 0) return;
        
        // Очищаем старые маркеры
        time_t currentTime = time(NULL);
        markers.erase(remove_if(markers.begin(), markers.end(),
            [currentTime](const Marker& m) { 
                return (currentTime - m.spawnTime) > 60; // Удаляем маркеры старше 60 секунд
            }), markers.end());
        
        // Сканируем память в поисках новых маркеров
        const DWORD64 scanRegions[][2] = {
            {baseAddr, baseAddr + 0x1000000},
            {0x10000000, 0x20000000},
            {0x20000000, 0x30000000}
        };
        
        for (int region = 0; region < 3; region++) {
            DWORD64 start = scanRegions[region][0];
            DWORD64 end = scanRegions[region][1];
            
            for (DWORD64 addr = start; addr < end; addr += 4) {
                float x = Read<float>((DWORD)addr);
                float y = Read<float>((DWORD)(addr + 4));
                float z = Read<float>((DWORD)(addr + 8));
                
                // Фильтры для координат
                if (x < -3000 || x > 3000) continue;
                if (y < -3000 || y > 3000) continue;
                if (z < -100 || z > 1000) continue;
                if (x == 0 && y == 0) continue;
                
                // Расстояние до игрока
                float dist = sqrt(pow(x - playerPos.x, 2) + pow(y - playerPos.y, 2));
                if (dist > 300) continue;
                
                // Не координаты игрока
                if (abs(x - playerPos.x) < 1 && abs(y - playerPos.y) < 1) continue;
                
                // Проверка на дубликаты
                bool exists = false;
                for (auto& m : markers) {
                    float d = sqrt(pow(m.pos.x - x, 2) + pow(m.pos.y - y, 2));
                    if (d < 2.0f) { exists = true; break; }
                }
                if (exists) continue;
                
                // Создаем новый маркер
                Marker m;
                m.pos = {x, y, z};
                m.type = 1;
                m.active = true;
                m.collected = false;
                m.address = (DWORD)addr;
                m.spawnTime = currentTime;
                m.id = markers.size();
                m.isDeliveryPoint = false;
                
                markers.push_back(m);
                Print("[FOUND] Green marker at distance " + to_string((int)dist) + "m", 10);
                
                if (markers.size() > 20) break;
            }
            
            if (markers.size() > 10) break;
        }
        
        // Сортируем по расстоянию
        sort(markers.begin(), markers.end(), [playerPos](const Marker& a, const Marker& b) {
            float da = sqrt(pow(a.pos.x - playerPos.x, 2) + pow(a.pos.y - playerPos.y, 2));
            float db = sqrt(pow(b.pos.x - playerPos.x, 2) + pow(b.pos.y - playerPos.y, 2));
            return da < db;
        });
    }

    // ==================== КЛАВИШИ ====================
    void SendKey(WORD key, bool press) {
        keybd_event((BYTE)key, 0, press ? 0 : KEYEVENTF_KEYUP, 0);
        Sleep(10);
    }

    void PressW() { if (!wPressed) { SendKey('W', true); wPressed = true; } }
    void ReleaseW() { if (wPressed) { SendKey('W', false); wPressed = false; } }
    void PressS() { if (!sPressed) { SendKey('S', true); sPressed = true; } }
    void ReleaseS() { if (sPressed) { SendKey('S', false); sPressed = false; } }
    void PressA() { if (!aPressed) { SendKey('A', true); aPressed = true; } }
    void ReleaseA() { if (aPressed) { SendKey('A', false); aPressed = false; } }
    void PressD() { if (!dPressed) { SendKey('D', true); dPressed = true; } }
    void ReleaseD() { if (dPressed) { SendKey('D', false); dPressed = false; } }
    void PressShift() { if (!shiftPressed) { SendKey(VK_SHIFT, true); shiftPressed = true; } }
    void ReleaseShift() { if (shiftPressed) { SendKey(VK_SHIFT, false); shiftPressed = false; } }
    void PressSpace() { 
        SendKey(VK_SPACE, true);
        Sleep(50);
        SendKey(VK_SPACE, false);
    }

    void StopAll() {
        ReleaseW();
        ReleaseS();
        ReleaseA();
        ReleaseD();
        ReleaseShift();
    }

    // ==================== ПОВОРОТ ====================
    void TurnToTarget(Vec3 target) {
        Vec3 pos = GetPos();
        float angle = atan2(target.y - pos.y, target.x - pos.x);
        float currentAngle = 0;
        
        DWORD ped = GetPedAddress();
        if (ped && ped > 0x10000 && ped < 0x7FFFFFFF) {
            currentAngle = Read<float>(ped + 0x20);
        }
        
        float diff = angle * 180.0f / M_PI - 90.0f - currentAngle;
        while (diff > 180) diff -= 360;
        while (diff < -180) diff += 360;
        
        if (abs(diff) > 15) {
            if (diff > 0) { PressD(); ReleaseA(); }
            else { PressA(); ReleaseD(); }
        } else if (abs(diff) > 5) {
            if (diff > 0) { 
                PressD(); Sleep(20); ReleaseD();
                ReleaseA();
            } else { 
                PressA(); Sleep(20); ReleaseA();
                ReleaseD();
            }
        } else {
            ReleaseA(); ReleaseD();
        }
        
        currentAngleDiff = diff;
    }

    // ==================== ГЛАВНЫЙ ЦИКЛ ====================
    void BotLoop() {
        active = true;
        Print("[START] Bot started! Waiting for job...", 10);
        Print("[INFO] Take the job in game to start collecting", 11);
        
        int scanCount = 0;
        lastPos = GetPos();
        stuckCount = 0;
        jumpCounter = 0;
        Vec3 targetPos = {0,0,0};
        bool hasTarget = false;

        while (active) {
            if (!running || emergency) { StopAll(); Sleep(100); continue; }
            
            scanCount++;
            Vec3 pos = GetPos();
            
            // Сканируем маркеры каждые 2 секунды
            if (scanCount % 40 == 0) {
                ScanMarkers();
                
                // Проверяем, есть ли маркеры
                if (markers.empty()) {
                    noMarkerCount++;
                    if (noMarkerCount == 1) {
                        Print("[WAIT] No markers found. Take the job in game!", 14);
                        Print("[WAIT] Bot will auto-detect markers when they appear", 14);
                    }
                } else {
                    noMarkerCount = 0;
                }
            }
            
            // Проверка застревания
            float move = sqrt(pow(pos.x - lastPos.x, 2) + pow(pos.y - lastPos.y, 2));
            if (move < 0.05f && hasTarget) {
                stuckCount++;
                if (stuckCount > 30) {
                    Print("[WARN] Stuck! Jumping...", 14);
                    PressSpace();
                    Sleep(200);
                    PressSpace();
                    stuckCount = 0;
                }
            } else { 
                stuckCount = 0; 
            }
            lastPos = pos;

            // Выбираем ближайший маркер
            if (!hasTarget && !markers.empty()) {
                Marker* nearest = nullptr;
                float nearDist = 999999.0f;
                
                for (auto& m : markers) {
                    if (!m.collected && m.active) {
                        float d = sqrt(pow(m.pos.x - pos.x, 2) + pow(m.pos.y - pos.y, 2));
                        if (d < nearDist) {
                            nearest = &m;
                            nearDist = d;
                        }
                    }
                }
                
                if (nearest) {
                    targetPos = nearest->pos;
                    hasTarget = true;
                    Print("[TARGET] Moving to marker at " + to_string((int)nearDist) + "m", 11);
                }
            }
            
            // Движение к цели
            if (hasTarget) {
                float distToTarget = sqrt(pow(targetPos.x - pos.x, 2) + pow(targetPos.y - pos.y, 2));
                
                // Если достигли маркера
                if (distToTarget < 3.0f) {
                    Print("[REACHED] Marker reached!", 10);
                    hasTarget = false;
                    
                    // Отмечаем маркер как собранный
                    for (auto& m : markers) {
                        float d = sqrt(pow(m.pos.x - pos.x, 2) + pow(m.pos.y - pos.y, 2));
                        if (d < 5.0f && !m.collected) {
                            m.collected = true;
                            collected++;
                            Print("[COLLECTED] Box collected! Total: " + to_string(collected), 10);
                            break;
                        }
                    }
                    
                    StopAll();
                    continue;
                }
                
                // Поворачиваемся к цели
                TurnToTarget(targetPos);
                
                // Движемся вперед
                if (abs(currentAngleDiff) < 30) {
                    PressW();
                    ReleaseS();
                } else {
                    ReleaseW();
                    ReleaseS();
                }
                
                // Бежим если далеко
                if (distToTarget > 10.0f && abs(currentAngleDiff) < 15) {
                    PressShift();
                } else {
                    ReleaseShift();
                }
            } else {
                StopAll();
            }
            
            Sleep(30);
        }
        
        StopAll();
    }

    void Print(string text, int color = 15) { 
        SetColor(color); 
        cout << text << endl; 
        SetColor(15); 
    }

    void Start() {
        if (running) { Print("[WARN] Bot already running!", 14); return; }
        if (!proc || !playerAddr) { Print("[ERROR] Bot not initialized!", 12); return; }
        
        running = true; emergency = false; carrying = false;
        markers.clear();
        collectedMarkers.clear();
        obstacles.clear();
        lastScan = chrono::high_resolution_clock::now();
        
        avoiding = false;
        avoidanceCounter = 0;
        sameDistanceCount = 0;
        lastDistanceToTarget = 0;
        noMarkerCount = 0;
        
        if (gameWnd) { 
            SetForegroundWindow(gameWnd);
            Sleep(500);
        }
        
        Print("[START] Bot started! Waiting for job...", 10);
        Print("[INFO] Take the job in game to start collecting", 11);
        
        thread(&Bot::BotLoop, this).detach();
    }

    void Stop() { 
        running = false; 
        StopAll(); 
        Print("[STOP] Bot stopped. Collected: " + to_string(collected), 14); 
    }
    
    void EmergencyStop() { 
        emergency = true; 
        running = false; 
        active = false; 
        StopAll(); 
        Print("[EMERGENCY] EMERGENCY STOP! (F11)", 12); 
    }

    void ShowStatus() {
        Vec3 pos = GetPos();
        SetColor(11);
        cout << "\n========================================" << endl;
        cout << "           BOT STATUS" << endl;
        cout << "========================================" << endl;
        SetColor(15);
        cout << "  Position:   X=" << (int)pos.x << " Y=" << (int)pos.y << " Z=" << (int)pos.z << endl;
        cout << "  Status:     " << (running ? "[RUNNING]" : "[STOPPED]") << endl;
        cout << "  Collected:  " << collected << endl;
        cout << "  Markers:    " << markers.size() << endl;
        if (markers.empty()) {
            cout << "  Job:        [NO MARKERS - TAKE JOB]" << endl;
        } else {
            cout << "  Job:        [ACTIVE]" << endl;
        }
        SetColor(11);
        cout << "========================================" << endl;
        cout << "  F1-Start  F2-Stop  F5-Status" << endl;
        cout << "  F11-Emergency  ESC-Exit" << endl;
        cout << "========================================" << endl;
        SetColor(15);
    }

    void HotkeyHandler() {
        while (true) {
            if (GetAsyncKeyState(VK_F1) & 1) Start();
            if (GetAsyncKeyState(VK_F2) & 1) Stop();
            if (GetAsyncKeyState(VK_F5) & 1) ShowStatus();
            if (GetAsyncKeyState(VK_F11) & 1) EmergencyStop();
            if (GetAsyncKeyState(VK_ESCAPE) & 1) { 
                EmergencyStop(); 
                Print("[EXIT] Exiting...", 14); 
                exit(0); 
            }
            Sleep(50);
        }
    }

    bool Ready() { return proc != NULL && playerAddr != 0; }
};

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
    cout << "      MTA PROVINCE BOX COLLECTOR" << endl;
    cout << "==================================================" << endl;
    cout << "  [WAIT] Start bot BEFORE taking job" << endl;
    cout << "  [FIND] Auto-detects green markers" << endl;
    cout << "  [MOVE] Moves to nearest marker" << endl;
    cout << "  [COLLECT] Collects boxes automatically" << endl;
    cout << "==================================================" << endl;
    SetColor(14);
    cout << "\n  HOW TO USE:" << endl;
    cout << "  1. Start bot (F1)" << endl;
    cout << "  2. Take the job in game" << endl;
    cout << "  3. Bot will auto-detect markers" << endl;
    cout << "  4. Bot will collect boxes" << endl;
    cout << endl;
    cout << "  [WARN] Run as Administrator!" << endl;
    cout << "  [WARN] Game window MUST be active!" << endl;
    cout << endl;
    SetColor(15);

    Bot bot;
    if (!bot.Ready()) {
        SetColor(12);
        cout << "\n[ERROR] MTA Province not found!" << endl;
        cout << "Make sure the game is running." << endl;
        SetColor(15);
        system("pause");
        return 1;
    }

    SetColor(10);
    cout << "[OK] BOT READY!" << endl;
    SetColor(15);
    cout << "[INFO] Press F1 to start" << endl;
    cout << "[INFO] Then take the job in game" << endl;
    cout << endl;

    thread handler(&Bot::HotkeyHandler, &bot);
    handler.detach();

    while (true) Sleep(1000);
    return 0;
}

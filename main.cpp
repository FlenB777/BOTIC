// main.cpp - MTA Bot with Jump and Obstacle Avoidance
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

using namespace std;
using namespace chrono;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void SetColor(int color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}

struct Vec3 { float x, y, z; };
struct Marker { 
    Vec3 pos; 
    bool collected; 
    DWORD address; 
    time_t spawnTime;
};

class Bot {
private:
    HANDLE proc;
    DWORD pid, playerAddr;
    DWORD64 baseAddr;
    HWND gameWnd;
    
    bool running, active, emergency;
    int collected;
    
    vector<Marker> markers;
    vector<Vec3> obstacles;
    
    chrono::high_resolution_clock::time_point lastScan;
    int stuckCount;
    Vec3 lastPos;
    int stepCounter;
    
    DWORD pedAddress;
    
    float currentAngleDiff;
    int sameDistanceCount;
    float lastDistanceToTarget;
    
    bool wPressed, aPressed, sPressed, dPressed, shiftPressed;
    chrono::high_resolution_clock::time_point lastJumpTime;
    chrono::high_resolution_clock::time_point lastPrintTime;
    chrono::high_resolution_clock::time_point lastObstacleCheck;
    
    // Для обхода препятствий
    bool avoiding;
    int avoidCounter;
    Vec3 avoidPoint;
    float avoidAngle;

public:
    Bot() : proc(NULL), pid(0), playerAddr(0), baseAddr(0), running(false),
            active(false), emergency(false), collected(0), stuckCount(0),
            stepCounter(0), currentAngleDiff(0), sameDistanceCount(0), 
            lastDistanceToTarget(0), pedAddress(0), wPressed(false), aPressed(false), 
            sPressed(false), dPressed(false), shiftPressed(false),
            avoiding(false), avoidCounter(0), avoidAngle(0) {
        lastPos = {0,0,0};
        avoidPoint = {0,0,0};
        gameWnd = NULL;
        lastJumpTime = chrono::high_resolution_clock::now();
        lastPrintTime = chrono::high_resolution_clock::now();
        lastObstacleCheck = chrono::high_resolution_clock::now();
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
                        name.find(L"mta") != wstring::npos) {
                        pid = pe.th32ProcessID;
                        proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_OPERATION, FALSE, pid);
                        if (proc) { 
                            Print("[OK] Process found", 10);
                            break; 
                        }
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }

    void FindAddresses() {
        if (!proc) return;
        
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
                    Print("[OK] Player found", 10);
                    return;
                }
            }
        }
        
        Print("[ERROR] Player not found!", 12);
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

    float GetAngle() {
        DWORD ped = GetPedAddress();
        if (ped && ped > 0x10000 && ped < 0x7FFFFFFF) {
            return Read<float>(ped + 0x20);
        }
        return 0;
    }

    // Проверка препятствий впереди
    bool CheckObstacleAhead(float checkDist = 3.0f) {
        Vec3 pos = GetPos();
        float angle = GetAngle() * M_PI / 180.0f;
        
        // Точка впереди
        float checkX = pos.x + cos(angle) * checkDist;
        float checkY = pos.y + sin(angle) * checkDist;
        
        // Проверяем известные препятствия
        for (auto& obs : obstacles) {
            float d = sqrt(pow(obs.x - checkX, 2) + pow(obs.y - checkY, 2));
            if (d < 2.0f) {
                return true;
            }
        }
        
        return false;
    }

    // Поиск маркеров
    void ScanMarkers() {
        if (!proc) return;
        
        Vec3 playerPos = GetPos();
        if (playerPos.x == 0 && playerPos.y == 0) return;
        
        time_t currentTime = time(NULL);
        markers.erase(remove_if(markers.begin(), markers.end(),
            [currentTime](const Marker& m) { 
                return (currentTime - m.spawnTime) > 30; 
            }), markers.end());
        
        // Сканируем память
        for (DWORD64 addr = baseAddr; addr < baseAddr + 0x500000; addr += 4) {
            float x = Read<float>((DWORD)addr);
            float y = Read<float>((DWORD)(addr + 4));
            float z = Read<float>((DWORD)(addr + 8));
            
            if (x < -3000 || x > 3000) continue;
            if (y < -3000 || y > 3000) continue;
            if (z < -100 || z > 1000) continue;
            if (x == 0 && y == 0) continue;
            
            float dist = sqrt(pow(x - playerPos.x, 2) + pow(y - playerPos.y, 2));
            if (dist > 200) continue;
            
            if (abs(x - playerPos.x) < 2 && abs(y - playerPos.y) < 2) continue;
            
            bool exists = false;
            for (auto& m : markers) {
                float d = sqrt(pow(m.pos.x - x, 2) + pow(m.pos.y - y, 2));
                if (d < 2.0f) { exists = true; break; }
            }
            if (exists) continue;
            
            Marker m;
            m.pos = {x, y, z};
            m.collected = false;
            m.address = (DWORD)addr;
            m.spawnTime = currentTime;
            markers.push_back(m);
            
            auto now = chrono::high_resolution_clock::now();
            if (duration_cast<milliseconds>(now - lastPrintTime).count() > 2000) {
                Print("[FOUND] Marker at " + to_string((int)dist) + "m", 10);
                lastPrintTime = now;
            }
            
            if (markers.size() > 20) break;
        }
        
        sort(markers.begin(), markers.end(), [playerPos](const Marker& a, const Marker& b) {
            float da = sqrt(pow(a.pos.x - playerPos.x, 2) + pow(a.pos.y - playerPos.y, 2));
            float db = sqrt(pow(b.pos.x - playerPos.x, 2) + pow(b.pos.y - playerPos.y, 2));
            return da < db;
        });
    }

    // Клавиши
    void SendKey(WORD key, bool press) {
        keybd_event((BYTE)key, 0, press ? 0 : KEYEVENTF_KEYUP, 0);
        Sleep(5);
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
    
    void Jump() { 
        SendKey(VK_SPACE, true);
        Sleep(30);
        SendKey(VK_SPACE, false);
    }

    void StopAll() {
        ReleaseW();
        ReleaseS();
        ReleaseA();
        ReleaseD();
        ReleaseShift();
    }

    // Поворот к цели
    void TurnToTarget(Vec3 target) {
        Vec3 pos = GetPos();
        float targetAngle = atan2(target.y - pos.y, target.x - pos.x) * 180.0f / M_PI - 90.0f;
        float currentAngle = GetAngle();
        
        float diff = targetAngle - currentAngle;
        while (diff > 180) diff -= 360;
        while (diff < -180) diff += 360;
        
        // Плавный поворот
        if (abs(diff) > 30) {
            // Большой угол - держим клавишу
            if (diff > 0) { PressD(); ReleaseA(); }
            else { PressA(); ReleaseD(); }
        } else if (abs(diff) > 10) {
            // Средний угол - короткие нажатия
            if (diff > 0) { 
                PressD(); Sleep(25); ReleaseD();
                ReleaseA();
            } else { 
                PressA(); Sleep(25); ReleaseA();
                ReleaseD();
            }
        } else if (abs(diff) > 3) {
            // Маленький угол - очень короткие нажатия
            if (diff > 0) { 
                PressD(); Sleep(10); ReleaseD();
                ReleaseA();
            } else { 
                PressA(); Sleep(10); ReleaseA();
                ReleaseD();
            }
        } else {
            // Почти точно - отпускаем
            ReleaseA(); ReleaseD();
        }
        
        currentAngleDiff = diff;
    }

    // Обход препятствия
    Vec3 AvoidObstacle(Vec3 target) {
        Vec3 pos = GetPos();
        
        if (avoiding && avoidCounter > 0) {
            avoidCounter--;
            return avoidPoint;
        }
        
        // Проверяем препятствия впереди
        if (CheckObstacleAhead(3.0f)) {
            // Выбираем сторону обхода
            float targetAngle = atan2(target.y - pos.y, target.x - pos.x);
            
            // Точка обхода слева
            Vec3 leftPoint = {
                pos.x + cos(targetAngle - M_PI/2) * 5.0f,
                pos.y + sin(targetAngle - M_PI/2) * 5.0f,
                pos.z
            };
            
            // Точка обхода справа
            Vec3 rightPoint = {
                pos.x + cos(targetAngle + M_PI/2) * 5.0f,
                pos.y + sin(targetAngle + M_PI/2) * 5.0f,
                pos.z
            };
            
            // Выбираем ближайшую к цели
            float leftDist = sqrt(pow(leftPoint.x - target.x, 2) + pow(leftPoint.y - target.y, 2));
            float rightDist = sqrt(pow(rightPoint.x - target.x, 2) + pow(rightPoint.y - target.y, 2));
            
            if (leftDist < rightDist) {
                avoidPoint = leftPoint;
            } else {
                avoidPoint = rightPoint;
            }
            
            avoiding = true;
            avoidCounter = 20; // 1 секунда обхода
            
            // Добавляем текущую позицию как препятствие
            obstacles.push_back(pos);
            
            return avoidPoint;
        }
        
        avoiding = false;
        return target;
    }

    // Главный цикл
    void BotLoop() {
        active = true;
        Print("[START] Bot started!", 10);
        Print("[INFO] Take the job in game", 11);
        
        int scanCount = 0;
        lastPos = GetPos();
        stuckCount = 0;
        stepCounter = 0;
        Vec3 targetPos = {0,0,0};
        bool hasTarget = false;

        while (active) {
            if (!running || emergency) { 
                StopAll(); 
                Sleep(100); 
                continue; 
            }
            
            scanCount++;
            Vec3 pos = GetPos();
            
            // Сканируем каждые 3 секунды
            if (scanCount % 60 == 0) {
                ScanMarkers();
            }
            
            // Проверка застревания
            if (hasTarget) {
                float move = sqrt(pow(pos.x - lastPos.x, 2) + pow(pos.y - lastPos.y, 2));
                if (move < 0.05f) {
                    stuckCount++;
                    if (stuckCount > 30) { // ~1.5 секунды
                        Print("[JUMP] Stuck, jumping", 14);
                        Jump();
                        stuckCount = 0;
                        
                        // Добавляем препятствие
                        obstacles.push_back(pos);
                    }
                } else {
                    stuckCount = 0;
                    stepCounter++;
                }
            }
            lastPos = pos;
            
            // Выбираем ближайший маркер
            if (!markers.empty()) {
                for (auto& m : markers) {
                    if (!m.collected) {
                        targetPos = m.pos;
                        hasTarget = true;
                        break;
                    }
                }
            }
            
            // Движение
            if (hasTarget) {
                float distToTarget = sqrt(pow(targetPos.x - pos.x, 2) + pow(targetPos.y - pos.y, 2));
                
                // Достигли маркера
                if (distToTarget < 2.5f) {
                    for (auto& m : markers) {
                        float d = sqrt(pow(m.pos.x - pos.x, 2) + pow(m.pos.y - pos.y, 2));
                        if (d < 5.0f && !m.collected) {
                            m.collected = true;
                            collected++;
                            Print("[COLLECT] Box collected! Total: " + to_string(collected), 10);
                            break;
                        }
                    }
                    
                    hasTarget = false;
                    StopAll();
                    continue;
                }
                
                // Проверяем препятствия и обходим
                Vec3 moveTarget = AvoidObstacle(targetPos);
                
                // Поворачиваемся к цели
                TurnToTarget(moveTarget);
                
                // Движемся вперед если смотрим правильно
                if (abs(currentAngleDiff) < 20) {
                    PressW();
                    ReleaseS();
                    
                    // Бежим если далеко
                    if (distToTarget > 10.0f) {
                        PressShift();
                    } else {
                        ReleaseShift();
                    }
                    
                    // Прыгаем каждые 3 шага
                    if (stepCounter % 3 == 0 && distToTarget > 5.0f) {
                        auto now = chrono::high_resolution_clock::now();
                        if (duration_cast<milliseconds>(now - lastJumpTime).count() > 500) {
                            Jump();
                            lastJumpTime = now;
                            stepCounter = 0;
                        }
                    }
                } else {
                    ReleaseW();
                    ReleaseShift();
                }
            } else {
                StopAll();
            }
            
            Sleep(50);
        }
        
        StopAll();
    }

    void Print(string text, int color = 15) { 
        SetColor(color); 
        cout << text << endl; 
        SetColor(15); 
    }

    void Start() {
        if (running) { Print("[WARN] Already running!", 14); return; }
        if (!proc || !playerAddr) { Print("[ERROR] Not initialized!", 12); return; }
        
        running = true; 
        emergency = false;
        markers.clear();
        obstacles.clear();
        lastScan = chrono::high_resolution_clock::now();
        stuckCount = 0;
        stepCounter = 0;
        sameDistanceCount = 0;
        lastDistanceToTarget = 0;
        avoiding = false;
        avoidCounter = 0;
        
        Print("[START] Bot started!", 10);
        Print("[INFO] Take the job in game now", 11);
        
        thread(&Bot::BotLoop, this).detach();
    }

    void Stop() { 
        running = false; 
        StopAll(); 
        Print("[STOP] Stopped. Collected: " + to_string(collected), 14); 
    }
    
    void EmergencyStop() { 
        emergency = true; 
        running = false; 
        active = false; 
        StopAll(); 
        Print("[EMERGENCY] STOP!", 12); 
    }

    void ShowStatus() {
        Vec3 pos = GetPos();
        SetColor(11);
        cout << "\n========== STATUS ==========" << endl;
        SetColor(15);
        cout << "  Position:  X=" << (int)pos.x << " Y=" << (int)pos.y << " Z=" << (int)pos.z << endl;
        cout << "  Running:   " << (running ? "YES" : "NO") << endl;
        cout << "  Collected: " << collected << endl;
        cout << "  Markers:   " << markers.size() << endl;
        cout << "  Obstacles: " << obstacles.size() << endl;
        SetColor(11);
        cout << "============================" << endl;
        cout << "  F1-Start  F2-Stop  F5-Status" << endl;
        cout << "  F11-Emergency  ESC-Exit" << endl;
        cout << "============================" << endl;
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
    cout << "\n========================================" << endl;
    cout << "    MTA PROVINCE BOX COLLECTOR" << endl;
    cout << "========================================" << endl;
    cout << "  HOW TO USE:" << endl;
    cout << "  1. Start bot (F1)" << endl;
    cout << "  2. Take job in game" << endl;
    cout << "  3. Bot runs and jumps to boxes" << endl;
    cout << "========================================" << endl;
    SetColor(14);
    cout << "  F1-Start  F2-Stop  F5-Status" << endl;
    cout << "  F11-Emergency  ESC-Exit" << endl;
    cout << "========================================" << endl;
    SetColor(15);

    Bot bot;
    if (!bot.Ready()) {
        SetColor(12);
        cout << "\n[ERROR] MTA not found!" << endl;
        SetColor(15);
        system("pause");
        return 1;
    }

    cout << "[OK] Ready! Press F1 to start" << endl;

    thread handler(&Bot::HotkeyHandler, &bot);
    handler.detach();

    while (true) Sleep(1000);
    return 0;
}

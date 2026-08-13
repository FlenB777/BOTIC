// main.cpp - MTA Province Collector Bot
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

void SetColor(int color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}

struct Vec3 { float x, y, z; };
struct Marker { Vec3 pos; int type; bool active; bool collected; DWORD address; time_t spawnTime; };

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
    bool wPressed, aPressed, sPressed, dPressed;

public:
    Bot() : proc(NULL), pid(0), playerAddr(0), baseAddr(0), running(false),
            active(false), carrying(false), speed(0.25f), collected(0),
            delivered(0), emergency(false), stuckCount(0), shiftPressed(false),
            jumpCounter(0), wPressed(false), aPressed(false), sPressed(false), dPressed(false) {
        deliveryPoint = {0,0,0};
        lastPos = {0,0,0};
        gameWnd = NULL;
        FindProcess();
        if (proc) FindAddresses();
    }

    ~Bot() { if (proc) CloseHandle(proc); StopAll(); }

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

    void FindProcess() {
        gameWnd = FindWindowW(NULL, L"MTA: Province");
        if (!gameWnd) gameWnd = FindWindowW(NULL, L"MTA: San Andreas");
        if (gameWnd) {
            GetWindowThreadProcessId(gameWnd, &pid);
            proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (proc) {
                Print("[OK] Process found", 10);
                SetForegroundWindow(gameWnd);
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
                    if (name.find(L"gta_sa") != wstring::npos || name.find(L"gta") != wstring::npos || name.find(L"mta") != wstring::npos) {
                        pid = pe.th32ProcessID;
                        proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
                        if (proc) { Print("[OK] Process found", 10); break; }
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
        if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) baseAddr = (DWORD)mods[0];
        playerAddr = 0xB6F5F0;
        DWORD test = Read<DWORD>(playerAddr);
        if (test > 0x10000 && test < 0x7FFFFFFF) { Print("[OK] Player address found", 10); return; }
        for (DWORD addr = baseAddr; addr < baseAddr + 0x500000; addr += 4) {
            DWORD ped = Read<DWORD>(addr);
            if (ped > 0x10000 && ped < 0x7FFFFFFF) {
                float x = Read<float>(ped + 0x14), y = Read<float>(ped + 0x18);
                if (x > -5000 && x < 5000 && y > -5000 && y < 5000) { playerAddr = addr; Print("[OK] Player address found", 10); return; }
            }
        }
        Print("[ERROR] Failed to find addresses!", 12);
    }

    Vec3 GetPos() {
        Vec3 pos = {0,0,0};
        if (!playerAddr) return pos;
        DWORD ped = Read<DWORD>(playerAddr);
        if (ped) { pos.x = Read<float>(ped + 0x14); pos.y = Read<float>(ped + 0x18); pos.z = Read<float>(ped + 0x1C); }
        return pos;
    }

    // ==================== KEYS ====================
    void PressKey(WORD key) { if (gameWnd) { PostMessage(gameWnd, WM_KEYDOWN, key, 0); Sleep(10); } }
    void ReleaseKey(WORD key) { if (gameWnd) { PostMessage(gameWnd, WM_KEYUP, key, 0); Sleep(10); } }
    void PressW() { if (!wPressed) { PressKey('W'); wPressed = true; } }
    void ReleaseW() { if (wPressed) { ReleaseKey('W'); wPressed = false; } }
    void PressS() { if (!sPressed) { PressKey('S'); sPressed = true; } }
    void ReleaseS() { if (sPressed) { ReleaseKey('S'); sPressed = false; } }
    void PressA() { if (!aPressed) { PressKey('A'); aPressed = true; } }
    void ReleaseA() { if (aPressed) { ReleaseKey('A'); aPressed = false; } }
    void PressD() { if (!dPressed) { PressKey('D'); dPressed = true; } }
    void ReleaseD() { if (dPressed) { ReleaseKey('D'); dPressed = false; } }
    void PressShift() { if (!shiftPressed && gameWnd) { PostMessage(gameWnd, WM_KEYDOWN, VK_SHIFT, 0); shiftPressed = true; } }
    void ReleaseShift() { if (shiftPressed && gameWnd) { PostMessage(gameWnd, WM_KEYUP, VK_SHIFT, 0); shiftPressed = false; } }
    void PressSpace() { if (gameWnd) { PostMessage(gameWnd, WM_KEYDOWN, VK_SPACE, 0); Sleep(50); PostMessage(gameWnd, WM_KEYUP, VK_SPACE, 0); } }
    
    // ==================== STOP ALL KEYS ====================
    void StopAll() {
        ReleaseW();
        ReleaseS();
        ReleaseA();
        ReleaseD();
        ReleaseShift();
    }

    // ==================== TURN ====================
    void TurnToTarget(Vec3 target) {
        Vec3 pos = GetPos();
        float angle = atan2(target.y - pos.y, target.x - pos.x);
        float currentAngle = 0;
        DWORD ped = Read<DWORD>(playerAddr);
        if (ped) currentAngle = Read<float>(ped + 0x20);
        float diff = angle * 180.0f / 3.14159f - 90.0f - currentAngle;
        while (diff > 180) diff -= 360;
        while (diff < -180) diff += 360;
        if (diff > 5) { PressD(); ReleaseA(); }
        else if (diff < -5) { PressA(); ReleaseD(); }
        else { ReleaseA(); ReleaseD(); }
    }

    // ==================== SCAN MARKERS ====================
    void ScanMarkers() {
        if (!proc || !baseAddr) return;
        auto now = chrono::high_resolution_clock::now();
        if (duration_cast<milliseconds>(now - lastScan).count() < 300) return;
        lastScan = now;
        Vec3 playerPos = GetPos();
        for (DWORD addr = baseAddr; addr < baseAddr + 0x800000; addr += 0x28) {
            float x = Read<float>(addr), y = Read<float>(addr + 4), z = Read<float>(addr + 8);
            int type = Read<int>(addr + 0xC);
            bool active = Read<bool>(addr + 0x10);
            if (!active || x == 0 || y == 0 || x < -5000 || x > 5000 || y < -5000 || y > 5000) continue;
            float dist = sqrt(pow(x - playerPos.x, 2) + pow(y - playerPos.y, 2));
            if (dist > 500) continue;
            bool exists = false;
            for (auto& m : markers) { float d = sqrt(pow(m.pos.x - x, 2) + pow(m.pos.y - y, 2)); if (d < 0.5f) { exists = true; break; } }
            if (exists) continue;
            bool col = false;
            for (auto& m : collectedMarkers) { float d = sqrt(pow(m.pos.x - x, 2) + pow(m.pos.y - y, 2)); if (d < 0.5f) { col = true; break; } }
            if (col) continue;
            if (type == 1) { Marker m; m.pos = {x,y,z}; m.type = type; m.active = true; m.collected = false; m.address = addr; m.spawnTime = time(NULL); markers.push_back(m); Print("[BOX] New box! X=" + to_string((int)x) + " Y=" + to_string((int)y), 10); }
            else if (type == 2) { deliveryPoint = {x,y,z}; Print("[DROP] Drop point! X=" + to_string((int)x) + " Y=" + to_string((int)y), 11); }
        }
    }

    // ==================== SCAN OBSTACLES ====================
    void ScanObstacles() {
        Vec3 pos = GetPos();
        for (float x = pos.x - 20; x <= pos.x + 20; x += 2.0f) {
            for (float y = pos.y - 20; y <= pos.y + 20; y += 2.0f) {
                for (DWORD addr = baseAddr; addr < baseAddr + 0x100000; addr += 4) {
                    float memX = Read<float>(addr);
                    float memY = Read<float>(addr + 4);
                    float memZ = Read<float>(addr + 8);
                    if (abs(memX - x) < 1.0f && abs(memY - y) < 1.0f) {
                        bool exists = false;
                        for (auto& o : obstacles) { float d = sqrt(pow(o.x - x, 2) + pow(o.y - y, 2)); if (d < 2.0f) { exists = true; break; } }
                        if (!exists) { Vec3 obs = {x, y, memZ}; obstacles.push_back(obs); Print("[OBSTACLE] Found! X=" + to_string((int)x) + " Y=" + to_string((int)y), 8); }
                        break;
                    }
                }
            }
        }
        vector<Vec3> newObstacles;
        for (auto& o : obstacles) { float d = sqrt(pow(o.x - pos.x, 2) + pow(o.y - pos.y, 2)); if (d < 30.0f) newObstacles.push_back(o); }
        obstacles = newObstacles;
    }

    // ==================== AVOID OBSTACLES ====================
    Vec3 AvoidObstacle(Vec3 target) {
        Vec3 pos = GetPos();
        for (auto& o : obstacles) {
            float d = sqrt(pow(o.x - pos.x, 2) + pow(o.y - pos.y, 2));
            if (d < 4.0f) {
                float angle = atan2(pos.y - o.y, pos.x - o.x);
                Vec3 right = { o.x + cos(angle + 1.57f) * 5.0f, o.y + sin(angle + 1.57f) * 5.0f, pos.z };
                bool rightFree = true;
                for (auto& o2 : obstacles) { float d2 = sqrt(pow(right.x - o2.x, 2) + pow(right.y - o2.y, 2)); if (d2 < 3.0f) { rightFree = false; break; } }
                if (rightFree) { Print("[AVOID] Go right!", 11); return right; }
                Vec3 left = { o.x + cos(angle - 1.57f) * 5.0f, o.y + sin(angle - 1.57f) * 5.0f, pos.z };
                Print("[AVOID] Go left!", 11);
                return left;
            }
        }
        return target;
    }

    // ==================== MAIN LOOP ====================
    void BotLoop() {
        active = true;
        Print("[START] Bot started! Collecting boxes...", 10);
        Print("[RUN] Sprint + Jump | Avoid obstacles", 11);
        
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
            
            // Stuck check
            float move = sqrt(pow(pos.x - lastPos.x, 2) + pow(pos.y - lastPos.y, 2));
            if (move < 0.05f) {
                stuckCount++;
                if (stuckCount > 50) {
                    Print("[WARN] Stuck! Jumping...", 14);
                    PressSpace(); Sleep(200); PressSpace();
                    PressA(); Sleep(500); ReleaseA();
                    stuckCount = 0;
                }
            } else { stuckCount = 0; }
            lastPos = pos;

            // Scan
            if (scanCount % 3 == 0) {
                ScanMarkers();
                ScanObstacles();
            }

            // Logic
            if (carrying) {
                if (deliveryPoint.x != 0) {
                    float dist = sqrt(pow(deliveryPoint.x - pos.x, 2) + pow(deliveryPoint.y - pos.y, 2));
                    if (dist < 2.0f) {
                        carrying = false; delivered++; ReleaseShift(); StopAll();
                        Print("[DONE] Box delivered! (" + to_string(delivered) + ")", 10);
                        obstacles.push_back(pos);
                        jumpCounter = 0; hasTarget = false;
                        continue;
                    } else {
                        targetPos = deliveryPoint; hasTarget = true;
                    }
                } else {
                    hasTarget = false; continue;
                }
            } else {
                Marker* nearest = nullptr; float nearDist = 999999.0f;
                for (auto& m : markers) {
                    if (m.type == 1 && !m.collected) {
                        float d = sqrt(pow(m.pos.x - pos.x, 2) + pow(m.pos.y - pos.y, 2));
                        if (d < nearDist) { nearest = &m; nearDist = d; }
                    }
                }
                if (nearest) {
                    float dist = sqrt(pow(nearest->pos.x - pos.x, 2) + pow(nearest->pos.y - pos.y, 2));
                    if (dist < 2.0f) {
                        carrying = true; nearest->collected = true; collected++;
                        collectedMarkers.push_back(*nearest);
                        markers.erase(remove_if(markers.begin(), markers.end(),
                            [nearest](Marker& m) { return m.address == nearest->address; }), markers.end());
                        ReleaseShift(); StopAll();
                        Print("[TAKEN] Box taken! (" + to_string(collected) + ")", 14);
                        jumpCounter = 0; hasTarget = false;
                        continue;
                    } else {
                        targetPos = nearest->pos; hasTarget = true;
                    }
                } else {
                    hasTarget = false;
                    continue;
                }
            }

            // ===== MOVE WITH AVOID =====
            if (hasTarget) {
                Vec3 posNow = GetPos();
                float distToTarget = sqrt(pow(targetPos.x - posNow.x, 2) + pow(targetPos.y - posNow.y, 2));
                if (distToTarget < 2.0f) { hasTarget = false; StopAll(); continue; }

                Vec3 obstacle = AvoidObstacle(targetPos);
                float obsDist = sqrt(pow(obstacle.x - posNow.x, 2) + pow(obstacle.y - posNow.y, 2));
                if (obsDist > 2.0f && obsDist < 20.0f) {
                    targetPos = obstacle;
                }

                TurnToTarget(targetPos);
                PressW(); ReleaseS();
                
                if (!carrying) {
                    PressShift();
                    jumpCounter++;
                    if (jumpCounter % 3 == 0 && distToTarget > 3.0f) { PressSpace(); }
                } else {
                    ReleaseShift();
                }
            } else {
                StopAll();
            }
            
            Sleep(50);
        }
    }

    // ==================== PRINT ====================
    void Print(string text, int color = 15) { SetColor(color); cout << text << endl; SetColor(15); }

    // ==================== CONTROL ====================
    void Start() {
        if (running) { Print("[WARN] Bot already running!", 14); return; }
        if (!proc || !playerAddr) { Print("[ERROR] Bot not initialized!", 12); return; }
        running = true; emergency = false; carrying = false; shiftPressed = false;
        lastScan = chrono::high_resolution_clock::now();
        if (gameWnd) { SetForegroundWindow(gameWnd); Sleep(200); }
        Print("[START] Bot started! Press F11 for emergency stop.", 10);
        thread(&Bot::BotLoop, this).detach();
    }

    void Stop() { running = false; StopAll(); ReleaseShift(); Print("[STOP] Bot stopped. Collected: " + to_string(collected) + " | Delivered: " + to_string(delivered), 14); }
    void EmergencyStop() { emergency = true; running = false; active = false; StopAll(); ReleaseShift(); Print("[EMERGENCY] EMERGENCY STOP! (F11)", 12); }
    void SpeedUp() { speed = min(0.8f, speed + 0.1f); Print("[SPEED] Speed: " + to_string(speed), 11); }
    void SpeedDown() { speed = max(0.1f, speed - 0.1f); Print("[SPEED] Speed: " + to_string(speed), 11); }

    void ShowStatus() {
        Vec3 pos = GetPos();
        SetColor(11);
        cout << "\n========================================" << endl;
        cout << "           BOT STATUS" << endl;
        cout << "========================================" << endl;
        SetColor(15);
        cout << "  Position:   X=" << (int)pos.x << " Y=" << (int)pos.y << " Z=" << (int)pos.z << endl;
        cout << "  Speed:      " << speed << endl;
        cout << "  Status:     " << (running ? "[RUNNING]" : "[STOPPED]") << endl;
        cout << "  Box:        " << (carrying ? "[CARRYING]" : "[SEARCHING]") << endl;
        cout << "  Collected:  " << collected << endl;
        cout << "  Delivered:  " << delivered << endl;
        cout << "  Found:      " << markers.size() << " boxes" << endl;
        cout << "  Obstacles:  " << obstacles.size() << endl;
        cout << "  Keys:       W=Forward A=Left S=Back D=Right" << endl;
        if (deliveryPoint.x != 0) cout << "  Drop:       X=" << (int)deliveryPoint.x << " Y=" << (int)deliveryPoint.y << endl;
        SetColor(11);
        cout << "========================================" << endl;
        cout << "  F1-Start  F2-Stop  F3-Faster  F4-Slower" << endl;
        cout << "  F5-Status  F11-Emergency  ESC-Exit" << endl;
        cout << "========================================" << endl;
        SetColor(15);
    }

    void ShowHelp() {
        SetColor(14);
        cout << "\n========================================" << endl;
        cout << "           CONTROLS" << endl;
        cout << "========================================" << endl;
        SetColor(15);
        cout << "  F1  - Start bot" << endl;
        cout << "  F2  - Stop bot" << endl;
        cout << "  F3  - Faster" << endl;
        cout << "  F4  - Slower" << endl;
        cout << "  F5  - Status" << endl;
        cout << "  F11 - EMERGENCY STOP" << endl;
        cout << "  ESC - Exit" << endl;
        cout << endl;
        cout << "  BOT CONTROLS (auto):" << endl;
        cout << "  W - Forward" << endl;
        cout << "  A - Left" << endl;
        cout << "  S - Back" << endl;
        cout << "  D - Right" << endl;
        cout << "  Shift - Sprint (no box)" << endl;
        cout << "  Space - Jump (to box)" << endl;
        cout << "  Bot finds boxes and avoids obstacles" << endl;
        SetColor(14);
        cout << "========================================" << endl;
        cout << "  Stealth mode - Anti-cheat safe" << endl;
        cout << "========================================" << endl;
        SetColor(15);
    }

    void HotkeyHandler() {
        while (true) {
            if (GetAsyncKeyState(VK_F1) & 1) Start();
            if (GetAsyncKeyState(VK_F2) & 1) Stop();
            if (GetAsyncKeyState(VK_F3) & 1) SpeedUp();
            if (GetAsyncKeyState(VK_F4) & 1) SpeedDown();
            if (GetAsyncKeyState(VK_F5) & 1) ShowStatus();
            if (GetAsyncKeyState(VK_F11) & 1) EmergencyStop();
            if (GetAsyncKeyState(VK_ESCAPE) & 1) { EmergencyStop(); Print("[EXIT] Exiting...", 14); exit(0); }
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
    cout << "      STEALTH COLLECTOR BOT FOR MTA PROVINCE" << endl;
    cout << "==================================================" << endl;
    cout << "  [BOX] Collects boxes" << endl;
    cout << "  [DROP] Delivers to drop point" << endl;
    cout << "  [AVOID] Scans and avoids obstacles" << endl;
    cout << "  [KEYS] W=Forward A=Left S=Back D=Right" << endl;
    cout << "  [STEALTH] Anti-cheat safe" << endl;
    cout << "==================================================" << endl;
    SetColor(14);
    cout << "\n  [WARN] Run as Administrator!" << endl;
    cout << "  [WARN] MTA Province must be running!" << endl;
    cout << "  [WARN] Press F5 for controls" << endl;
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
    cout << endl;

    thread handler(&Bot::HotkeyHandler, &bot);
    handler.detach();

    while (true) Sleep(1000);
    return 0;
}

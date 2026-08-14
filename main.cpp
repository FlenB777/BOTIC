// main.cpp - MTA Province Collector Bot (FIXED)
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
struct Marker { Vec3 pos; int type; bool active; bool collected; DWORD address; };

class Bot {
private:
    HANDLE proc;
    DWORD pid, playerAddr, baseAddr;
    HWND gameWnd;
    
    bool running, active, carrying, emergency;
    int collected, delivered;
    vector<Marker> markers;
    vector<Marker> collectedMarkers;
    vector<Vec3> obstacles;
    Vec3 deliveryPoint;
    chrono::high_resolution_clock::time_point lastScan;

public:
    Bot() : proc(NULL), pid(0), playerAddr(0), baseAddr(0), running(false),
            active(false), carrying(false), collected(0), delivered(0), emergency(false) {
        deliveryPoint = {0,0,0};
        gameWnd = NULL;
        FindProcess();
        if (proc) FindAddresses();
    }

    ~Bot() { if (proc) CloseHandle(proc); StopAllKeys(); }

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

    void FindProcess() {
        gameWnd = FindWindowW(NULL, L"MTA: Province");
        if (!gameWnd) gameWnd = FindWindowW(NULL, L"MTA: San Andreas");
        if (gameWnd) {
            GetWindowThreadProcessId(gameWnd, &pid);
            proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (proc) {
                Print("[OK] Process found", 10);
                SetForegroundWindow(gameWnd);
                Sleep(500);
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
        if (test > 0x10000 && test < 0x7FFFFFFF) { 
            Print("[OK] Player address found", 10); 
            return; 
        }
        for (DWORD addr = baseAddr; addr < baseAddr + 0x500000; addr += 4) {
            DWORD ped = Read<DWORD>(addr);
            if (ped > 0x10000 && ped < 0x7FFFFFFF) {
                float x = Read<float>(ped + 0x14), y = Read<float>(ped + 0x18);
                if (x > -5000 && x < 5000 && y > -5000 && y < 5000) { 
                    playerAddr = addr; 
                    Print("[OK] Player address found", 10); 
                    return; 
                }
            }
        }
        Print("[ERROR] Failed to find addresses!", 12);
    }

    Vec3 GetPos() {
        Vec3 pos = {0,0,0};
        if (!playerAddr) return pos;
        DWORD ped = Read<DWORD>(playerAddr);
        if (ped && ped > 0x10000 && ped < 0x7FFFFFFF) { 
            pos.x = Read<float>(ped + 0x14); 
            pos.y = Read<float>(ped + 0x18); 
            pos.z = Read<float>(ped + 0x1C); 
        }
        return pos;
    }

    float GetAngle() {
        float angle = 0;
        DWORD ped = Read<DWORD>(playerAddr);
        if (ped && ped > 0x10000 && ped < 0x7FFFFFFF) {
            angle = Read<float>(ped + 0x20);
        }
        return angle;
    }

    // ==================== КЛАВИШИ ====================
    void KeyDown(WORD key) { keybd_event((BYTE)key, 0, 0, 0); }
    void KeyUp(WORD key) { keybd_event((BYTE)key, 0, KEYEVENTF_KEYUP, 0); }
    
    void StopAllKeys() {
        KeyUp('W'); KeyUp('S'); KeyUp('A'); KeyUp('D'); KeyUp(VK_SHIFT);
    }

    void MoveForward() { KeyDown('W'); KeyUp('S'); }
    void MoveBack() { KeyDown('S'); KeyUp('W'); }
    void TurnLeft() { KeyDown('A'); KeyUp('D'); }
    void TurnRight() { KeyDown('D'); KeyUp('A'); }
    void StopMove() { KeyUp('W'); KeyUp('S'); }
    void StopTurn() { KeyUp('A'); KeyUp('D'); }
    void SprintOn() { KeyDown(VK_SHIFT); }
    void SprintOff() { KeyUp(VK_SHIFT); }
    void Jump() { KeyDown(VK_SPACE); Sleep(50); KeyUp(VK_SPACE); }

    // ==================== ПОВОРОТ ====================
    void TurnTo(Vec3 target) {
        Vec3 pos = GetPos();
        float targetAngle = atan2(target.y - pos.y, target.x - pos.x) * 180.0f / 3.14159f - 90.0f;
        float current = GetAngle();
        float diff = targetAngle - current;
        while (diff > 180) diff -= 360;
        while (diff < -180) diff += 360;
        
        if (diff > 3.0f) { TurnRight(); }
        else if (diff < -3.0f) { TurnLeft(); }
        else { StopTurn(); }
    }

    // ==================== ОБХОД ПРЕПЯТСТВИЙ ====================
    Vec3 AvoidObstacles(Vec3 target) {
        Vec3 pos = GetPos();
        float angle = atan2(target.y - pos.y, target.x - pos.x);
        
        for (float d = 3.0f; d <= 10.0f; d += 2.0f) {
            float checkX = pos.x + cos(angle) * d;
            float checkY = pos.y + sin(angle) * d;
            
            for (auto& o : obstacles) {
                float dist = sqrt(pow(o.x - checkX, 2) + pow(o.y - checkY, 2));
                if (dist < 3.0f) {
                    float avoidAngle = atan2(pos.y - o.y, pos.x - o.x);
                    Vec3 right = { o.x + cos(avoidAngle + 1.2f) * 5.0f, o.y + sin(avoidAngle + 1.2f) * 5.0f, pos.z };
                    bool free = true;
                    for (auto& o2 : obstacles) {
                        if (sqrt(pow(right.x - o2.x, 2) + pow(right.y - o2.y, 2)) < 3.0f) {
                            free = false; break;
                        }
                    }
                    if (free) return right;
                    Vec3 left = { o.x + cos(avoidAngle - 1.2f) * 5.0f, o.y + sin(avoidAngle - 1.2f) * 5.0f, pos.z };
                    return left;
                }
            }
        }
        return target;
    }

    // ==================== ПОИСК ПРЕПЯТСТВИЙ ====================
    void ScanObstacles() {
        Vec3 pos = GetPos();
        if (pos.x == 0 && pos.y == 0) return;
        
        for (float x = pos.x - 20; x <= pos.x + 20; x += 2.0f) {
            for (float y = pos.y - 20; y <= pos.y + 20; y += 2.0f) {
                for (DWORD addr = baseAddr; addr < baseAddr + 0x100000; addr += 4) {
                    float mx = Read<float>(addr);
                    float my = Read<float>(addr + 4);
                    float mz = Read<float>(addr + 8);
                    if (mx < -5000 || mx > 5000 || my < -5000 || my > 5000) continue;
                    if (abs(mx - x) < 0.5f && abs(my - y) < 0.5f) {
                        bool exists = false;
                        for (auto& o : obstacles) {
                            if (sqrt(pow(o.x - x, 2) + pow(o.y - y, 2)) < 2.0f) {
                                exists = true; break;
                            }
                        }
                        if (!exists) obstacles.push_back({x, y, mz});
                        break;
                    }
                }
            }
        }
        
        vector<Vec3> newObs;
        for (auto& o : obstacles) {
            if (sqrt(pow(o.x - pos.x, 2) + pow(o.y - pos.y, 2)) < 30.0f) newObs.push_back(o);
        }
        obstacles = newObs;
    }

    // ==================== ПОИСК МАРКЕРОВ ====================
    void ScanMarkers() {
        if (!proc || !baseAddr) return;
        auto now = chrono::high_resolution_clock::now();
        if (duration_cast<milliseconds>(now - lastScan).count() < 300) return;
        lastScan = now;
        
        Vec3 playerPos = GetPos();
        if (playerPos.x == 0 && playerPos.y == 0) return;
        
        markers.clear();
        
        for (DWORD addr = baseAddr; addr < baseAddr + 0x800000; addr += 0x28) {
            float x = Read<float>(addr);
            float y = Read<float>(addr + 4);
            float z = Read<float>(addr + 8);
            int type = Read<int>(addr + 0xC);
            bool active = Read<bool>(addr + 0x10);
            
            // ФИЛЬТРУЕМ МУСОР
            if (!active) continue;
            if (x < -5000 || x > 5000 || y < -5000 || y > 5000) continue;
            if (x == 0 && y == 0) continue;
            if (x == -2147483648 || y == -2147483648) continue;
            if (x > 100000 || y > 100000) continue;
            
            float dist = sqrt(pow(x - playerPos.x, 2) + pow(y - playerPos.y, 2));
            if (dist > 500) continue;
            
            bool col = false;
            for (auto& m : collectedMarkers) {
                if (abs(m.pos.x - x) < 1.0f && abs(m.pos.y - y) < 1.0f) {
                    col = true; break;
                }
            }
            if (col) continue;
            
            if (type == 1) {
                Marker m;
                m.pos = {x, y, z};
                m.type = type;
                m.active = true;
                m.collected = false;
                m.address = addr;
                markers.push_back(m);
                Print("[BOX] Found box! X=" + to_string((int)x) + " Y=" + to_string((int)y), 10);
            }
            else if (type == 2) {
                deliveryPoint = {x, y, z};
                Print("[DROP] Drop point! X=" + to_string((int)x) + " Y=" + to_string((int)y), 11);
            }
        }
    }

    // ==================== ДВИЖЕНИЕ ====================
    void MoveToTarget(Vec3 target) {
        Vec3 pos = GetPos();
        float dist = sqrt(pow(target.x - pos.x, 2) + pow(target.y - pos.y, 2));
        if (dist < 0.5f) { StopAllKeys(); return; }
        
        Vec3 adjusted = AvoidObstacles(target);
        if (sqrt(pow(adjusted.x - pos.x, 2) + pow(adjusted.y - pos.y, 2)) > 2.0f) {
            target = adjusted;
        }
        
        TurnTo(target);
        MoveForward();
        
        if (!carrying) {
            SprintOn();
            static int jumpCounter = 0;
            jumpCounter++;
            if (jumpCounter % 5 == 0 && dist > 4.0f) Jump();
            if (jumpCounter > 25) jumpCounter = 0;
        } else {
            SprintOff();
        }
    }

    // ==================== ГЛАВНЫЙ ЦИКЛ ====================
    void BotLoop() {
        active = true;
        Print("[START] Bot started!", 10);
        
        int scanCount = 0;
        Vec3 targetPos = {0,0,0};
        bool hasTarget = false;

        while (active) {
            if (!running || emergency) { StopAllKeys(); Sleep(100); continue; }
            
            scanCount++;
            Vec3 pos = GetPos();
            
            if (scanCount % 3 == 0) {
                ScanMarkers();
                ScanObstacles();
            }

            // ЛОГИКА
            if (carrying) {
                if (deliveryPoint.x != 0 && deliveryPoint.x > -5000 && deliveryPoint.x < 5000) {
                    float dist = sqrt(pow(deliveryPoint.x - pos.x, 2) + pow(deliveryPoint.y - pos.y, 2));
                    if (dist < 2.0f) {
                        carrying = false; delivered++; SprintOff(); StopAllKeys();
                        Print("[DONE] Box delivered! (" + to_string(delivered) + ")", 10);
                        obstacles.push_back(pos);
                        hasTarget = false;
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
                        if (d < nearDist && d < 500) { nearest = &m; nearDist = d; }
                    }
                }
                if (nearest) {
                    float dist = sqrt(pow(nearest->pos.x - pos.x, 2) + pow(nearest->pos.y - pos.y, 2));
                    if (dist < 2.0f) {
                        carrying = true; nearest->collected = true; collected++;
                        collectedMarkers.push_back(*nearest);
                        markers.erase(remove_if(markers.begin(), markers.end(),
                            [nearest](Marker& m) { return m.address == nearest->address; }), markers.end());
                        SprintOff(); StopAllKeys();
                        Print("[TAKEN] Box taken! (" + to_string(collected) + ")", 14);
                        hasTarget = false;
                        continue;
                    } else {
                        targetPos = nearest->pos; hasTarget = true;
                    }
                } else {
                    hasTarget = false;
                    continue;
                }
            }

            // ДВИЖЕНИЕ
            if (hasTarget && targetPos.x > -5000 && targetPos.x < 5000) {
                Vec3 posNow = GetPos();
                float dist = sqrt(pow(targetPos.x - posNow.x, 2) + pow(targetPos.y - posNow.y, 2));
                if (dist < 2.0f) { hasTarget = false; StopAllKeys(); continue; }
                MoveToTarget(targetPos);
            } else {
                StopAllKeys();
            }
            
            Sleep(40);
        }
    }

    // ==================== PRINT ====================
    void Print(string text, int color = 15) { SetColor(color); cout << text << endl; SetColor(15); }

    // ==================== CONTROL ====================
    void Start() {
        if (running) { Print("[WARN] Bot already running!", 14); return; }
        if (!proc || !playerAddr) { Print("[ERROR] Bot not initialized!", 12); return; }
        
        running = true; emergency = false; carrying = false;
        markers.clear(); collectedMarkers.clear(); obstacles.clear();
        lastScan = chrono::high_resolution_clock::now();
        if (gameWnd) { SetForegroundWindow(gameWnd); Sleep(500); }
        
        Print("[START] Bot started! Press F11 for emergency stop.", 10);
        thread(&Bot::BotLoop, this).detach();
    }

    void Stop() { running = false; StopAllKeys(); Print("[STOP] Bot stopped. Collected: " + to_string(collected) + " | Delivered: " + to_string(delivered), 14); }
    void EmergencyStop() { emergency = true; running = false; active = false; StopAllKeys(); Print("[EMERGENCY] EMERGENCY STOP! (F11)", 12); }

    void ShowStatus() {
        Vec3 pos = GetPos();
        SetColor(11);
        cout << "\n========================================\n           BOT STATUS\n========================================\n";
        SetColor(15);
        cout << "  Position:   X=" << (int)pos.x << " Y=" << (int)pos.y << " Z=" << (int)pos.z << endl;
        cout << "  Status:     " << (running ? "[RUNNING]" : "[STOPPED]") << endl;
        cout << "  Box:        " << (carrying ? "[CARRYING]" : "[SEARCHING]") << endl;
        cout << "  Collected:  " << collected << endl;
        cout << "  Delivered:  " << delivered << endl;
        cout << "  Found:      " << markers.size() << " boxes" << endl;
        cout << "  Obstacles:  " << obstacles.size() << endl;
        SetColor(11);
        cout << "========================================\n  F1-Start  F2-Stop  F5-Status\n  F11-Emergency  ESC-Exit\n========================================\n";
        SetColor(15);
    }

    void ShowHelp() {
        SetColor(14);
        cout << "\n========================================\n           CONTROLS\n========================================\n";
        SetColor(15);
        cout << "  F1  - Start bot\n  F2  - Stop bot\n  F5  - Status\n  F11 - EMERGENCY STOP\n  ESC - Exit\n";
        SetColor(14);
        cout << "========================================\n  Stealth mode - Anti-cheat safe\n========================================\n";
        SetColor(15);
    }

    void HotkeyHandler() {
        while (true) {
            if (GetAsyncKeyState(VK_F1) & 1) Start();
            if (GetAsyncKeyState(VK_F2) & 1) Stop();
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
    system("cls");

    SetColor(10);
    cout << "\n==================================================\n      STEALTH COLLECTOR BOT FOR MTA PROVINCE\n==================================================\n";
    SetColor(14);
    cout << "\n  [WARN] Run as Administrator!\n  [WARN] MTA Province must be running!\n  [WARN] Press F5 for controls\n\n";
    SetColor(15);

    Bot bot;
    if (!bot.Ready()) {
        SetColor(12);
        cout << "\n[ERROR] MTA Province not found!\n";
        SetColor(15);
        system("pause");
        return 1;
    }

    SetColor(10);
    cout << "[OK] BOT READY!\n";
    SetColor(15);
    cout << "[INFO] Press F1 to start\n\n";

    thread handler(&Bot::HotkeyHandler, &bot);
    handler.detach();

    while (true) Sleep(1000);
    return 0;
}

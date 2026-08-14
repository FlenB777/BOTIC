// main.cpp - MTA Province Bot (Mouse Turn + Smooth Movement)
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
#include <random>

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
    bool wPressed, sPressed;
    
    // Для плавного движения
    float targetAngle;
    float currentAngle;
    int smoothCounter;
    Vec3 lastTargetPos;
    bool hasLastTarget;
    POINT lastMousePos;

public:
    Bot() : proc(NULL), pid(0), playerAddr(0), baseAddr(0), running(false),
            active(false), carrying(false), speed(0.25f), collected(0),
            delivered(0), emergency(false), stuckCount(0), shiftPressed(false),
            jumpCounter(0), wPressed(false), sPressed(false),
            targetAngle(0), currentAngle(0), smoothCounter(0), hasLastTarget(false) {
        deliveryPoint = {0,0,0};
        lastPos = {0,0,0};
        lastTargetPos = {0,0,0};
        lastMousePos = {0,0};
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
    void SendKey(WORD key, bool press) {
        keybd_event((BYTE)key, 0, press ? 0 : KEYEVENTF_KEYUP, 0);
        Sleep(5);
    }

    void PressW() { if (!wPressed) { SendKey('W', true); wPressed = true; } }
    void ReleaseW() { if (wPressed) { SendKey('W', false); wPressed = false; } }
    void PressS() { if (!sPressed) { SendKey('S', true); sPressed = true; } }
    void ReleaseS() { if (sPressed) { SendKey('S', false); sPressed = false; } }
    
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
        ReleaseShift();
    }

    // ==================== ПОВОРОТ МЫШКОЙ ====================
    void MoveMouse(int deltaX, int deltaY) {
        // Получаем текущую позицию мыши
        POINT currentPos;
        GetCursorPos(&currentPos);
        
        // Если мышь не в игре - центрируем
        RECT rect;
        GetWindowRect(gameWnd, &rect);
        int centerX = (rect.left + rect.right) / 2;
        int centerY = (rect.top + rect.bottom) / 2;
        
        // Если мышь далеко от центра - перемещаем в центр
        if (abs(currentPos.x - centerX) > 100 || abs(currentPos.y - centerY) > 100) {
            SetCursorPos(centerX, centerY);
            currentPos.x = centerX;
            currentPos.y = centerY;
        }
        
        // Двигаем мышь
        SetCursorPos(currentPos.x + deltaX, currentPos.y + deltaY);
        lastMousePos = {currentPos.x + deltaX, currentPos.y + deltaY};
    }

    void SmoothTurnToTarget(Vec3 target) {
        Vec3 pos = GetPos();
        float angle = atan2(target.y - pos.y, target.x - pos.x) * 180.0f / 3.14159f - 90.0f;
        float currentAngle = GetAngle();
        
        float diff = angle - currentAngle;
        while (diff > 180) diff -= 360;
        while (diff < -180) diff += 360;
        
        // Ограничиваем максимальный поворот за один шаг (плавность)
        float maxTurn = 6.0f;
        if (abs(diff) > maxTurn) {
            diff = (diff > 0) ? maxTurn : -maxTurn;
        }
        
        // Добавляем небольшое случайное отклонение (как у игрока)
        float randomOffset = (rand() % 40 - 20) / 100.0f;
        diff += randomOffset;
        
        // Конвертируем угол в пиксели мыши
        // В MTA чувствительность мыши примерно 1 градус = 2-3 пикселя
        int mouseMove = (int)(diff * 2.5f);
        
        if (abs(mouseMove) > 1) {
            MoveMouse(mouseMove, 0);
        }
    }

    // ==================== SCAN MARKERS ====================
    void ScanMarkers() {
        if (!proc || !baseAddr) return;
        auto now = chrono::high_resolution_clock::now();
        if (duration_cast<milliseconds>(now - lastScan).count() < 300) return;
        lastScan = now;
        
        Vec3 playerPos = GetPos();
        if (playerPos.x == 0 && playerPos.y == 0) return;
        
        for (DWORD addr = baseAddr; addr < baseAddr + 0x800000; addr += 0x28) {
            float x = Read<float>(addr);
            float y = Read<float>(addr + 4);
            float z = Read<float>(addr + 8);
            int type = Read<int>(addr + 0xC);
            bool active = Read<bool>(addr + 0x10);
            
            if (!active) continue;
            if (x < -5000 || x > 5000) continue;
            if (y < -5000 || y > 5000) continue;
            if (x == 0 && y == 0) continue;
            if (x == -2147483648 || y == -2147483648) continue;
            
            float dist = sqrt(pow(x - playerPos.x, 2) + pow(y - playerPos.y, 2));
            if (dist > 500) continue;
            
            bool exists = false;
            for (auto& m : markers) {
                float d = sqrt(pow(m.pos.x - x, 2) + pow(m.pos.y - y, 2));
                if (d < 1.0f) { exists = true; break; }
            }
            if (exists) continue;
            
            bool col = false;
            for (auto& m : collectedMarkers) {
                float d = sqrt(pow(m.pos.x - x, 2) + pow(m.pos.y - y, 2));
                if (d < 1.0f) { col = true; break; }
            }
            if (col) continue;
            
            if (type == 1) { 
                Marker m; 
                m.pos = {x, y, z}; 
                m.type = type; 
                m.active = true; 
                m.collected = false; 
                m.address = addr; 
                m.spawnTime = time(NULL); 
                markers.push_back(m);
                Print("[BOX] New box! X=" + to_string((int)x) + " Y=" + to_string((int)y), 10); 
            }
            else if (type == 2) { 
                deliveryPoint = {x, y, z}; 
                Print("[DROP] Drop point! X=" + to_string((int)x) + " Y=" + to_string((int)y), 11); 
            }
        }
        
        if (markers.size() > 50) {
            vector<Marker> cleanMarkers;
            for (auto& m : markers) {
                if (m.pos.x > -5000 && m.pos.x < 5000 && 
                    m.pos.y > -5000 && m.pos.y < 5000 &&
                    m.pos.x != -2147483648 && m.pos.y != -2147483648) {
                    cleanMarkers.push_back(m);
                }
            }
            markers = cleanMarkers;
        }
    }

    // ==================== ОБХОД ПРЕПЯТСТВИЙ ====================
    Vec3 ScanObstacles(Vec3 target) {
        Vec3 pos = GetPos();
        Vec3 result = target;
        
        float angle = atan2(target.y - pos.y, target.x - pos.x);
        float checkDistance = 8.0f;
        
        for (float d = 2.0f; d <= checkDistance; d += 1.0f) {
            float checkX = pos.x + cos(angle) * d;
            float checkY = pos.y + sin(angle) * d;
            
            for (auto& obs : obstacles) {
                float dist = sqrt(pow(obs.x - checkX, 2) + pow(obs.y - checkY, 2));
                if (dist < 3.0f) {
                    float avoidAngle = atan2(pos.y - obs.y, pos.x - obs.x);
                    
                    Vec3 right = {
                        obs.x + cos(avoidAngle + 1.2f) * 4.0f,
                        obs.y + sin(avoidAngle + 1.2f) * 4.0f,
                        pos.z
                    };
                    
                    bool rightFree = true;
                    for (auto& obs2 : obstacles) {
                        float d2 = sqrt(pow(right.x - obs2.x, 2) + pow(right.y - obs2.y, 2));
                        if (d2 < 3.0f) { rightFree = false; break; }
                    }
                    
                    if (rightFree) {
                        result = right;
                        return result;
                    } else {
                        Vec3 left = {
                            obs.x + cos(avoidAngle - 1.2f) * 4.0f,
                            obs.y + sin(avoidAngle - 1.2f) * 4.0f,
                            pos.z
                        };
                        result = left;
                        return result;
                    }
                }
            }
        }
        
        return result;
    }

    // ==================== ПОИСК ПРЕПЯТСТВИЙ ====================
    void DetectObstacles() {
        Vec3 pos = GetPos();
        if (pos.x == 0 && pos.y == 0) return;
        
        for (float x = pos.x - 15; x <= pos.x + 15; x += 1.5f) {
            for (float y = pos.y - 15; y <= pos.y + 15; y += 1.5f) {
                for (DWORD addr = baseAddr; addr < baseAddr + 0x100000; addr += 4) {
                    float memX = Read<float>(addr);
                    float memY = Read<float>(addr + 4);
                    float memZ = Read<float>(addr + 8);
                    
                    if (memX < -5000 || memX > 5000) continue;
                    if (memY < -5000 || memY > 5000) continue;
                    
                    if (abs(memX - x) < 0.5f && abs(memY - y) < 0.5f) {
                        bool exists = false;
                        for (auto& o : obstacles) { 
                            float d = sqrt(pow(o.x - x, 2) + pow(o.y - y, 2)); 
                            if (d < 2.0f) { exists = true; break; } 
                        }
                        if (!exists) { 
                            Vec3 obs = {x, y, memZ}; 
                            obstacles.push_back(obs); 
                        }
                        break;
                    }
                }
            }
        }
        
        vector<Vec3> newObstacles;
        for (auto& o : obstacles) { 
            float d = sqrt(pow(o.x - pos.x, 2) + pow(o.y - pos.y, 2)); 
            if (d < 30.0f) newObstacles.push_back(o); 
        }
        obstacles = newObstacles;
    }

    // ==================== ДВИЖЕНИЕ ====================
    void MoveToTarget(Vec3 target) {
        Vec3 pos = GetPos();
        float dist = sqrt(pow(target.x - pos.x, 2) + pow(target.y - pos.y, 2));
        
        if (dist < 0.5f) {
            StopAll();
            return;
        }
        
        // Проверяем препятствия
        Vec3 adjustedTarget = ScanObstacles(target);
        if (adjustedTarget.x != target.x || adjustedTarget.y != target.y) {
            float d = sqrt(pow(adjustedTarget.x - pos.x, 2) + pow(adjustedTarget.y - pos.y, 2));
            if (d > 2.0f && d < 20.0f) {
                target = adjustedTarget;
            }
        }
        
        // Плавный поворот мышкой
        SmoothTurnToTarget(target);
        
        // Движение вперёд
        PressW();
        ReleaseS();
        
        // Shift (бег) только если нет ящика
        if (!carrying) {
            PressShift();
            jumpCounter++;
            if (jumpCounter % 4 == 0 && dist > 5.0f && rand() % 3 == 0) {
                PressSpace();
            }
            if (jumpCounter > 20) jumpCounter = 0;
        } else {
            ReleaseShift();
            if (jumpCounter % 8 == 0 && dist > 3.0f && rand() % 5 == 0) {
                PressSpace();
            }
        }
    }

    // ==================== MAIN LOOP ====================
    void BotLoop() {
        active = true;
        Print("[START] Bot started! Mouse turn enabled.", 10);
        Print("[RUN] Smooth movement + Obstacle avoidance", 11);
        
        int scanCount = 0;
        lastPos = GetPos();
        stuckCount = 0;
        jumpCounter = 0;
        Vec3 targetPos = {0,0,0};
        bool hasTarget = false;
        int moveCounter = 0;
        int randomDirection = 0;

        while (active) {
            if (!running || emergency) { StopAll(); Sleep(100); continue; }
            
            scanCount++;
            Vec3 pos = GetPos();
            
            // Проверка застревания
            float move = sqrt(pow(pos.x - lastPos.x, 2) + pow(pos.y - lastPos.y, 2));
            if (move < 0.03f) {
                stuckCount++;
                if (stuckCount > 80) {
                    Print("[WARN] Stuck! Jump and turn...", 14);
                    PressSpace(); Sleep(150); PressSpace();
                    MoveMouse(-200, 0);
                    PressSpace();
                    stuckCount = 0;
                }
            } else {
                stuckCount = max(0, stuckCount - 2);
            }
            lastPos = pos;

            // Сканирование
            if (scanCount % 3 == 0) {
                ScanMarkers();
                DetectObstacles();
            }

            // ===== ЛОГИКА =====
            if (carrying) {
                if (deliveryPoint.x != 0 && deliveryPoint.x > -5000 && deliveryPoint.x < 5000) {
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
                    hasTarget = false; 
                    for (auto& m : markers) {
                        if (m.type == 2) {
                            deliveryPoint = m.pos;
                            Print("[DROP] Found drop point!", 11);
                            break;
                        }
                    }
                    continue;
                }
            } else {
                Marker* nearest = nullptr; 
                float nearDist = 999999.0f;
                for (auto& m : markers) {
                    if (m.type == 1 && !m.collected) {
                        float d = sqrt(pow(m.pos.x - pos.x, 2) + pow(m.pos.y - pos.y, 2));
                        if (d < nearDist && d < 500) { 
                            nearest = &m; 
                            nearDist = d; 
                        }
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
                        ReleaseShift(); StopAll();
                        Print("[TAKEN] Box taken! (" + to_string(collected) + ")", 14);
                        jumpCounter = 0; hasTarget = false;
                        continue;
                    } else {
                        targetPos = nearest->pos; 
                        hasTarget = true;
                    }
                } else {
                    hasTarget = false;
                    moveCounter++;
                    if (moveCounter % 30 == 0) {
                        randomDirection = rand() % 4;
                    }
                    if (moveCounter % 5 == 0) {
                        switch(randomDirection) {
                            case 0: PressW(); ReleaseS(); break;
                            case 1: PressS(); ReleaseW(); break;
                            case 2: MoveMouse(-20, 0); break;
                            case 3: MoveMouse(20, 0); break;
                        }
                    }
                    if (moveCounter % 60 == 0) {
                        StopAll();
                    }
                    continue;
                }
            }

            // ===== ДВИЖЕНИЕ =====
            if (hasTarget && targetPos.x > -5000 && targetPos.x < 5000) {
                Vec3 posNow = GetPos();
                float distToTarget = sqrt(pow(targetPos.x - posNow.x, 2) + pow(targetPos.y - posNow.y, 2));
                
                if (distToTarget < 2.0f) { 
                    hasTarget = false; 
                    StopAll(); 
                    continue; 
                }

                if (distToTarget > 500) {
                    hasTarget = false;
                    continue;
                }

                MoveToTarget(targetPos);
            } else {
                StopAll();
            }
            
            Sleep(30);
        }
    }

    // ==================== PRINT ====================
    void Print(string text, int color = 15) { SetColor(color); cout << text << endl; SetColor(15); }

    // ==================== CONTROL ====================
    void Start() {
        if (running) { Print("[WARN] Bot already running!", 14); return; }
        if (!proc || !playerAddr) { Print("[ERROR] Bot not initialized!", 12); return; }
        
        running = true; emergency = false; carrying = false; shiftPressed = false;
        markers.clear();
        collectedMarkers.clear();
        obstacles.clear();
        lastScan = chrono::high_resolution_clock::now();
        
        if (gameWnd) { 
            SetForegroundWindow(gameWnd);
            Sleep(500);
            // Центрируем мышь в окне игры
            RECT rect;
            GetWindowRect(gameWnd, &rect);
            SetCursorPos((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
        }
        
        Print("[START] Bot started! Mouse turn enabled.", 10);
        Print("[INFO] Press F11 for emergency stop.", 14);
        
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
        cout << "  Control:    MOUSE TURN (smooth)" << endl;
        if (deliveryPoint.x != 0 && deliveryPoint.x > -5000 && deliveryPoint.x < 5000) {
            cout << "  Drop:       X=" << (int)deliveryPoint.x << " Y=" << (int)deliveryPoint.y << endl;
        }
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
        cout << "  MOVEMENT:" << endl;
        cout << "  W - Forward" << endl;
        cout << "  S - Back" << endl;
        cout << "  MOUSE - Turning (smooth)" << endl;
        cout << "  Shift - Sprint (no box)" << endl;
        cout << "  Space - Jump" << endl;
        cout << endl;
        cout << "  FEATURES:" << endl;
        cout << "  Mouse turn (faster, realistic)" << endl;
        cout << "  Smooth movement" << endl;
        cout << "  Obstacle avoidance" << endl;
        SetColor(14);
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
    cout << "  [AVOID] Smart obstacle avoidance" << endl;
    cout << "  [MOUSE] Mouse turn (faster, realistic)" << endl;
    cout << "  [REAL] Looks like a real player" << endl;
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

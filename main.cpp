// main.cpp - MTA Bot (Bind System)
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
    int jumpCounter;
    
    // Состояние биндов
    bool bindsActive;

public:
    Bot() : proc(NULL), pid(0), playerAddr(0), baseAddr(0), running(false),
            active(false), carrying(false), speed(0.25f), collected(0),
            delivered(0), emergency(false), stuckCount(0),
            jumpCounter(0), bindsActive(false) {
        deliveryPoint = {0,0,0};
        lastPos = {0,0,0};
        gameWnd = NULL;
        FindProcess();
        if (proc) FindAddresses();
    }

    ~Bot() { if (proc) CloseHandle(proc); UnbindAll(); }

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

    // ==================== ОТПРАВКА КОМАНД В КОНСОЛЬ MTA ====================
    void SendConsoleCommand(string cmd) {
        if (!gameWnd) return;
        
        // Открываем консоль (~ или F8)
        // Отправляем команду через нажатия клавиш
        // Альтернатива: используем WM_COPYDATA
        COPYDATASTRUCT cds;
        cds.dwData = 0;
        cds.cbData = cmd.length() + 1;
        cds.lpData = (void*)cmd.c_str();
        
        SendMessage(gameWnd, WM_COPYDATA, 0, (LPARAM)&cds);
        Sleep(50);
    }

    // ==================== БИНДЫ ====================
    void ApplyBinds() {
        if (bindsActive) return;
        
        Print("[BINDS] Applying binds...", 11);
        
        // Сначала сбрасываем старые
        UnbindAll();
        
        // Устанавливаем новые бинды
        SendConsoleCommand("bind w +forward");
        SendConsoleCommand("bind s +backward");
        SendConsoleCommand("bind a +left");
        SendConsoleCommand("bind d +right");
        SendConsoleCommand("bind shift +sprint");
        SendConsoleCommand("bind space +jump");
        
        bindsActive = true;
        Print("[BINDS] Binds applied!", 10);
    }

    void UnbindAll() {
        if (!bindsActive) return;
        
        Print("[BINDS] Removing binds...", 14);
        
        SendConsoleCommand("unbind w");
        SendConsoleCommand("unbind s");
        SendConsoleCommand("unbind a");
        SendConsoleCommand("unbind d");
        SendConsoleCommand("unbind shift");
        SendConsoleCommand("unbind space");
        
        bindsActive = false;
        Print("[BINDS] Binds removed.", 14);
    }

    // ==================== УПРАВЛЕНИЕ ЧЕРЕЗ БИНДЫ ====================
    // Вместо симуляции нажатий, бот управляет через изменение угла
    // Движение вперёд/назад включается через флаги

    void MoveForward() { /* Бинд w уже активен, нужно только удерживать направление */ }
    void MoveBack() { /* Бинд s уже активен */ }
    void StopMove() { /* Отпускаем W и S через бинды не нужно, бинды сами работают */ }

    // ==================== ПОВОРОТ (ПЛАВНЫЙ) ====================
    void TurnToTarget(Vec3 target) {
        Vec3 pos = GetPos();
        float targetAngle = atan2(target.y - pos.y, target.x - pos.x) * 180.0f / 3.14159f - 90.0f;
        float currentAngle = GetAngle();
        
        float diff = targetAngle - currentAngle;
        while (diff > 180) diff -= 360;
        while (diff < -180) diff += 360;
        
        // Плавный поворот через бинды A/D
        float maxTurn = 6.0f;
        if (abs(diff) > maxTurn) {
            diff = (diff > 0) ? maxTurn : -maxTurn;
        }
        
        // A и D уже забинжены, они работают автоматически
        // Но нам нужно их нажимать/отпускать
        if (diff > 2.0f) {
            // Нажимаем D (поворот направо)
            keybd_event('D', 0, 0, 0);
            Sleep(10);
        } else if (diff < -2.0f) {
            // Нажимаем A (поворот налево)
            keybd_event('A', 0, 0, 0);
            Sleep(10);
        } else {
            // Отпускаем A и D
            keybd_event('D', 0, KEYEVENTF_KEYUP, 0);
            keybd_event('A', 0, KEYEVENTF_KEYUP, 0);
        }
    }

    // ==================== ОБХОД ПРЕПЯТСТВИЙ ====================
    void ScanObstacles() {
        Vec3 pos = GetPos();
        if (pos.x == 0 && pos.y == 0) return;
        
        for (float x = pos.x - 15; x <= pos.x + 15; x += 1.5f) {
            for (float y = pos.y - 15; y <= pos.y + 15; y += 1.5f) {
                for (DWORD addr = baseAddr; addr < baseAddr + 0x100000; addr += 4) {
                    float mx = Read<float>(addr);
                    float my = Read<float>(addr + 4);
                    float mz = Read<float>(addr + 8);
                    
                    if (mx < -5000 || mx > 5000 || my < -5000 || my > 5000) continue;
                    if (abs(mx - x) < 0.5f && abs(my - y) < 0.5f) {
                        bool exists = false;
                        for (auto& o : obstacles) {
                            if (sqrt(pow(o.x - x, 2) + pow(o.y - y, 2)) < 2.0f) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            obstacles.push_back({x, y, mz});
                        }
                        break;
                    }
                }
            }
        }
        
        vector<Vec3> newObstacles;
        for (auto& o : obstacles) {
            if (sqrt(pow(o.x - pos.x, 2) + pow(o.y - pos.y, 2)) < 30.0f) {
                newObstacles.push_back(o);
            }
        }
        obstacles = newObstacles;
    }

    Vec3 AvoidObstacle(Vec3 target) {
        Vec3 pos = GetPos();
        float angle = atan2(target.y - pos.y, target.x - pos.x);
        
        for (float d = 2.0f; d <= 8.0f; d += 1.0f) {
            float checkX = pos.x + cos(angle) * d;
            float checkY = pos.y + sin(angle) * d;
            
            for (auto& o : obstacles) {
                float dist = sqrt(pow(o.x - checkX, 2) + pow(o.y - checkY, 2));
                if (dist < 2.5f) {
                    float avoidAngle = atan2(pos.y - o.y, pos.x - o.x);
                    
                    Vec3 right = {
                        o.x + cos(avoidAngle + 1.2f) * 4.0f,
                        o.y + sin(avoidAngle + 1.2f) * 4.0f,
                        pos.z
                    };
                    
                    bool rightFree = true;
                    for (auto& o2 : obstacles) {
                        if (sqrt(pow(right.x - o2.x, 2) + pow(right.y - o2.y, 2)) < 2.5f) {
                            rightFree = false;
                            break;
                        }
                    }
                    
                    if (rightFree) {
                        Print("[AVOID] Right!", 11);
                        return right;
                    }
                    
                    Vec3 left = {
                        o.x + cos(avoidAngle - 1.2f) * 4.0f,
                        o.y + sin(avoidAngle - 1.2f) * 4.0f,
                        pos.z
                    };
                    Print("[AVOID] Left!", 11);
                    return left;
                }
            }
        }
        return target;
    }

    // ==================== ПОИСК МАРКЕРОВ ====================
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
            if (x < -5000 || x > 5000 || y < -5000 || y > 5000) continue;
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

    // ==================== ГЛАВНЫЙ ЦИКЛ ====================
    void BotLoop() {
        active = true;
        Print("[START] Bot started! Using binds.", 10);
        Print("[RUN] W=forward S=back A/D=turn Shift=sprint Space=jump", 11);
        
        // Применяем бинды при старте
        ApplyBinds();
        
        int scanCount = 0;
        lastPos = GetPos();
        stuckCount = 0;
        jumpCounter = 0;
        Vec3 targetPos = {0,0,0};
        bool hasTarget = false;
        bool movingForward = false;

        while (active) {
            if (!running || emergency) { 
                // Отпускаем все клавиши
                keybd_event('W', 0, KEYEVENTF_KEYUP, 0);
                keybd_event('S', 0, KEYEVENTF_KEYUP, 0);
                keybd_event('A', 0, KEYEVENTF_KEYUP, 0);
                keybd_event('D', 0, KEYEVENTF_KEYUP, 0);
                keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
                Sleep(100); 
                continue; 
            }
            
            scanCount++;
            Vec3 pos = GetPos();
            
            // Проверка застревания
            float move = sqrt(pow(pos.x - lastPos.x, 2) + pow(pos.y - lastPos.y, 2));
            if (move < 0.03f && movingForward) {
                stuckCount++;
                if (stuckCount > 60) {
                    Print("[WARN] Stuck! Jumping...", 14);
                    // Прыжок через бинд space
                    keybd_event(VK_SPACE, 0, 0, 0);
                    Sleep(50);
                    keybd_event(VK_SPACE, 0, KEYEVENTF_KEYUP, 0);
                    Sleep(150);
                    keybd_event(VK_SPACE, 0, 0, 0);
                    Sleep(50);
                    keybd_event(VK_SPACE, 0, KEYEVENTF_KEYUP, 0);
                    // Поворот налево
                    keybd_event('A', 0, 0, 0);
                    Sleep(400);
                    keybd_event('A', 0, KEYEVENTF_KEYUP, 0);
                    stuckCount = 0;
                }
            } else {
                stuckCount = max(0, stuckCount - 2);
            }
            lastPos = pos;

            // Сканирование
            if (scanCount % 3 == 0) {
                ScanMarkers();
                ScanObstacles();
            }

            // ===== ЛОГИКА =====
            if (carrying) {
                if (deliveryPoint.x != 0 && deliveryPoint.x > -5000 && deliveryPoint.x < 5000) {
                    float dist = sqrt(pow(deliveryPoint.x - pos.x, 2) + pow(deliveryPoint.y - pos.y, 2));
                    if (dist < 2.0f) {
                        carrying = false; delivered++;
                        // Отпускаем Shift (бег)
                        keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
                        // Отпускаем W
                        keybd_event('W', 0, KEYEVENTF_KEYUP, 0);
                        Print("[DONE] Box delivered! (" + to_string(delivered) + ")", 10);
                        obstacles.push_back(pos);
                        jumpCounter = 0; hasTarget = false; movingForward = false;
                        continue;
                    } else {
                        targetPos = deliveryPoint; hasTarget = true;
                    }
                } else {
                    hasTarget = false; continue;
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
                        // Отпускаем Shift и W
                        keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
                        keybd_event('W', 0, KEYEVENTF_KEYUP, 0);
                        Print("[TAKEN] Box taken! (" + to_string(collected) + ")", 14);
                        jumpCounter = 0; hasTarget = false; movingForward = false;
                        continue;
                    } else {
                        targetPos = nearest->pos; 
                        hasTarget = true;
                    }
                } else {
                    hasTarget = false;
                    continue;
                }
            }

            // ===== ДВИЖЕНИЕ С БИНДАМИ =====
            if (hasTarget && targetPos.x > -5000 && targetPos.x < 5000) {
                Vec3 posNow = GetPos();
                float distToTarget = sqrt(pow(targetPos.x - posNow.x, 2) + pow(targetPos.y - posNow.y, 2));
                
                if (distToTarget < 2.0f) { 
                    hasTarget = false; 
                    keybd_event('W', 0, KEYEVENTF_KEYUP, 0);
                    movingForward = false;
                    continue; 
                }

                // Проверяем препятствия
                Vec3 obstacleTarget = AvoidObstacle(targetPos);
                float obsDist = sqrt(pow(obstacleTarget.x - posNow.x, 2) + pow(obstacleTarget.y - posNow.y, 2));
                if (obsDist > 2.0f && obsDist < 20.0f) {
                    targetPos = obstacleTarget;
                }

                // Плавный поворот (через A/D)
                TurnToTarget(targetPos);
                
                // Движение вперёд (через бинд W)
                keybd_event('W', 0, 0, 0);
                keybd_event('S', 0, KEYEVENTF_KEYUP, 0);
                movingForward = true;
                
                // Бег (через бинд Shift)
                if (!carrying) {
                    keybd_event(VK_SHIFT, 0, 0, 0);
                    jumpCounter++;
                    if (jumpCounter % 5 == 0 && distToTarget > 4.0f) { 
                        keybd_event(VK_SPACE, 0, 0, 0);
                        Sleep(50);
                        keybd_event(VK_SPACE, 0, KEYEVENTF_KEYUP, 0);
                    }
                    if (jumpCounter > 25) jumpCounter = 0;
                } else {
                    keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
                }
            } else {
                // Отпускаем W если нет цели
                keybd_event('W', 0, KEYEVENTF_KEYUP, 0);
                movingForward = false;
            }
            
            Sleep(35);
        }
    }

    // ==================== PRINT ====================
    void Print(string text, int color = 15) { SetColor(color); cout << text << endl; SetColor(15); }

    // ==================== УПРАВЛЕНИЕ ====================
    void Start() {
        if (running) { Print("[WARN] Bot already running!", 14); return; }
        if (!proc || !playerAddr) { Print("[ERROR] Bot not initialized!", 12); return; }
        
        running = true; emergency = false; carrying = false;
        markers.clear();
        collectedMarkers.clear();
        obstacles.clear();
        lastScan = chrono::high_resolution_clock::now();
        
        if (gameWnd) { 
            SetForegroundWindow(gameWnd);
            Sleep(500);
        }
        
        // Применяем бинды
        ApplyBinds();
        
        Print("[START] Bot started! Press F11 for emergency stop.", 10);
        Print("[INFO] Binds active: W=forward S=back A/D=turn Shift=sprint Space=jump", 11);
        
        thread(&Bot::BotLoop, this).detach();
    }

    void Stop() { 
        running = false; 
        // Отпускаем все клавиши
        keybd_event('W', 0, KEYEVENTF_KEYUP, 0);
        keybd_event('S', 0, KEYEVENTF_KEYUP, 0);
        keybd_event('A', 0, KEYEVENTF_KEYUP, 0);
        keybd_event('D', 0, KEYEVENTF_KEYUP, 0);
        keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
        // Сбрасываем бинды
        UnbindAll();
        Print("[STOP] Bot stopped. Collected: " + to_string(collected) + " | Delivered: " + to_string(delivered), 14); 
    }
    
    void EmergencyStop() { 
        emergency = true; 
        running = false; 
        active = false; 
        keybd_event('W', 0, KEYEVENTF_KEYUP, 0);
        keybd_event('S', 0, KEYEVENTF_KEYUP, 0);
        keybd_event('A', 0, KEYEVENTF_KEYUP, 0);
        keybd_event('D', 0, KEYEVENTF_KEYUP, 0);
        keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
        UnbindAll();
        Print("[EMERGENCY] EMERGENCY STOP! (F11)", 12); 
    }

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
        cout << "  Binds:      " << (bindsActive ? "[ACTIVE]" : "[REMOVED]") << endl;
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
        cout << "  F1  - Start bot (applies binds)" << endl;
        cout << "  F2  - Stop bot (removes binds)" << endl;
        cout << "  F3  - Faster" << endl;
        cout << "  F4  - Slower" << endl;
        cout << "  F5  - Status" << endl;
        cout << "  F11 - EMERGENCY STOP (removes binds)" << endl;
        cout << "  ESC - Exit (removes binds)" << endl;
        cout << endl;
        cout << "  BINDS (auto-applied):" << endl;
        cout << "  w - +forward" << endl;
        cout << "  s - +backward" << endl;
        cout << "  a - +left" << endl;
        cout << "  d - +right" << endl;
        cout << "  shift - +sprint" << endl;
        cout << "  space - +jump" << endl;
        cout << endl;
        cout << "  Binds are REMOVED when bot stops!" << endl;
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
    cout << "      STEALTH COLLECTOR BOT FOR MTA PROVINCE" << endl;
    cout << "==================================================" << endl;
    cout << "  [BOX] Collects boxes" << endl;
    cout << "  [DROP] Delivers to drop point" << endl;
    cout << "  [AVOID] Smart obstacle avoidance" << endl;
    cout << "  [BINDS] Uses in-game binds (auto-removed)" << endl;
    cout << "==================================================" << endl;
    SetColor(14);
    cout << "\n  [WARN] Run as Administrator!" << endl;
    cout << "  [WARN] MTA Province must be running!" << endl;
    cout << "  [WARN] Binds are REMOVED when bot stops!" << endl;
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
    cout << "[INFO] Press F1 to start (applies binds)" << endl;
    cout << "[INFO] Press F2 to stop (removes binds)" << endl;
    cout << endl;

    thread handler(&Bot::HotkeyHandler, &bot);
    handler.detach();

    while (true) Sleep(1000);
    return 0;
}

// main.cpp - MTA Bot (Console Control)
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
    
    // Новые поля для плавности
    float currentAngleDiff;
    float lastAngleDiff;
    int turnStabilityCounter;
    Vec3 avoidancePoint;
    bool avoiding;
    int avoidanceCounter;
    float lastDistanceToTarget;
    int sameDistanceCount;
    Vec3 lastTargetPos;
    int targetChangeCounter;

public:
    Bot() : proc(NULL), pid(0), playerAddr(0), baseAddr(0), running(false),
            active(false), carrying(false), speed(0.25f), collected(0),
            delivered(0), emergency(false), stuckCount(0), shiftPressed(false),
            jumpCounter(0), wPressed(false), aPressed(false), sPressed(false), dPressed(false),
            currentAngleDiff(0), lastAngleDiff(0), turnStabilityCounter(0),
            avoiding(false), avoidanceCounter(0), lastDistanceToTarget(0),
            sameDistanceCount(0), targetChangeCounter(0) {
        deliveryPoint = {0,0,0};
        lastPos = {0,0,0};
        avoidancePoint = {0,0,0};
        lastTargetPos = {0,0,0};
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

    // ==================== ВНУТРИИГРОВЫЕ КОМАНДЫ ====================
    void SendCommand(string cmd) {
        if (!gameWnd) return;
        
        COPYDATASTRUCT cds;
        cds.dwData = 0;
        cds.cbData = cmd.length() + 1;
        cds.lpData = (void*)cmd.c_str();
        
        SendMessage(gameWnd, WM_COPYDATA, 0, (LPARAM)&cds);
    }

    // ==================== KEYS ====================
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

    // ==================== УЛУЧШЕННЫЙ ПОВОРОТ ====================
    void TurnToTarget(Vec3 target) {
        Vec3 pos = GetPos();
        float angle = atan2(target.y - pos.y, target.x - pos.x);
        float currentAngle = 0;
        DWORD ped = Read<DWORD>(playerAddr);
        if (ped && ped > 0x10000 && ped < 0x7FFFFFFF) {
            currentAngle = Read<float>(ped + 0x20);
        }
        
        float diff = angle * 180.0f / 3.14159f - 90.0f - currentAngle;
        while (diff > 180) diff -= 360;
        while (diff < -180) diff += 360;
        
        // Плавное управление поворотом
        if (abs(diff) > 15) {
            // Большой угол - быстро поворачиваем
            if (diff > 0) { PressD(); ReleaseA(); }
            else { PressA(); ReleaseD(); }
        } else if (abs(diff) > 5) {
            // Средний угол - короткие нажатия для плавности
            if (diff > 0) { 
                PressD(); Sleep(20); ReleaseD();
                ReleaseA();
            } else { 
                PressA(); Sleep(20); ReleaseA();
                ReleaseD();
            }
        } else {
            // Малый угол - не поворачиваем
            ReleaseA(); ReleaseD();
        }
        
        currentAngleDiff = diff;
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

    // ==================== УЛУЧШЕННОЕ ИЗБЕГАНИЕ ПРЕПЯТСТВИЙ ====================
    Vec3 AvoidObstacle(Vec3 target) {
        Vec3 pos = GetPos();
        Vec3 result = target;
        float closestObstacleDist = 999999.0f;
        Vec3 closestObstacle = {0,0,0};
        bool foundObstacle = false;
        
        // Находим ближайшее препятствие
        for (auto& o : obstacles) {
            float d = sqrt(pow(o.x - pos.x, 2) + pow(o.y - pos.y, 2));
            if (d < 5.0f && d < closestObstacleDist) {
                closestObstacleDist = d;
                closestObstacle = o;
                foundObstacle = true;
            }
        }
        
        if (foundObstacle) {
            // Угол от препятствия к игроку
            float angleFromObstacle = atan2(pos.y - closestObstacle.y, pos.x - closestObstacle.x);
            
            // Угол к цели
            float angleToTarget = atan2(target.y - pos.y, target.x - pos.x);
            
            // Выбираем сторону обхода (левую или правую)
            float angleDiff = angleToTarget - angleFromObstacle;
            while (angleDiff > M_PI) angleDiff -= 2 * M_PI;
            while (angleDiff < -M_PI) angleDiff += 2 * M_PI;
            
            float avoidAngle;
            if (angleDiff > 0) {
                // Обходим справа
                avoidAngle = angleFromObstacle + M_PI / 2;
            } else {
                // Обходим слева
                avoidAngle = angleFromObstacle - M_PI / 2;
            }
            
            // Точка обхода
            float avoidDist = 7.0f; // дистанция обхода
            result.x = pos.x + cos(avoidAngle) * avoidDist;
            result.y = pos.y + sin(avoidAngle) * avoidDist;
            result.z = pos.z;
            
            // Плавный переход к точке обхода
            if (!avoiding) {
                avoiding = true;
                avoidanceCounter = 30; // количество итераций обхода
                avoidancePoint = result;
            }
        }
        
        if (avoiding) {
            avoidanceCounter--;
            if (avoidanceCounter > 0) {
                result = avoidancePoint;
            } else {
                avoiding = false;
            }
        }
        
        return result;
    }

    // ==================== ПРОВЕРКА ПРЕПЯТСТВИЙ ВПЕРЕДИ ====================
    bool CheckObstacleAhead(Vec3 target) {
        Vec3 pos = GetPos();
        float distToTarget = sqrt(pow(target.x - pos.x, 2) + pow(target.y - pos.y, 2));
        
        if (distToTarget < 10.0f) {
            // Проверяем, не движемся ли мы к препятствию
            for (auto& o : obstacles) {
                float distToObstacle = sqrt(pow(o.x - pos.x, 2) + pow(o.y - pos.y, 2));
                if (distToObstacle < 3.0f) {
                    // Препятствие очень близко
                    return true;
                }
            }
        }
        return false;
    }

    // ==================== MAIN LOOP ====================
    void BotLoop() {
        active = true;
        Print("[START] Bot started! Press F11 for emergency stop.", 10);
        Print("[RUN] Use WASD + Shift + Space", 11);
        
        int scanCount = 0;
        lastPos = GetPos();
        stuckCount = 0;
        jumpCounter = 0;
        Vec3 targetPos = {0,0,0};
        bool hasTarget = false;
        int smoothCounter = 0;

        while (active) {
            if (!running || emergency) { StopAll(); Sleep(100); continue; }
            
            scanCount++;
            Vec3 pos = GetPos();
            
            // Проверка застревания (улучшенная)
            float move = sqrt(pow(pos.x - lastPos.x, 2) + pow(pos.y - lastPos.y, 2));
            if (move < 0.05f && hasTarget) {
                stuckCount++;
                if (stuckCount > 30) { // Уменьшено с 50 до 30 для быстрой реакции
                    Print("[WARN] Stuck! Avoiding...", 14);
                    
                    // Пытаемся обойти препятствие
                    if (avoiding) {
                        // Если уже обходим, прыгаем
                        PressSpace(); Sleep(200); PressSpace();
                    } else {
                        // Начинаем обход
                        avoiding = true;
                        avoidanceCounter = 40;
                        
                        // Создаем точку обхода перпендикулярно направлению движения
                        float currentAngle = 0;
                        DWORD ped = Read<DWORD>(playerAddr);
                        if (ped && ped > 0x10000 && ped < 0x7FFFFFFF) {
                            currentAngle = Read<float>(ped + 0x20) * M_PI / 180.0f;
                        }
                        
                        avoidancePoint.x = pos.x + cos(currentAngle + M_PI / 2) * 5.0f;
                        avoidancePoint.y = pos.y + sin(currentAngle + M_PI / 2) * 5.0f;
                        avoidancePoint.z = pos.z;
                        
                        // Добавляем текущую позицию как препятствие
                        obstacles.push_back(pos);
                    }
                    
                    stuckCount = 0;
                }
            } else { 
                stuckCount = 0; 
                if (move > 0.1f) {
                    // Движемся нормально
                    avoiding = false;
                }
            }
            lastPos = pos;

            // Сканирование
            if (scanCount % 3 == 0) {
                ScanMarkers();
            }

            // Логика
            if (carrying) {
                if (deliveryPoint.x != 0) {
                    float dist = sqrt(pow(deliveryPoint.x - pos.x, 2) + pow(deliveryPoint.y - pos.y, 2));
                    if (dist < 2.0f) {
                        carrying = false; delivered++; ReleaseShift(); StopAll();
                        Print("[DONE] Box delivered! (" + to_string(delivered) + ")", 10);
                        obstacles.push_back(pos);
                        jumpCounter = 0; hasTarget = false;
                        avoiding = false;
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
                        ReleaseShift(); StopAll();
                        Print("[TAKEN] Box taken! (" + to_string(collected) + ")", 14);
                        jumpCounter = 0; hasTarget = false;
                        avoiding = false;
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

            // ===== ДВИЖЕНИЕ (УЛУЧШЕННОЕ) =====
            if (hasTarget) {
                Vec3 posNow = GetPos();
                float distToTarget = sqrt(pow(targetPos.x - posNow.x, 2) + pow(targetPos.y - posNow.y, 2));
                
                if (distToTarget < 2.0f) { 
                    hasTarget = false; 
                    StopAll(); 
                    avoiding = false;
                    continue; 
                }

                // Проверяем, не застряли ли мы на месте
                if (abs(distToTarget - lastDistanceToTarget) < 0.1f) {
                    sameDistanceCount++;
                    if (sameDistanceCount > 20) {
                        // Не двигаемся к цели, пробуем обойти
                        if (!avoiding) {
                            avoiding = true;
                            avoidanceCounter = 30;
                            
                            // Обход перпендикулярно направлению к цели
                            float angleToTarget = atan2(targetPos.y - posNow.y, targetPos.x - posNow.x);
                            avoidancePoint.x = posNow.x + cos(angleToTarget + M_PI / 2) * 4.0f;
                            avoidancePoint.y = posNow.y + sin(angleToTarget + M_PI / 2) * 4.0f;
                            avoidancePoint.z = posNow.z;
                        }
                        sameDistanceCount = 0;
                    }
                } else {
                    sameDistanceCount = 0;
                }
                lastDistanceToTarget = distToTarget;

                // Определяем точку движения
                Vec3 moveTarget = targetPos;
                
                if (avoiding && avoidanceCounter > 0) {
                    // Движемся к точке обхода
                    moveTarget = avoidancePoint;
                    avoidanceCounter--;
                    
                    // Проверяем, достаточно ли далеко ушли от препятствия
                    if (avoidanceCounter <= 0) {
                        avoiding = false;
                    }
                } else {
                    // Проверяем препятствия
                    Vec3 obstacleCheck = AvoidObstacle(targetPos);
                    if (obstacleCheck.x != targetPos.x || obstacleCheck.y != targetPos.y) {
                        moveTarget = obstacleCheck;
                    }
                }

                // Плавный поворот
                TurnToTarget(moveTarget);
                
                // Движение вперед с контролем
                if (abs(currentAngleDiff) < 30) {
                    PressW();
                    ReleaseS();
                } else {
                    // Слишком большой угол, замедляемся
                    ReleaseW();
                    ReleaseS();
                }
                
                // Управление скоростью
                if (!carrying) {
                    if (distToTarget > 15.0f && abs(currentAngleDiff) < 15) {
                        PressShift();
                    } else {
                        ReleaseShift();
                    }
                    
                    // Прыжки только при необходимости
                    jumpCounter++;
                    if (jumpCounter % 4 == 0 && distToTarget > 5.0f && abs(currentAngleDiff) < 10 && !avoiding) { 
                        PressSpace(); 
                    }
                } else {
                    ReleaseShift();
                }
                
                // Плавное замедление при приближении к цели
                if (distToTarget < 5.0f) {
                    ReleaseShift();
                    // Короткие нажатия для точного позиционирования
                    if (distToTarget < 3.0f) {
                        PressW();
                        Sleep(30);
                        ReleaseW();
                        Sleep(20);
                    }
                }
            } else {
                StopAll();
                avoiding = false;
            }
            
            Sleep(30); // Уменьшено с 50 до 30 для более плавных движений
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
        
        avoiding = false;
        avoidanceCounter = 0;
        sameDistanceCount = 0;
        lastDistanceToTarget = 0;
        
        if (gameWnd) { 
            SetForegroundWindow(gameWnd);
            Sleep(500);
        }
        
        Print("[START] Bot started! Press F11 for emergency stop.", 10);
        Print("[INFO] Make sure the game window is ACTIVE!", 11);
        Print("[INFO] Do NOT minimize the game!", 11);
        
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
        cout << "  Avoiding:   " << (avoiding ? "[YES]" : "[NO]") << endl;
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
        cout << "  BOT CONTROLS (auto):" << endl;
        cout << "  W - Forward" << endl;
        cout << "  A - Left" << endl;
        cout << "  S - Back" << endl;
        cout << "  D - Right" << endl;
        cout << "  Shift - Sprint (no box)" << endl;
        cout << "  Space - Jump (to box)" << endl;
        SetColor(14);
        cout << "========================================" << endl;
        cout << "  IMPORTANT: Game window MUST be active!" << endl;
        cout << "  DO NOT minimize the game!" << endl;
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
    cout << "  [AVOID] Improved obstacle avoidance" << endl;
    cout << "  [SMOOTH] Smooth movement system" << endl;
    cout << "  [KEYS] W=Forward A=Left S=Back D=Right" << endl;
    cout << "==================================================" << endl;
    SetColor(14);
    cout << "\n  [WARN] Run as Administrator!" << endl;
    cout << "  [WARN] MTA Province must be running!" << endl;
    cout << "  [WARN] GAME WINDOW MUST BE ACTIVE!" << endl;
    cout << "  [WARN] DO NOT MINIMIZE THE GAME!" << endl;
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

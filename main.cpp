// main.cpp - MTA Bot with Bind System (Complete)
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

mutex memoryMutex;

void SetColor(int color) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, color);
}

struct Vec3 { float x, y, z; };
struct Marker { Vec3 pos; int type; bool active; bool collected; DWORD address; time_t spawnTime; };

class MemoryCache {
private:
    struct CacheEntry {
        DWORD address;
        vector<byte> data;
        chrono::steady_clock::time_point timestamp;
    };
    
    map<DWORD, CacheEntry> cache;
    chrono::milliseconds cacheLifetime;
    
public:
    MemoryCache() : cacheLifetime(100) {}
    
    template<typename T>
    bool ReadCached(HANDLE proc, DWORD address, T& value) {
        auto now = chrono::steady_clock::now();
        
        auto it = cache.find(address);
        if (it != cache.end()) {
            auto age = chrono::duration_cast<chrono::milliseconds>(now - it->second.timestamp);
            if (age < cacheLifetime && it->second.data.size() >= sizeof(T)) {
                memcpy(&value, it->second.data.data(), sizeof(T));
                return true;
            }
        }
        
        SIZE_T bytesRead;
        if (ReadProcessMemory(proc, (LPCVOID)address, &value, sizeof(T), &bytesRead)) {
            if (bytesRead == sizeof(T)) {
                CacheEntry entry;
                entry.address = address;
                entry.data.resize(sizeof(T));
                memcpy(entry.data.data(), &value, sizeof(T));
                entry.timestamp = now;
                cache[address] = entry;
                return true;
            }
        }
        return false;
    }
    
    void Clear() {
        cache.clear();
    }
};

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
    
    bool forwardBound, backBound, leftBound, rightBound, sprintBound;
    
    MemoryCache memoryCache;
    DWORD pedAddress;
    
    float currentAngleDiff;
    Vec3 avoidancePoint;
    bool avoiding;
    int avoidanceCounter;
    float lastDistanceToTarget;
    int sameDistanceCount;

public:
    Bot() : proc(NULL), pid(0), playerAddr(0), baseAddr(0), running(false),
            active(false), carrying(false), speed(0.25f), collected(0),
            delivered(0), emergency(false), stuckCount(0),
            jumpCounter(0), forwardBound(false), backBound(false),
            leftBound(false), rightBound(false), sprintBound(false),
            currentAngleDiff(0), avoiding(false), avoidanceCounter(0),
            lastDistanceToTarget(0), sameDistanceCount(0), pedAddress(0) {
        deliveryPoint = {0,0,0};
        lastPos = {0,0,0};
        avoidancePoint = {0,0,0};
        gameWnd = NULL;
        FindProcess();
        if (proc) FindAddresses();
    }

    ~Bot() { 
        CleanupBinds();
        if (proc) CloseHandle(proc); 
    }

    template<typename T>
    T Read(DWORD addr, bool useCache = true) {
        T val = {};
        if (!proc || !addr) return val;
        
        if (useCache) {
            lock_guard<mutex> lock(memoryMutex);
            if (memoryCache.ReadCached(proc, addr, val)) {
                return val;
            }
        }
        
        SIZE_T bytesRead;
        if (ReadProcessMemory(proc, (LPCVOID)addr, &val, sizeof(T), &bytesRead)) {
            if (bytesRead == sizeof(T)) {
                return val;
            }
        }
        
        typedef NTSTATUS(WINAPI* NtRead)(HANDLE, PVOID, PVOID, SIZE_T, SIZE_T*);
        static NtRead NtReadMem = NULL;
        if (!NtReadMem) {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            NtReadMem = (NtRead)GetProcAddress(ntdll, "NtReadVirtualMemory");
        }
        if (NtReadMem) { 
            SIZE_T read; 
            NtReadMem(proc, (PVOID)addr, &val, sizeof(T), &read); 
        }
        return val;
    }

    DWORD GetPedAddress() {
        if (pedAddress == 0 || pedAddress < 0x10000) {
            pedAddress = Read<DWORD>(playerAddr, false);
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
            baseAddr = (DWORD)mods[0];
        }
        
        vector<DWORD> knownAddresses = {
            0xB6F5F0, 0xB6F5F4, 0xB6F5EC, 0xB6F5E8,
            0xB74490, 0xB74494, 0xB6F5F8, 0xB6F5FC
        };
        
        for (DWORD addr : knownAddresses) {
            DWORD test = Read<DWORD>(addr, false);
            if (test > 0x10000 && test < 0x7FFFFFFF) {
                float x = Read<float>(test + 0x14, false);
                float y = Read<float>(test + 0x18, false);
                float z = Read<float>(test + 0x1C, false);
                
                if (x > -10000 && x < 10000 && 
                    y > -10000 && y < 10000 && 
                    z > -1000 && z < 10000) {
                    playerAddr = addr;
                    pedAddress = test;
                    Print("[OK] Player address found at 0x" + to_string(addr), 10);
                    return;
                }
            }
        }
        
        Print("[INFO] Scanning memory for player...", 11);
        for (DWORD addr = baseAddr; addr < baseAddr + 0x500000; addr += 4) {
            DWORD ped = Read<DWORD>(addr, false);
            if (ped > 0x10000 && ped < 0x7FFFFFFF) {
                float x = Read<float>(ped + 0x14, false);
                float y = Read<float>(ped + 0x18, false);
                float z = Read<float>(ped + 0x1C, false);
                
                if (x > -10000 && x < 10000 && 
                    y > -10000 && y < 10000 && 
                    z > -1000 && z < 10000 &&
                    x != 0 && y != 0) {
                    playerAddr = addr;
                    pedAddress = ped;
                    Print("[OK] Player address found at 0x" + to_string(addr), 10);
                    return;
                }
            }
        }
        
        Print("[ERROR] Failed to find addresses!", 12);
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

    // ==================== СИСТЕМА БИНДОВ ====================
    void SendChatCommand(string cmd) {
        if (!gameWnd) return;
        
        // Открываем консоль
        PostMessage(gameWnd, WM_KEYDOWN, 'T', 0);
        Sleep(50);
        PostMessage(gameWnd, WM_KEYUP, 'T', 0);
        Sleep(100);
        
        // Отправляем команду через буфер обмена
        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, cmd.length() + 1);
            if (hMem) {
                char* pMem = (char*)GlobalLock(hMem);
                strcpy(pMem, cmd.c_str());
                GlobalUnlock(hMem);
                SetClipboardData(CF_TEXT, hMem);
            }
            CloseClipboard();
        }
        
        // Вставляем команду
        keybd_event(VK_CONTROL, 0, 0, 0);
        keybd_event('V', 0, 0, 0);
        keybd_event('V', 0, KEYEVENTF_KEYUP, 0);
        keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
        Sleep(100);
        
        // Отправляем Enter
        keybd_event(VK_RETURN, 0, 0, 0);
        keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);
        Sleep(100);
    }

    void SetupBinds() {
        Print("[BINDS] Setting up movement binds...", 11);
        
        SendChatCommand("bind w forward");
        Sleep(200);
        SendChatCommand("bind s back");
        Sleep(200);
        SendChatCommand("bind a left");
        Sleep(200);
        SendChatCommand("bind d right");
        Sleep(200);
        SendChatCommand("bind shift sprint");
        Sleep(200);
        SendChatCommand("bind space jump");
        Sleep(200);
        
        forwardBound = true;
        backBound = true;
        leftBound = true;
        rightBound = true;
        sprintBound = true;
        
        Print("[BINDS] Movement binds configured!", 10);
    }

    void CleanupBinds() {
        if (!forwardBound && !backBound && !leftBound && !rightBound && !sprintBound) return;
        
        Print("[BINDS] Cleaning up binds...", 11);
        
        SendChatCommand("unbind w");
        Sleep(200);
        SendChatCommand("unbind s");
        Sleep(200);
        SendChatCommand("unbind a");
        Sleep(200);
        SendChatCommand("unbind d");
        Sleep(200);
        SendChatCommand("unbind shift");
        Sleep(200);
        SendChatCommand("unbind space");
        Sleep(200);
        
        forwardBound = false;
        backBound = false;
        leftBound = false;
        rightBound = false;
        sprintBound = false;
        
        Print("[BINDS] Binds cleaned up!", 10);
    }

    // ==================== УПРАВЛЕНИЕ ДВИЖЕНИЕМ ====================
    void SetControlState(string control, bool state) {
        if (!gameWnd) return;
        
        if (state) {
            if (control == "forward") SendChatCommand("setControlState forward true");
            else if (control == "back") SendChatCommand("setControlState back true");
            else if (control == "left") SendChatCommand("setControlState left true");
            else if (control == "right") SendChatCommand("setControlState right true");
            else if (control == "sprint") SendChatCommand("setControlState sprint true");
            else if (control == "jump") SendChatCommand("setControlState jump true");
        } else {
            if (control == "forward") SendChatCommand("setControlState forward false");
            else if (control == "back") SendChatCommand("setControlState back false");
            else if (control == "left") SendChatCommand("setControlState left false");
            else if (control == "right") SendChatCommand("setControlState right false");
            else if (control == "sprint") SendChatCommand("setControlState sprint false");
            else if (control == "jump") SendChatCommand("setControlState jump false");
        }
    }

    void PressForward() { SetControlState("forward", true); }
    void ReleaseForward() { SetControlState("forward", false); }
    void PressBack() { SetControlState("back", true); }
    void ReleaseBack() { SetControlState("back", false); }
    void PressLeft() { SetControlState("left", true); }
    void ReleaseLeft() { SetControlState("left", false); }
    void PressRight() { SetControlState("right", true); }
    void ReleaseRight() { SetControlState("right", false); }
    void PressSprint() { SetControlState("sprint", true); }
    void ReleaseSprint() { SetControlState("sprint", false); }
    void PressJump() { 
        SetControlState("jump", true);
        Sleep(50);
        SetControlState("jump", false);
    }

    void StopAll() {
        ReleaseForward();
        ReleaseBack();
        ReleaseLeft();
        ReleaseRight();
        ReleaseSprint();
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
        
        float diff = angle * 180.0f / 3.14159f - 90.0f - currentAngle;
        while (diff > 180) diff -= 360;
        while (diff < -180) diff += 360;
        
        if (abs(diff) > 15) {
            if (diff > 0) { PressRight(); ReleaseLeft(); }
            else { PressLeft(); ReleaseRight(); }
        } else if (abs(diff) > 5) {
            if (diff > 0) { 
                PressRight(); Sleep(20); ReleaseRight();
                ReleaseLeft();
            } else { 
                PressLeft(); Sleep(20); ReleaseLeft();
                ReleaseRight();
            }
        } else {
            ReleaseLeft(); ReleaseRight();
        }
        
        currentAngleDiff = diff;
    }

    // ==================== СКАНИРОВАНИЕ МАРКЕРОВ ====================
    void ScanMarkers() {
        if (!proc || !baseAddr) return;
        auto now = chrono::high_resolution_clock::now();
        if (duration_cast<milliseconds>(now - lastScan).count() < 300) return;
        lastScan = now;
        
        Vec3 playerPos = GetPos();
        if (playerPos.x == 0 && playerPos.y == 0) return;
        
        const DWORD scanSize = 0x800000;
        const DWORD blockSize = 0x10000;
        
        for (DWORD blockStart = baseAddr; blockStart < baseAddr + scanSize; blockStart += blockSize) {
            vector<byte> buffer(blockSize);
            SIZE_T bytesRead;
            
            if (ReadProcessMemory(proc, (LPCVOID)blockStart, buffer.data(), blockSize, &bytesRead)) {
                for (DWORD offset = 0; offset + 0x14 <= bytesRead; offset += 0x28) {
                    float* x = (float*)(buffer.data() + offset);
                    float* y = (float*)(buffer.data() + offset + 4);
                    float* z = (float*)(buffer.data() + offset + 8);
                    int* type = (int*)(buffer.data() + offset + 0xC);
                    
                    if (*x < -5000 || *x > 5000) continue;
                    if (*y < -5000 || *y > 5000) continue;
                    if (*z < -1000 || *z > 10000) continue;
                    if (*x == 0 && *y == 0) continue;
                    if (*x == -2147483648 || *y == -2147483648) continue;
                    if (*type != 1 && *type != 2) continue;
                    
                    float dist = sqrt(pow(*x - playerPos.x, 2) + pow(*y - playerPos.y, 2));
                    if (dist > 500) continue;
                    
                    DWORD addr = blockStart + offset;
                    
                    bool exists = false;
                    for (auto& m : markers) {
                        float d = sqrt(pow(m.pos.x - *x, 2) + pow(m.pos.y - *y, 2));
                        if (d < 1.0f) { exists = true; break; }
                    }
                    if (exists) continue;
                    
                    bool col = false;
                    for (auto& m : collectedMarkers) {
                        float d = sqrt(pow(m.pos.x - *x, 2) + pow(m.pos.y - *y, 2));
                        if (d < 1.0f) { col = true; break; }
                    }
                    if (col) continue;
                    
                    if (*type == 1) { 
                        Marker m; 
                        m.pos = {*x, *y, *z}; 
                        m.type = *type; 
                        m.active = true; 
                        m.collected = false; 
                        m.address = addr; 
                        m.spawnTime = time(NULL); 
                        markers.push_back(m);
                        Print("[BOX] New box! X=" + to_string((int)*x) + " Y=" + to_string((int)*y), 10); 
                    }
                    else if (*type == 2) { 
                        deliveryPoint = {*x, *y, *z}; 
                        Print("[DROP] Drop point! X=" + to_string((int)*x) + " Y=" + to_string((int)*y), 11); 
                    }
                }
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

    // ==================== ИЗБЕГАНИЕ ПРЕПЯТСТВИЙ ====================
    Vec3 AvoidObstacle(Vec3 target) {
        Vec3 pos = GetPos();
        Vec3 result = target;
        float closestObstacleDist = 999999.0f;
        Vec3 closestObstacle = {0,0,0};
        bool foundObstacle = false;
        
        for (auto& o : obstacles) {
            float d = sqrt(pow(o.x - pos.x, 2) + pow(o.y - pos.y, 2));
            if (d < 5.0f && d < closestObstacleDist) {
                closestObstacleDist = d;
                closestObstacle = o;
                foundObstacle = true;
            }
        }
        
        if (foundObstacle) {
            float angleFromObstacle = atan2(pos.y - closestObstacle.y, pos.x - closestObstacle.x);
            float angleToTarget = atan2(target.y - pos.y, target.x - pos.x);
            
            float angleDiff = angleToTarget - angleFromObstacle;
            while (angleDiff > M_PI) angleDiff -= 2 * M_PI;
            while (angleDiff < -M_PI) angleDiff += 2 * M_PI;
            
            float avoidAngle;
            if (angleDiff > 0) {
                avoidAngle = angleFromObstacle + M_PI / 2;
            } else {
                avoidAngle = angleFromObstacle - M_PI / 2;
            }
            
            float avoidDist = 7.0f;
            result.x = pos.x + cos(avoidAngle) * avoidDist;
            result.y = pos.y + sin(avoidAngle) * avoidDist;
            result.z = pos.z;
            
            if (!avoiding) {
                avoiding = true;
                avoidanceCounter = 30;
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

    // ==================== ГЛАВНЫЙ ЦИКЛ ====================
    void BotLoop() {
        active = true;
        Print("[START] Bot started! Press F11 for emergency stop.", 10);
        
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
            
            float move = sqrt(pow(pos.x - lastPos.x, 2) + pow(pos.y - lastPos.y, 2));
            if (move < 0.05f && hasTarget) {
                stuckCount++;
                if (stuckCount > 30) {
                    Print("[WARN] Stuck! Avoiding...", 14);
                    
                    if (avoiding) {
                        PressJump(); Sleep(200); PressJump();
                    } else {
                        avoiding = true;
                        avoidanceCounter = 40;
                        
                        float currentAngle = 0;
                        DWORD ped = GetPedAddress();
                        if (ped && ped > 0x10000 && ped < 0x7FFFFFFF) {
                            currentAngle = Read<float>(ped + 0x20) * M_PI / 180.0f;
                        }
                        
                        avoidancePoint.x = pos.x + cos(currentAngle + M_PI / 2) * 5.0f;
                        avoidancePoint.y = pos.y + sin(currentAngle + M_PI / 2) * 5.0f;
                        avoidancePoint.z = pos.z;
                        
                        obstacles.push_back(pos);
                    }
                    
                    stuckCount = 0;
                }
            } else { 
                stuckCount = 0; 
                if (move > 0.1f) {
                    avoiding = false;
                }
            }
            lastPos = pos;

            if (scanCount % 3 == 0) {
                ScanMarkers();
            }

            if (carrying) {
                if (deliveryPoint.x != 0) {
                    float dist = sqrt(pow(deliveryPoint.x - pos.x, 2) + pow(deliveryPoint.y - pos.y, 2));
                    if (dist < 2.0f) {
                        carrying = false; delivered++; ReleaseSprint(); StopAll();
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
                        ReleaseSprint(); StopAll();
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

            if (hasTarget) {
                Vec3 posNow = GetPos();
                float distToTarget = sqrt(pow(targetPos.x - posNow.x, 2) + pow(targetPos.y - posNow.y, 2));
                
                if (distToTarget < 2.0f) { 
                    hasTarget = false; 
                    StopAll(); 
                    avoiding = false;
                    continue; 
                }

                if (abs(distToTarget - lastDistanceToTarget) < 0.1f) {
                    sameDistanceCount++;
                    if (sameDistanceCount > 20) {
                        if (!avoiding) {
                            avoiding = true;
                            avoidanceCounter = 30;
                            
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

                Vec3 moveTarget = targetPos;
                
                if (avoiding && avoidanceCounter > 0) {
                    moveTarget = avoidancePoint;
                    avoidanceCounter--;
                    
                    if (avoidanceCounter <= 0) {
                        avoiding = false;
                    }
                } else {
                    Vec3 obstacleCheck = AvoidObstacle(targetPos);
                    if (obstacleCheck.x != targetPos.x || obstacleCheck.y != targetPos.y) {
                        moveTarget = obstacleCheck;
                    }
                }

                TurnToTarget(moveTarget);
                
                if (abs(currentAngleDiff) < 30) {
                    PressForward();
                    ReleaseBack();
                } else {
                    ReleaseForward();
                    ReleaseBack();
                }
                
                if (!carrying) {
                    if (distToTarget > 15.0f && abs(currentAngleDiff) < 15) {
                        PressSprint();
                    } else {
                        ReleaseSprint();
                    }
                    
                    jumpCounter++;
                    if (jumpCounter % 4 == 0 && distToTarget > 5.0f && abs(currentAngleDiff) < 10 && !avoiding) { 
                        PressJump(); 
                    }
                } else {
                    ReleaseSprint();
                }
                
                if (distToTarget < 5.0f) {
                    ReleaseSprint();
                    if (distToTarget < 3.0f) {
                        PressForward();
                        Sleep(30);
                        ReleaseForward();
                        Sleep(20);
                    }
                }
            } else {
                StopAll();
                avoiding = false;
            }
            
            Sleep(30);
        }
        
        StopAll();
        CleanupBinds();
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
        
        if (gameWnd) { 
            SetForegroundWindow(gameWnd);
            Sleep(500);
        }
        
        SetupBinds();
        Sleep(1000);
        
        Print("[START] Bot started! Press F11 for emergency stop.", 10);
        Print("[INFO] Using MTA binds for movement", 11);
        
        thread(&Bot::BotLoop, this).detach();
    }

    void Stop() { 
        running = false; 
        StopAll(); 
        CleanupBinds();
        Print("[STOP] Bot stopped. Collected: " + to_string(collected) + " | Delivered: " + to_string(delivered), 14); 
    }
    
    void EmergencyStop() { 
        emergency = true; 
        running = false; 
        active = false; 
        StopAll(); 
        CleanupBinds();
        Print("[EMERGENCY] EMERGENCY STOP! (F11)", 12); 
    }
    
    void SpeedUp() { 
        speed = min(0.8f, speed + 0.1f); 
        Print("[SPEED] Speed: " + to_string(speed), 11); 
    }
    
    void SpeedDown() { 
        speed = max(0.1f, speed - 0.1f); 
        Print("[SPEED] Speed: " + to_string(speed), 11); 
    }

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
        cout << "  Binds:      " << (forwardBound ? "[ACTIVE]" : "[INACTIVE]") << endl;
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
    cout << "  [BINDS] Uses MTA bind system for movement" << endl;
    cout << "  [BOX] Collects boxes" << endl;
    cout << "  [DROP] Delivers to drop point" << endl;
    cout << "  [AVOID] Improved obstacle avoidance" << endl;
    cout << "==================================================" << endl;
    SetColor(14);
    cout << "\n  [WARN] Run as Administrator!" << endl;
    cout << "  [WARN] MTA Province must be running!" << endl;
    cout << "  [WARN] Press F1 to start bot" << endl;
    cout << "  [WARN] Press F5 for status" << endl;
    cout << endl;
    SetColor(15);

    Bot bot;
    if (!bot.Ready()) {
        SetColor(12);
        cout << "\n[ERROR] MTA Province

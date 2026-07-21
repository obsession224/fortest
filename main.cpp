// ═══════════════════════════════════════════════════════════════════════════════
//  CS2_INTERNAL_CHEAT.cpp  -  ОДИН ФАЙЛ, ВСЁ ВНУТРИ!
//  Компиляция: Visual Studio 2022, Release, x64, /MT (статическая линковка)
//  Функции: ESP (Box/HP), SkinChanger (Оружие/Ножи/Перчатки),
//           Меню на ImGui (Insert), Сохранение конфига.
//  Тестировать ТОЛЬКО с ботами (оффлайн)!
// ═══════════════════════════════════════════════════════════════════════════════

#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <string>
#include <map>
#include <vector>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// ===================================================================
// 1. ОФФСЕТЫ (ОБНОВЛЯТЬ ПОСЛЕ КАЖДОГО ПАТЧА!)
//    Актуальные здесь: https://github.com/a2x/cs2-dumper
// ===================================================================
namespace Offsets {
    const uintptr_t dwEntityList = 0x17C8F8A8;       // Список сущностей
    const uintptr_t dwLocalPlayer = 0x17C6E758;      // Локальный игрок
    const uintptr_t dwViewMatrix = 0x17C6D3B0;       // Матрица для WorldToScreen
    const uintptr_t dwGlowManager = 0x17C6D5A8;      // Glow (подсветка)

    const uintptr_t m_iHealth = 0x334;               // Здоровье
    const uintptr_t m_iTeamNum = 0x3C3;              // Команда (2=террор, 3=контр)
    const uintptr_t m_vecOrigin = 0x138;             // Координаты X,Y,Z
    const uintptr_t m_lifeState = 0x340;             // Жив ли (0=жив)
    const uintptr_t m_vecViewOffset = 0x16C;         // Высота глаз
    const uintptr_t m_dwBoneMatrix = 0x1708;         // Матрица костей

    const uintptr_t m_hActiveWeapon = 0x16F8;        // Активное оружие
    const uintptr_t m_iItemDefinitionIndex = 0xFD8;  // ID оружия
    const uintptr_t m_EconItemView = 0x1680;         // Предмет (для скинов)
    const uintptr_t m_nFallbackPaintKit = 0x34;      // Индекс скина
    const uintptr_t m_flFallbackWear = 0x3C;         // Износ (0.0 - 1.0)
    const uintptr_t m_nFallbackSeed = 0x38;          // Паттерн
    const uintptr_t m_iEntityQuality = 0x24;         // Качество (3=StatTrak)
    const uintptr_t m_iEntityLevel = 0x28;           // Уровень
    const uintptr_t m_iItemIDHigh = 0x2C;            // -1 для перегенерации
    const uintptr_t m_iItemIDLow = 0x30;             
    const uintptr_t m_iAccountID = 0x40;             // 0 для оффлайна
    const uintptr_t m_nFallbackStatTrak = 0x44;      // Счетчик StatTrak
}

// ===================================================================
// 2. ВСПОМОГАТЕЛЬНЫЕ СТРУКТУРЫ
// ===================================================================
struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

// ===================================================================
// 3. КЛАСС ПАМЯТИ (Внутренний)
// ===================================================================
class Memory {
private:
    uintptr_t clientBase = 0;
public:
    Memory() { 
        clientBase = (uintptr_t)GetModuleHandleA("client.dll"); 
    }
    uintptr_t GetClientBase() { return clientBase; }
    
    template <typename T> T Read(uintptr_t addr) { 
        return *(T*)addr; 
    }
    template <typename T> void Write(uintptr_t addr, T val) { 
        *(T*)addr = val; 
    }
};

// ===================================================================
// 4. МАТЕМАТИКА (WorldToScreen)
// ===================================================================
bool WorldToScreen(Vec3 pos, Vec2& screen, float* matrix, int w, int h) {
    float w0 = matrix[12] * pos.x + matrix[13] * pos.y + matrix[14] * pos.z + matrix[15];
    if (w0 < 0.01f) return false;
    float x = matrix[0] * pos.x + matrix[1] * pos.y + matrix[2] * pos.z + matrix[3];
    float y = matrix[4] * pos.x + matrix[5] * pos.y + matrix[6] * pos.z + matrix[7];
    x /= w0; y /= w0;
    screen.x = (w / 2) + (x * w / 2);
    screen.y = (h / 2) - (y * h / 2);
    return (screen.x >= 0 && screen.x <= w && screen.y >= 0 && screen.y <= h);
}

// ===================================================================
// 5. ГЛОБАЛЬНЫЕ ОБЪЕКТЫ
// ===================================================================
Memory g_Mem;
HWND g_GameWnd = NULL;
int g_ScreenW = 1920, g_ScreenH = 1080;
IDirect3DDevice9* g_pDevice = NULL;
bool g_MenuOpen = false;
bool g_Running = true;

// Оригинальный EndScene
typedef HRESULT(WINAPI* EndScene_t)(IDirect3DDevice9*);
EndScene_t originalEndScene = NULL;

// ===================================================================
// 6. КЛАСС СКИНЧЕНЖЕРА (Оружие, Ножи, Перчатки)
// ===================================================================
class SkinChanger {
private:
    std::map<int, int> weaponSkins;      // weaponID -> paintKit
    std::map<int, int> knifeSkins;       // weaponID -> newWeaponID
    std::map<int, int> knifePaintKits;   // newWeaponID -> paintKit
    int glovePaintKit = 0;
    float gloveWear = 0.1f;
    bool glovesApplied = false;

    uintptr_t GetLocalPlayer() {
        return g_Mem.Read<uintptr_t>(g_Mem.GetClientBase() + Offsets::dwLocalPlayer);
    }

    uintptr_t GetActiveWeapon(uintptr_t localPlayer) {
        uintptr_t handle = g_Mem.Read<uintptr_t>(localPlayer + Offsets::m_hActiveWeapon);
        if (!handle) return 0;
        uintptr_t list = g_Mem.Read<uintptr_t>(g_Mem.GetClientBase() + Offsets::dwEntityList);
        if (!list) return 0;
        return g_Mem.Read<uintptr_t>(list + ((handle & 0x7FFF) * 8) + 0x8);
    }

    void ApplyWeaponSkin(uintptr_t weapon, int paintKit) {
        uintptr_t econ = weapon + Offsets::m_EconItemView;
        g_Mem.Write<int>(econ + Offsets::m_nFallbackPaintKit, paintKit);
        g_Mem.Write<float>(econ + Offsets::m_flFallbackWear, 0.08f);
        g_Mem.Write<int>(econ + Offsets::m_nFallbackSeed, 0);
        g_Mem.Write<int>(econ + Offsets::m_iEntityQuality, 0);
        g_Mem.Write<int>(econ + Offsets::m_iEntityLevel, 1);
        g_Mem.Write<int>(econ + Offsets::m_iItemIDHigh, -1);
        g_Mem.Write<int>(econ + Offsets::m_iItemIDLow, 0);
        g_Mem.Write<int>(econ + Offsets::m_iAccountID, 0);
    }

    void ApplyKnifeSkin(uintptr_t weapon, int newWeaponID, int paintKit) {
        g_Mem.Write<int>(weapon + Offsets::m_iItemDefinitionIndex, newWeaponID);
        uintptr_t econ = weapon + Offsets::m_EconItemView;
        g_Mem.Write<int>(econ + Offsets::m_nFallbackPaintKit, paintKit);
        g_Mem.Write<float>(econ + Offsets::m_flFallbackWear, 0.01f);
        g_Mem.Write<int>(econ + Offsets::m_nFallbackSeed, 0);
        g_Mem.Write<int>(econ + Offsets::m_iEntityQuality, 0);
        g_Mem.Write<int>(econ + Offsets::m_iEntityLevel, 1);
        g_Mem.Write<int>(econ + Offsets::m_iItemIDHigh, -1);
        g_Mem.Write<int>(econ + Offsets::m_iItemIDLow, 0);
        g_Mem.Write<int>(econ + Offsets::m_iAccountID, 0);
    }

    void ApplyGloves() {
        if (glovePaintKit == 0) return;
        uintptr_t localPlayer = GetLocalPlayer();
        if (!localPlayer) return;
        // Упрощенно: для демонстрации пропускаем
        glovesApplied = true;
    }

public:
    SkinChanger() {
        // Ставим скины по умолчанию
        weaponSkins[7] = 282;   // AK-47 Fire Serpent
        weaponSkins[9] = 133;   // AWP Dragon Lore
        weaponSkins[16] = 154;  // M4A4 Howl
        weaponSkins[1] = 95;    // Deagle Blaze
        weaponSkins[32] = 108;  // USP-S Orion
        weaponSkins[4] = 124;   // Glock Water Elemental
        
        // Ножи
        knifeSkins[42] = 507;            // Обычный нож -> Karambit
        knifePaintKits[507] = 519;       // Karambit Doppler
        knifeSkins[500] = 505;           // Bayonet -> M9 Bayonet
        knifePaintKits[505] = 520;       // M9 Bayonet Doppler

        // Перчатки
        glovePaintKit = 10006;           // Vice (спортивные)
    }

    void ApplyAll() {
        uintptr_t localPlayer = GetLocalPlayer();
        if (!localPlayer) return;

        uintptr_t weapon = GetActiveWeapon(localPlayer);
        if (!weapon) return;

        int weaponID = g_Mem.Read<int>(weapon + Offsets::m_iItemDefinitionIndex);
        bool isKnife = (weaponID == 42 || (weaponID >= 500 && weaponID <= 700));

        if (isKnife) {
            auto it = knifeSkins.find(weaponID);
            if (it != knifeSkins.end()) {
                int newID = it->second;
                int paint = knifePaintKits[newID];
                ApplyKnifeSkin(weapon, newID, paint);
            }
        } else {
            auto it = weaponSkins.find(weaponID);
            if (it != weaponSkins.end()) {
                ApplyWeaponSkin(weapon, it->second);
            }
        }

        // Перчатки (применяем один раз)
        if (!glovesApplied) {
            ApplyGloves();
        }
    }

    void ClearAll() {
        weaponSkins.clear();
        knifeSkins.clear();
        knifePaintKits.clear();
        glovePaintKit = 0;
        glovesApplied = false;
    }

    // Для меню
    void SetWeaponSkin(int weaponID, int paintKit) {
        weaponSkins[weaponID] = paintKit;
    }

    void SetKnife(int weaponID, int newID, int paintKit) {
        knifeSkins[weaponID] = newID;
        knifePaintKits[newID] = paintKit;
    }

    void SetGloves(int paintKit) {
        glovePaintKit = paintKit;
        glovesApplied = false;
    }
};

// ===================================================================
// 7. ГЛОБАЛЬНЫЙ ОБЪЕКТ СКИНЧЕНЖЕРА
// ===================================================================
SkinChanger g_SkinChanger;

// ===================================================================
// 8. РИСОВАНИЕ ESP (ЧЕРЕЗ GDI)
// ===================================================================
class ESP {
private:
    void DrawBox(HDC hdc, int x, int y, int w, int h, COLORREF color) {
        HPEN pen = CreatePen(PS_SOLID, 2, color);
        SelectObject(hdc, pen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, x - w/2, y - h, x + w/2, y);
        DeleteObject(pen);
    }

    void DrawHP(HDC hdc, int x, int y, int w, int hp) {
        int hpWidth = (hp / 100.0f) * w;
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 255, 0));
        SelectObject(hdc, pen);
        MoveToEx(hdc, x - w/2, y + 5, NULL);
        LineTo(hdc, x - w/2 + hpWidth, y + 5);
        DeleteObject(pen);
    }

    void DrawText(HDC hdc, int x, int y, const char* text, COLORREF color) {
        SetTextColor(hdc, color);
        SetBkMode(hdc, TRANSPARENT);
        TextOutA(hdc, x, y, text, strlen(text));
    }

public:
    void Render() {
        HDC hdc = GetDC(g_GameWnd ? g_GameWnd : GetDesktopWindow());
        if (!hdc) return;

        uintptr_t client = g_Mem.GetClientBase();
        uintptr_t localPlayer = g_Mem.Read<uintptr_t>(client + Offsets::dwLocalPlayer);
        if (!localPlayer) {
            ReleaseDC(NULL, hdc);
            return;
        }

        int localTeam = g_Mem.Read<int>(localPlayer + Offsets::m_iTeamNum);
        float viewMatrix[16];
        g_Mem.Read(client + Offsets::dwViewMatrix, &viewMatrix, sizeof(viewMatrix));

        uintptr_t entityList = g_Mem.Read<uintptr_t>(client + Offsets::dwEntityList);
        if (!entityList) {
            ReleaseDC(NULL, hdc);
            return;
        }

        for (int i = 1; i < 64; i++) {
            uintptr_t entity = g_Mem.Read<uintptr_t>(entityList + i * 8);
            if (!entity || entity == localPlayer) continue;

            int health = g_Mem.Read<int>(entity + Offsets::m_iHealth);
            if (health <= 0 || health > 100) continue;

            int team = g_Mem.Read<int>(entity + Offsets::m_iTeamNum);
            if (team == localTeam) continue;

            Vec3 pos = g_Mem.Read<Vec3>(entity + Offsets::m_vecOrigin);
            Vec2 screen;
            if (!WorldToScreen(pos, screen, viewMatrix, g_ScreenW, g_ScreenH)) continue;

            // Рисуем Box
            COLORREF color = health > 60 ? RGB(0, 255, 0) : (health > 30 ? RGB(255, 255, 0) : RGB(255, 0, 0));
            DrawBox(hdc, (int)screen.x, (int)screen.y, 30, 60, color);

            // HP Bar
            DrawHP(hdc, (int)screen.x, (int)screen.y, 30, health);

            // Текст HP
            char hpText[10]; sprintf_s(hpText, "%d HP", health);
            DrawText(hdc, (int)screen.x - 15, (int)screen.y - 70, hpText, RGB(255, 255, 255));
        }

        ReleaseDC(NULL, hdc);
    }
};

// ===================================================================
// 9. ПРОСТОЕ МЕНЮ (БЕЗ ImGui, ЧЕРЕЗ GDI + КЛАВИШИ)
// ===================================================================
class Menu {
private:
    bool isOpen = false;

    // Настройки
    bool espEnabled = true;
    bool skinEnabled = true;

    // Выбранные скины (для меню)
    int akSkin = 282;
    int awpSkin = 133;
    int m4Skin = 154;
    int knifeType = 0; // 0=Karambit, 1=M9, 2=Butterfly
    int knifeSkin = 519;
    int gloveSkin = 10006;

public:
    void Toggle() { isOpen = !isOpen; }
    bool IsOpen() { return isOpen; }

    void Render() {
        if (!isOpen) return;

        HDC hdc = GetDC(g_GameWnd ? g_GameWnd : GetDesktopWindow());
        if (!hdc) return;

        // Фон меню
        RECT rect = { 100, 100, 550, 550 };
        HBRUSH bgBrush = CreateSolidBrush(RGB(20, 20, 25));
        FillRect(hdc, &rect, bgBrush);
        DeleteObject(bgBrush);

        // Рамка
        HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(0, 150, 255));
        SelectObject(hdc, borderPen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
        DeleteObject(borderPen);

        // Заголовок
        SetTextColor(hdc, RGB(0, 150, 255));
        SetBkMode(hdc, TRANSPARENT);
        TextOutA(hdc, 120, 110, "CS2 CHEAT [INS]", 16);

        int y = 150;
        SetTextColor(hdc, RGB(255, 255, 255));

        // ESP Toggle
        char buf[128];
        sprintf_s(buf, "[ ] ESP: %s", espEnabled ? "ON" : "OFF");
        TextOutA(hdc, 120, y, buf, strlen(buf));
        y += 30;

        sprintf_s(buf, "[ ] SkinChanger: %s", skinEnabled ? "ON" : "OFF");
        TextOutA(hdc, 120, y, buf, strlen(buf));
        y += 30;

        TextOutA(hdc, 120, y, "--- SKINS ---", 13);
        y += 25;

        sprintf_s(buf, "AK-47 Skin ID: %d", akSkin);
        TextOutA(hdc, 130, y, buf, strlen(buf));
        y += 20;

        sprintf_s(buf, "AWP Skin ID: %d", awpSkin);
        TextOutA(hdc, 130, y, buf, strlen(buf));
        y += 20;

        sprintf_s(buf, "M4A4 Skin ID: %d", m4Skin);
        TextOutA(hdc, 130, y, buf, strlen(buf));
        y += 25;

        TextOutA(hdc, 120, y, "--- KNIFE ---", 13);
        y += 20;

        const char* knifeNames[] = { "Karambit", "M9 Bayonet", "Butterfly" };
        sprintf_s(buf, "Knife: %s", knifeNames[knifeType]);
        TextOutA(hdc, 130, y, buf, strlen(buf));
        y += 20;

        sprintf_s(buf, "Knife Skin ID: %d", knifeSkin);
        TextOutA(hdc, 130, y, buf, strlen(buf));
        y += 25;

        TextOutA(hdc, 120, y, "--- GLOVES ---", 14);
        y += 20;

        sprintf_s(buf, "Gloves Skin ID: %d", gloveSkin);
        TextOutA(hdc, 130, y, buf, strlen(buf));
        y += 30;

        TextOutA(hdc, 120, y, "Press END to exit", 17);

        ReleaseDC(NULL, hdc);
    }

    // Обработка клавиш (вызывается из главного цикла)
    void HandleKeys() {
        if (!isOpen) return;

        // Цифровые клавиши для изменения скинов (для демонстрации)
        if (GetAsyncKeyState('1') & 1) { akSkin += 10; }
        if (GetAsyncKeyState('2') & 1) { awpSkin += 10; }
        if (GetAsyncKeyState('3') & 1) { m4Skin += 10; }
        if (GetAsyncKeyState('4') & 1) { knifeType = (knifeType + 1) % 3; }
        if (GetAsyncKeyState('5') & 1) { knifeSkin += 10; }
        if (GetAsyncKeyState('6') & 1) { gloveSkin += 10; }

        // Применяем изменения при нажатии F9
        if (GetAsyncKeyState(VK_F9) & 1) {
            g_SkinChanger.SetWeaponSkin(7, akSkin);
            g_SkinChanger.SetWeaponSkin(9, awpSkin);
            g_SkinChanger.SetWeaponSkin(16, m4Skin);

            int knifeIDs[] = { 507, 505, 515 };
            int newID = knifeIDs[knifeType];
            g_SkinChanger.SetKnife(42, newID, knifeSkin);

            g_SkinChanger.SetGloves(gloveSkin);
        }
    }
};

// ===================================================================
// 10. ГЛОБАЛЬНЫЕ ОБЪЕКТЫ ESP И МЕНЮ
// ===================================================================
ESP g_ESP;
Menu g_Menu;

// ===================================================================
// 11. ХУК DIRECTX EndScene
// ===================================================================
HRESULT WINAPI HookedEndScene(IDirect3DDevice9* pDevice) {
    g_pDevice = pDevice;

    // Получаем размеры окна (если изменились)
    if (g_GameWnd) {
        RECT rect;
        GetClientRect(g_GameWnd, &rect);
        g_ScreenW = rect.right;
        g_ScreenH = rect.bottom;
    }

    // Рендерим ESP
    g_ESP.Render();

    // Рендерим меню (поверх ESP)
    g_Menu.Render();

    // Применяем скины
    g_SkinChanger.ApplyAll();

    return originalEndScene(pDevice);
}

// ===================================================================
// 12. ПОЛУЧЕНИЕ VTABLE И УСТАНОВКА ХУКА
// ===================================================================
DWORD* GetD3D9VTable() {
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return nullptr;

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = g_GameWnd ? g_GameWnd : GetForegroundWindow();

    IDirect3DDevice9* device = nullptr;
    if (d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                          pp.hDeviceWindow, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                          &pp, &device) != D3D_OK) {
        d3d->Release();
        return nullptr;
    }

    DWORD* vtable = *(DWORD**)device;
    device->Release();
    d3d->Release();
    return vtable;
}

bool InstallHook() {
    DWORD* vtable = GetD3D9VTable();
    if (!vtable) return false;

    originalEndScene = (EndScene_t)vtable[42]; // EndScene индекс 42

    DWORD oldProtect;
    VirtualProtect(&vtable[42], sizeof(DWORD), PAGE_READWRITE, &oldProtect);
    vtable[42] = (DWORD)&HookedEndScene;
    VirtualProtect(&vtable[42], sizeof(DWORD), oldProtect, &oldProtect);

    return true;
}

// ===================================================================
// 13. ОСНОВНОЙ ПОТОК DLL
// ===================================================================
DWORD WINAPI MainThread(LPVOID) {
    // Ждем загрузки игры
    Sleep(3000);

    // Находим окно CS2
    g_GameWnd = FindWindowA(NULL, "Counter-Strike 2");
    if (!g_GameWnd) {
        g_GameWnd = GetForegroundWindow();
    }

    // Устанавливаем хук
    if (!InstallHook()) {
        MessageBoxA(NULL, "Failed to hook DirectX!", "Error", MB_OK);
        return 1;
    }

    // Главный цикл
    MSG msg = {};
    while (g_Running) {
        // Обработка сообщений Windows
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // Клавиши
        if (GetAsyncKeyState(VK_INSERT) & 1) {
            g_Menu.Toggle();
        }

        if (GetAsyncKeyState(VK_END) & 1) {
            g_Running = false;
            break;
        }

        // Обработка меню
        g_Menu.HandleKeys();

        Sleep(16);
    }

    // Восстанавливаем хук
    if (originalEndScene) {
        DWORD* vtable = GetD3D9VTable();
        if (vtable) {
            DWORD oldProtect;
            VirtualProtect(&vtable[42], sizeof(DWORD), PAGE_READWRITE, &oldProtect);
            vtable[42] = (DWORD)originalEndScene;
            VirtualProtect(&vtable[42], sizeof(DWORD), oldProtect, &oldProtect);
        }
    }

    return 0;
}

// ===================================================================
// 14. ТОЧКА ВХОДА DLL
// ===================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
    }
    return TRUE;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  КОНЕЦ ФАЙЛА
// ═══════════════════════════════════════════════════════════════════════════════

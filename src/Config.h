#pragma once
#include <string>
#include <vector>
#include <map>
#include <opencv2/opencv.hpp>
#include "Input.h"

// Конфігурація кольорів для детекції (HSV та BGR)
struct ColorConfig {
    // Детекція імені NPC
    cv::Scalar npc_name_from_hsv        = {0, 0, 240};
    cv::Scalar npc_name_to_hsv          = {0, 0, 255};
    double     npc_name_color_threshold = 0.2;

    // HP/MP/CP бари персонажа (HSV)
    cv::Scalar my_hp_from_hsv = {2,   90,  120};
    cv::Scalar my_hp_to_hsv   = {5,   220, 170};
    cv::Scalar my_mp_from_hsv = {105, 100, 130};
    cv::Scalar my_mp_to_hsv   = {110, 255, 170};
    cv::Scalar my_cp_from_hsv = {16,  100, 120};
    cv::Scalar my_cp_to_hsv   = {22,  255, 200};

    // HP бар цілі (HSV)
    cv::Scalar target_hp_from_hsv = {0, 100, 100};
    cv::Scalar target_hp_to_hsv   = {2, 220, 140};

    // Кружечки вибору цілі (BGR)
    cv::Scalar target_gray_circle_bgr = {57,  60, 66};
    cv::Scalar target_blue_circle_bgr = {107, 48, 0};
    cv::Scalar target_red_circle_bgr  = {0,   4,  132};

    // Парсинг рядка "V1,V2,V3" в cv::Scalar; повертає def при помилці
    static cv::Scalar ParseScalar(const std::string& str, const cv::Scalar& def);
};

// Зчитує rdga1bot.ini і надає типізований доступ до параметрів.
class Config {
public:
    // [General]
    std::string window_title = "Lineage II";
    bool debug    = true;
    int log_level = 1; // 0=DEBUG 1=INFO 2=WARNING 3=ERROR 4=NONE

    // [Character]
    std::string char_class = "Mage"; // Mage, Archer, Spoiler

    // [Targeting]
    Input::KeyboardKey next_target_key = Input::KeyboardKey::F2; // /nexttarget macro
    std::vector<Input::KeyboardKey> target_macro_keys;
    int nearby_y_threshold = 200; // Screen-Y фільтр: cy < цього → "далеко" (0 = вимкнено)
    int max_far_rejects    = 5;   // Скільки разів відхилити "далекий" таргет → потім приймаємо

    // [Attack]
    std::vector<Input::KeyboardKey> attack_keys;
    double attack_wait     = 0.5;
    double attack_watchdog = 60.0; // секунди до watchdog таймауту
    std::vector<double> attack_delays; // per-skill затримки (порожньо = attack_wait для всіх)
    Input::KeyboardKey spoil_key = Input::KeyboardKey::F3;
    Input::KeyboardKey sweep_key = Input::KeyboardKey::F4;

    // Повернути затримку для скілу idx (з ротацією по вектору)
    double GetAttackDelay(int idx) const {
        if (attack_delays.empty()) return attack_wait;
        return attack_delays[idx % (int)attack_delays.size()];
    }

    // [Loot]
    Input::KeyboardKey loot_key = Input::KeyboardKey::F5;
    int loot_count = 10;
    bool loot_enabled = true; // false = пропустити очікування авто-лута, йти одразу до TARGETING

    // [Potions]
    Input::KeyboardKey hp_key = Input::KeyboardKey::F6;
    int hp_threshold = 70;
    Input::KeyboardKey mp_key = Input::KeyboardKey::F7;
    int mp_threshold = 40;
    Input::KeyboardKey cp_key = Input::KeyboardKey::F8;
    int cp_threshold = 90;

    // [Buffs]
    bool buff_enabled              = true;  // false = не бафатись взагалі
    std::vector<Input::KeyboardKey> buff_keys;
    int  buff_interval             = 900;  // секунди між перебафами (15 хв)
    bool buff_use_altb             = false; // ALT+B + mouse clicks замість buff_keys
    int  buff_post_combat_cooldown = 20;   // секунди після kill до бафу (флаг спадає)
    int  buff_tab_x = 2409, buff_tab_y = 425;         // координати вкладки "Баффер"
    int  buff_profile_x = 2479, buff_profile_y = 380; // координати профілю "tty"

    // [Special]
    bool has_pokemon_key = false;
    Input::KeyboardKey pokemon_key = Input::KeyboardKey::F3; // дія на мертвий таргет

    // [Telegram]
    std::string tg_token;
    std::string tg_chat_id;
    bool tg_on_death       = true;
    int  tg_stats_interval = 3600;

    // [Movement] — ЗАВЖДИ стрілки, НІКОЛИ W/S/A/D (відкривають чат в L2)
    Input::KeyboardKey move_forward  = Input::KeyboardKey::Up;
    Input::KeyboardKey move_back     = Input::KeyboardKey::Down;
    Input::KeyboardKey rotate_left   = Input::KeyboardKey::Left;
    Input::KeyboardKey rotate_right  = Input::KeyboardKey::Right;

    // [Stats]
    int auto_save_kills = 50; // авто-збереження stats кожні N kills (0 = вимкнено)

    // [Navigation] — детекція перешкод та застрягання
    bool nav_stuck_detection = true;  // frame diff: детекція "не рухаємось" після WalkForward
    bool nav_wall_detection  = false; // Sobel: детекція стін попереду (experimental)
    bool nav_flow_detection  = false; // Lucas-Kanade optical flow (experimental, +10мс/тік)
    int  nav_stuck_threshold = 2;     // скільки тіків "не рухаємось" поспіль → евакуаційна ротація

    // [Patrol] — патруль по маршруту коли немає мобів
    // Формат PatrolPath: кроки через кому. Кожен крок — літера+мілісекунди:
    //   F = forward, B = back, L = rotate-left, R = rotate-right
    //   Приклад: F1500,R500,F800,L300,F1000
    struct PatrolStep {
        enum class Dir { Forward, Back, RotateLeft, RotateRight };
        Dir dir = Dir::Forward;
        int ms  = 500;
    };
    bool patrol_enabled = false;
    int  patrol_trigger_attempts = 30; // скільки спроб без мобів → старт patrol
    std::vector<PatrolStep> patrol_path;

    // [Vision]
    bool use_robust_bar = true; // медіана 3 рядків замість середнього

    // Позиція TargetStatusWnd (читається з WindowsInfo.ini L2 клієнта)
    std::string windows_info_path;   // шлях до WindowsInfo.ini
    int target_wnd_x = 598;          // TargetStatusWnd posX (дефолт із WindowsInfo.ini)
    int target_wnd_y = 0;            // TargetStatusWnd posY
    int target_wnd_w = 179;          // TargetStatusWnd width
    int target_wnd_h = 46;           // TargetStatusWnd height

    // [KnownList] — KnownList сканер (OffsetScanner + KnownListReader)
    bool        knownlist_enabled      = false;
    bool        knownlist_autoscan     = true;   // автопошук playerBase при старті
    std::string knownlist_offsets_file = "offsets.json"; // файл кешованих offsets
    float       knownlist_max_range    = 2500.f; // L2 units — радіус пошуку мобів (> радіус мінімарти ~1560)

    // [Memory] — feature flags для memory read інтеграції (KnownList)
    // Головний вимикач: false = все як раніше (OpenCV only), true = з memory read
    bool mem_use_for_target_hp   = false; // HP моба з пам'яті замість OpenCV color detection
    bool mem_use_for_kill_detect = false; // isDead з пам'яті (instant, без 800мс debounce)
    bool mem_fallback_to_opencv  = true;  // при збої memory → fallback до OpenCV

    // [Navigation] — memory-based навігація до моба
    struct NavigationFeatures {
        bool  enabled         = false;  // вимкнено поки heading не відкалібровано
        float attack_range    = 150.f;  // дальність атаки в L2-юнітах (Dagger ~150)
        float angle_tolerance = 0.17f;  // допуск кута (рад) ~10°
        bool  use_heading     = false;  // використовувати heading (false = тільки рух)
    } navigation;

    // Нові offsets для MemReader
    uintptr_t mem_heading_off = 0;  // heading offset в PlayerObject

    // [MemReader] — читання пам'яті L2 процесу (Wine, /proc/PID/mem)
    // Всі значення = 0 → вимкнено (використовується OpenCV детекція)
    bool     mem_enabled     = false;
    std::string mem_proc_name = "l2.exe"; // ім'я процесу для пошуку
    uintptr_t mem_player_ptr  = 0;   // static offset від base l2.exe до pointer на PlayerObject
    std::vector<uintptr_t> mem_ptr_chain; // pointer chain offsets (порожньо = пряма адреса)
    uintptr_t mem_hp_off      = 0;
    uintptr_t mem_max_hp_off  = 0;
    uintptr_t mem_mp_off      = 0;
    uintptr_t mem_max_mp_off  = 0;
    uintptr_t mem_cp_off      = 0;
    uintptr_t mem_max_cp_off  = 0;
    uintptr_t mem_pos_x_off   = 0;
    uintptr_t mem_pos_y_off   = 0;
    uintptr_t mem_pos_z_off   = 0;

    // [Delays] — варіативні затримки для антидетекту (нормальний розподіл)
    struct DelayConfig {
        bool  enabled         = false;  // false = фіксовані затримки як раніше
        float attack_mean_ms  = 500.f,  attack_std_ms  = 75.f;  // між атаками
        float rotate_mean_ms  = 350.f,  rotate_std_ms  = 50.f;  // RotateLeft/Right
        float walk_mean_ms    = 800.f,  walk_std_ms    = 120.f; // WalkForward
        float potion_mean_ms  = 50.f,   potion_std_ms  = 15.f;  // потіони
    } delays;

    // [Geodata] — геодата L2J формату для навігації
    bool        geodata_enabled  = false;
    std::string geodata_path     = "./geodata/";
    bool        geodata_use_jps  = true;

    // [WeightedTargeting] — зважений вибір цілі
    struct WeightedTargetConfig {
        bool  enabled     = false;   // false = findNearestMob як раніше
        float w_distance  = 0.5f;   // вага відстані (ближче = краще)
        float w_low_hp    = 0.3f;   // вага низького HP
        float w_freshness = 0.2f;   // вага наявності name (region scan знайшов)
        float max_range   = 1200.f;
    } weighted_target;

    // [Colors_*]
    ColorConfig colors;

    bool IsSpoiler() const { return char_class == "Spoiler"; }

    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
    bool Validate() const; // перевірка конфігурації, виводить попередження
    bool InteractiveSetup();

private:
    std::map<std::string, std::map<std::string, std::string>> m_ini;

    void ParseINI(const std::string& text);
    std::string Get(const std::string& section, const std::string& key, const std::string& def = "") const;
    int    GetInt   (const std::string& section, const std::string& key, int def)    const;
    double GetDouble(const std::string& section, const std::string& key, double def) const;
    bool   GetBool  (const std::string& section, const std::string& key, bool def)   const;
    cv::Scalar GetScalar(const std::string& section, const std::string& key, const cv::Scalar& def) const;
    std::vector<Input::KeyboardKey> GetKeys(const std::string& section, const std::string& key) const;
    std::string KeyName(Input::KeyboardKey k) const;
};

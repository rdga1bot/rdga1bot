#include "Config.h"
#include "Utils.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <functional>

// ─── ColorConfig ───────────────────────────────────────────────────────────

cv::Scalar ColorConfig::ParseScalar(const std::string& str, const cv::Scalar& def) {
    std::istringstream ss(str);
    std::string tok;
    std::vector<double> vals;
    while (std::getline(ss, tok, ',')) {
        // Trim
        size_t s = tok.find_first_not_of(" \t");
        size_t e = tok.find_last_not_of(" \t");
        if (s == std::string::npos) continue;
        tok = tok.substr(s, e - s + 1);
        try { vals.push_back(std::stod(tok)); } catch (...) { return def; }
    }
    if (vals.size() < 3) return def;
    return cv::Scalar(vals[0], vals[1], vals[2]);
}

// Допоміжна функція: обрізати пробіли
static std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

void Config::ParseINI(const std::string& text) {
    m_ini.clear();
    std::string current_section;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos)
                current_section = Trim(line.substr(1, end - 1));
        } else {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = Trim(line.substr(0, eq));
                std::string val = Trim(line.substr(eq + 1));
                // Обрізати inline коментарі після #
                size_t hash = val.find('#');
                if (hash != std::string::npos)
                    val = Trim(val.substr(0, hash));
                m_ini[current_section][key] = val;
            }
        }
    }
}

std::string Config::Get(const std::string& section, const std::string& key, const std::string& def) const {
    auto sit = m_ini.find(section);
    if (sit == m_ini.end()) return def;
    auto kit = sit->second.find(key);
    if (kit == sit->second.end()) return def;
    return kit->second;
}

int Config::GetInt(const std::string& section, const std::string& key, int def) const {
    std::string v = Get(section, key);
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

double Config::GetDouble(const std::string& section, const std::string& key, double def) const {
    std::string v = Get(section, key);
    if (v.empty()) return def;
    try { return std::stod(v); } catch (...) { return def; }
}

bool Config::GetBool(const std::string& section, const std::string& key, bool def) const {
    std::string v = Get(section, key);
    if (v.empty()) return def;
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    if (v == "true" || v == "1" || v == "yes") return true;
    if (v == "false" || v == "0" || v == "no") return false;
    return def;
}

cv::Scalar Config::GetScalar(const std::string& section, const std::string& key, const cv::Scalar& def) const {
    std::string v = Get(section, key);
    if (v.empty()) return def;
    return ColorConfig::ParseScalar(v, def);
}

std::vector<Input::KeyboardKey> Config::GetKeys(const std::string& section, const std::string& key) const {
    std::string v = Get(section, key);
    std::vector<Input::KeyboardKey> result;
    if (v.empty()) return result;
    std::istringstream ss(v);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token = Trim(token);
        if (!token.empty()) {
            result.push_back(StringToKeyboardKey(token, Input::KeyboardKey::F1));
        }
    }
    return result;
}

std::vector<std::string> Config::GetStringList(const std::string& section,
                                                const std::string& key) const {
    std::string v = Get(section, key);
    std::vector<std::string> result;
    if (v.empty()) return result;
    std::istringstream ss(v);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token = Trim(token);
        if (!token.empty()) result.push_back(token);
    }
    return result;
}

bool Config::Load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[Config] Не вдалося відкрити " << path << " — використовуємо defaults\n";
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ParseINI(text);

    // [General]
    window_title = Get("General", "WindowTitle", window_title);
    debug = GetBool("General", "Debug", debug);

    // [Character]
    char_class = Get("Character", "Class", char_class);

    // [Targeting]
    next_target_key = StringToKeyboardKey(Get("Targeting", "NextTargetKey", "F2"), Input::KeyboardKey::F2);
    auto mk = GetKeys("Targeting", "MacroKeys");
    if (!mk.empty()) target_macro_keys = mk;
    nearby_y_threshold = GetInt("Targeting", "NearbyYThreshold", nearby_y_threshold);
    max_far_rejects    = GetInt("Targeting", "MaxFarRejects",    max_far_rejects);
    { auto v = GetStringList("Targeting", "MobNames"); if (!v.empty()) mob_names = v; }

    // [Attack]
    auto ak = GetKeys("Attack", "AttackKeys");
    if (!ak.empty()) attack_keys = ak;
    else if (attack_keys.empty()) attack_keys = { Input::KeyboardKey::F1 };
    attack_wait = GetDouble("Attack", "AttackWait", attack_wait);
    // AttackDelays: рядок через кому → вектор double
    {
        std::string v = Get("Attack", "AttackDelays", "");
        if (!v.empty()) {
            attack_delays.clear();
            std::istringstream ss(v);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                tok = Trim(tok);
                if (!tok.empty()) {
                    try { attack_delays.push_back(std::stod(tok)); } catch (...) {}
                }
            }
        }
    }
    spoil_key = StringToKeyboardKey(Get("Attack", "SpoilKey", "F3"), Input::KeyboardKey::F3);
    sweep_key = StringToKeyboardKey(Get("Attack", "SweepKey", "F4"), Input::KeyboardKey::F4);

    // [Loot]
    loot_key     = StringToKeyboardKey(Get("Loot", "LootKey", "F5"), Input::KeyboardKey::F5);
    loot_count   = GetInt ("Loot", "LootCount",   loot_count);
    loot_enabled = GetBool("Loot", "LootEnabled", loot_enabled);

    // [Potions]
    hp_key = StringToKeyboardKey(Get("Potions", "HP_Key", "F6"), Input::KeyboardKey::F6);
    hp_threshold = GetInt("Potions", "HP_Threshold", hp_threshold);
    mp_key = StringToKeyboardKey(Get("Potions", "MP_Key", "F7"), Input::KeyboardKey::F7);
    mp_threshold = GetInt("Potions", "MP_Threshold", mp_threshold);
    cp_key = StringToKeyboardKey(Get("Potions", "CP_Key", "F8"), Input::KeyboardKey::F8);
    cp_threshold = GetInt("Potions", "CP_Threshold", cp_threshold);

    // [Buffs]
    buff_enabled = GetBool("Buffs", "BuffEnabled", buff_enabled);
    auto bk = GetKeys("Buffs", "BuffKeys");
    if (!bk.empty()) buff_keys = bk;
    buff_interval             = GetInt ("Buffs", "BuffInterval",            buff_interval);
    buff_use_altb             = GetBool("Buffs", "BuffUseAltB",             buff_use_altb);
    buff_post_combat_cooldown = GetInt ("Buffs", "BuffPostCombatCooldown",  buff_post_combat_cooldown);
    buff_tab_x                = GetInt ("Buffs", "BuffTabX",                buff_tab_x);
    buff_tab_y                = GetInt ("Buffs", "BuffTabY",                buff_tab_y);
    buff_profile_x            = GetInt ("Buffs", "BuffProfileX",            buff_profile_x);
    buff_profile_y            = GetInt ("Buffs", "BuffProfileY",            buff_profile_y);

    // [Special]
    {
        std::string pk = Get("Special", "PokemonKey", "");
        if (!pk.empty()) {
            has_pokemon_key = true;
            pokemon_key = StringToKeyboardKey(pk, Input::KeyboardKey::F3);
        }
    }

    // [Telegram]
    tg_token          = Get("Telegram", "BotToken", tg_token);
    tg_chat_id        = Get("Telegram", "ChatID", tg_chat_id);
    tg_on_death       = GetBool("Telegram", "NotifyOnDeath", tg_on_death);
    tg_stats_interval = GetInt("Telegram", "StatsInterval", tg_stats_interval);

    // [Stats]
    auto_save_kills = GetInt("Stats", "AutoSaveInterval", auto_save_kills);

    // [General] log level
    {
        std::string lv = Get("General", "LogLevel", "");
        if      (lv == "DEBUG")   log_level = 0;
        else if (lv == "INFO")    log_level = 1;
        else if (lv == "WARNING") log_level = 2;
        else if (lv == "ERROR")   log_level = 3;
        else if (lv == "NONE")    log_level = 4;
    }

    // [Attack] watchdog
    attack_watchdog = GetDouble("Attack", "AttackWatchdog", attack_watchdog);

    // [Movement] — дефолт завжди стрілки
    move_forward  = StringToKeyboardKey(Get("Movement", "Forward",     "Up"),    Input::KeyboardKey::Up);
    move_back     = StringToKeyboardKey(Get("Movement", "Back",        "Down"),  Input::KeyboardKey::Down);
    rotate_left   = StringToKeyboardKey(Get("Movement", "RotateLeft",  "Left"),  Input::KeyboardKey::Left);
    rotate_right  = StringToKeyboardKey(Get("Movement", "RotateRight", "Right"), Input::KeyboardKey::Right);

    // [Navigation]
    nav_stuck_detection     = GetBool("Navigation", "StuckDetection",       nav_stuck_detection);
    nav_wall_detection      = GetBool("Navigation", "WallDetection",        nav_wall_detection);
    nav_flow_detection      = GetBool("Navigation", "FlowDetection",        nav_flow_detection);
    nav_stuck_threshold     = GetInt ("Navigation", "StuckThreshold",       nav_stuck_threshold);

    // [Patrol]
    patrol_enabled          = GetBool("Patrol", "PatrolEnabled",            patrol_enabled);
    patrol_trigger_attempts = GetInt ("Patrol", "PatrolTriggerAttempts",    patrol_trigger_attempts);
    {
        const std::string raw = Get("Patrol", "PatrolPath", "");
        patrol_path.clear();
        // Парсимо "F1500,R500,L300,B200,..."
        std::istringstream ss(raw);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token.empty()) continue;
            PatrolStep step;
            char dir = (char)std::toupper((unsigned char)token[0]);
            int ms = 0;
            try { ms = std::stoi(token.substr(1)); } catch (...) { continue; }
            if (ms <= 0) continue;
            switch (dir) {
                case 'F': step.dir = PatrolStep::Dir::Forward;     break;
                case 'B': step.dir = PatrolStep::Dir::Back;        break;
                case 'L': step.dir = PatrolStep::Dir::RotateLeft;  break;
                case 'R': step.dir = PatrolStep::Dir::RotateRight; break;
                default: continue;
            }
            step.ms = ms;
            patrol_path.push_back(step);
        }
    }

    // [Zone]
    zone_enabled = GetBool  ("Zone", "Enabled", zone_enabled);
    zone_x       = (float)GetDouble("Zone", "CenterX", zone_x);
    zone_y       = (float)GetDouble("Zone", "CenterY", zone_y);
    zone_radius  = (float)GetDouble("Zone", "Radius",  zone_radius);

    // [Rest] — mp_threshold (ще один ключ, той самий що і [Potions])
    mp_threshold = GetInt("Rest", "MPThreshold", mp_threshold);

    // [Vision]
    use_robust_bar = GetBool("Vision", "UseRobustBarDetection", use_robust_bar);
    windows_info_path = Get("Vision", "WindowsInfoPath", windows_info_path);

    // Парсимо WindowsInfo.ini L2 клієнта для визначення позиції TargetStatusWnd
    if (!windows_info_path.empty()) {
        std::ifstream wf(windows_info_path);
        if (wf.is_open()) {
            std::string wtext((std::istreambuf_iterator<char>(wf)), std::istreambuf_iterator<char>());
            // Парсимо вручну секцію [TargetStatusWnd]
            bool in_target = false;
            int wx = -1, wy = -1, ww = -1, wh = -1;
            std::istringstream wss(wtext);
            std::string wline;
            while (std::getline(wss, wline)) {
                wline = Trim(wline);
                if (wline == "[TargetStatusWnd]") {
                    in_target = true;
                    continue;
                }
                if (in_target) {
                    if (!wline.empty() && wline[0] == '[') break; // наступна секція
                    size_t eq = wline.find('=');
                    if (eq == std::string::npos) continue;
                    std::string k = Trim(wline.substr(0, eq));
                    std::string v = Trim(wline.substr(eq + 1));
                    try {
                        if      (k == "posX")   wx = std::stoi(v);
                        else if (k == "posY")   wy = std::stoi(v);
                        else if (k == "width")  ww = std::stoi(v);
                        else if (k == "height") wh = std::stoi(v);
                    } catch (...) {}
                }
            }
            if (wx >= 0) target_wnd_x = wx;
            if (wy >= 0) target_wnd_y = wy;
            if (ww > 0)  target_wnd_w = ww;
            if (wh > 0)  target_wnd_h = wh;
            std::cerr << "[Config] WindowsInfo: TargetStatusWnd posX=" << target_wnd_x
                      << " posY=" << target_wnd_y
                      << " width=" << target_wnd_w
                      << " height=" << target_wnd_h << "\n";
        } else {
            std::cerr << "[Config] WindowsInfoPath не знайдено: " << windows_info_path << "\n";
        }
    }

    // [Memory] — feature flags для memory read інтеграції
    mem_use_for_target_hp   = GetBool("Memory", "UseForTargetHP",   mem_use_for_target_hp);
    mem_use_for_kill_detect = GetBool("Memory", "UseForKillDetect", mem_use_for_kill_detect);
    mem_fallback_to_opencv  = GetBool("Memory", "FallbackToOpenCV", mem_fallback_to_opencv);

    // [Navigation] — memory-based навігація
    navigation.enabled         = GetBool  ("Navigation", "Enabled",        navigation.enabled);
    navigation.attack_range    = (float)GetDouble("Navigation", "AttackRange",   navigation.attack_range);
    navigation.angle_tolerance = (float)GetDouble("Navigation", "AngleTolerance",navigation.angle_tolerance);
    navigation.use_heading     = GetBool  ("Navigation", "UseHeading",      navigation.use_heading);

    // [KnownList]
    knownlist_enabled      = GetBool  ("KnownList", "Enabled",      knownlist_enabled);
    knownlist_autoscan     = GetBool  ("KnownList", "AutoScan",     knownlist_autoscan);
    knownlist_offsets_file = Get      ("KnownList", "OffsetsFile",  knownlist_offsets_file);
    knownlist_max_range    = (float)GetDouble("KnownList", "MaxRange", (double)knownlist_max_range);

    // [MemReader]
    mem_enabled   = GetBool  ("MemReader", "Enabled",    mem_enabled);
    mem_proc_name = Get      ("MemReader", "ProcName",   mem_proc_name);
    mem_player_ptr  = (uintptr_t)std::stoul(Get("MemReader", "PlayerPtr",  "0"), nullptr, 16);
    mem_hp_off      = (uintptr_t)std::stoul(Get("MemReader", "HP_Offset",  "0"), nullptr, 16);
    mem_max_hp_off  = (uintptr_t)std::stoul(Get("MemReader", "MaxHP_Offset","0"), nullptr, 16);
    mem_mp_off      = (uintptr_t)std::stoul(Get("MemReader", "MP_Offset",  "0"), nullptr, 16);
    mem_max_mp_off  = (uintptr_t)std::stoul(Get("MemReader", "MaxMP_Offset","0"), nullptr, 16);
    mem_cp_off      = (uintptr_t)std::stoul(Get("MemReader", "CP_Offset",  "0"), nullptr, 16);
    mem_max_cp_off  = (uintptr_t)std::stoul(Get("MemReader", "MaxCP_Offset","0"), nullptr, 16);
    mem_pos_x_off   = (uintptr_t)std::stoul(Get("MemReader", "PosX_Offset",    "0"), nullptr, 16);
    mem_pos_y_off   = (uintptr_t)std::stoul(Get("MemReader", "PosY_Offset",    "0"), nullptr, 16);
    mem_pos_z_off   = (uintptr_t)std::stoul(Get("MemReader", "PosZ_Offset",    "0"), nullptr, 16);
    mem_heading_off = (uintptr_t)std::stoul(Get("MemReader", "Heading_Offset", "0"), nullptr, 16);
    {
        // PtrChain = 0x10,0x44,0x0C  (pointer chain offsets hex)
        const std::string raw = Get("MemReader", "PtrChain", "");
        mem_ptr_chain.clear();
        std::istringstream ss(raw);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            tok.erase(0, tok.find_first_not_of(" \t"));
            if (!tok.empty())
                mem_ptr_chain.push_back((uintptr_t)std::stoul(tok, nullptr, 16));
        }
    }

    // [Delays] — варіативні затримки
    delays.enabled        = GetBool  ("Delays", "Enabled",       delays.enabled);
    delays.attack_mean_ms = (float)GetDouble("Delays", "AttackMeanMs",  (double)delays.attack_mean_ms);
    delays.attack_std_ms  = (float)GetDouble("Delays", "AttackStdMs",   (double)delays.attack_std_ms);
    delays.rotate_mean_ms = (float)GetDouble("Delays", "RotateMeanMs",  (double)delays.rotate_mean_ms);
    delays.rotate_std_ms  = (float)GetDouble("Delays", "RotateStdMs",   (double)delays.rotate_std_ms);
    delays.walk_mean_ms   = (float)GetDouble("Delays", "WalkMeanMs",    (double)delays.walk_mean_ms);
    delays.walk_std_ms    = (float)GetDouble("Delays", "WalkStdMs",     (double)delays.walk_std_ms);
    delays.potion_mean_ms = (float)GetDouble("Delays", "PotionMeanMs",  (double)delays.potion_mean_ms);
    delays.potion_std_ms  = (float)GetDouble("Delays", "PotionStdMs",   (double)delays.potion_std_ms);

    // [Geodata]
    geodata_enabled = GetBool  ("Geodata", "Enabled",  geodata_enabled);
    geodata_path    = Get      ("Geodata", "GeoPath",   geodata_path);
    geodata_use_jps = GetBool  ("Geodata", "UseJPS",    geodata_use_jps);

    // [WeightedTargeting]
    weighted_target.enabled    = GetBool  ("WeightedTargeting", "Enabled",        weighted_target.enabled);
    weighted_target.w_distance = (float)GetDouble("WeightedTargeting", "WeightDistance",  (double)weighted_target.w_distance);
    weighted_target.w_low_hp   = (float)GetDouble("WeightedTargeting", "WeightLowHP",     (double)weighted_target.w_low_hp);
    weighted_target.w_freshness= (float)GetDouble("WeightedTargeting", "WeightFreshness", (double)weighted_target.w_freshness);
    weighted_target.max_range  = (float)GetDouble("WeightedTargeting", "MaxRange",        (double)weighted_target.max_range);

    // [Fuzzy]
    fuzzy.enabled   = GetBool  ("Fuzzy", "Enabled",   fuzzy.enabled);
    fuzzy.threshold = GetDouble("Fuzzy", "Threshold",  fuzzy.threshold);

    // [TargetingTuning]
    targeting_tuning.minimap_dx_threshold     = GetInt("TargetingTuning","MinimapDxThreshold",    targeting_tuning.minimap_dx_threshold);
    targeting_tuning.minimap_rotate_limit     = GetInt("TargetingTuning","MinimapRotateLimit",    targeting_tuning.minimap_rotate_limit);
    targeting_tuning.dead_cycles_macro_switch = GetInt("TargetingTuning","DeadCyclesMacroSwitch", targeting_tuning.dead_cycles_macro_switch);
    targeting_tuning.macro_fallback_unreach   = GetInt("TargetingTuning","MacroFallbackUnreach",  targeting_tuning.macro_fallback_unreach);
    targeting_tuning.long_search_warn_at      = GetInt("TargetingTuning","LongSearchWarnAt",      targeting_tuning.long_search_warn_at);

    // [Threading]
    threading.enabled        = GetBool("Threading","Enabled",       threading.enabled);
    threading.cpu_affinity   = GetBool("Threading","CPUAffinity",   threading.cpu_affinity);
    threading.main_core      = GetInt ("Threading","MainCore",       threading.main_core);
    threading.vision_thread  = GetBool("Threading","VisionThread",  threading.vision_thread);
    threading.vision_core    = GetInt ("Threading","VisionCore",    threading.vision_core);
    threading.geodata_thread = GetBool("Threading","GeodataThread", threading.geodata_thread);
    threading.geodata_core   = GetInt ("Threading","GeodataCore",   threading.geodata_core);

    // [Colors_MyBars]
    colors.my_hp_from_hsv = GetScalar("Colors_MyBars", "HPFromHSV",  colors.my_hp_from_hsv);
    colors.my_hp_to_hsv   = GetScalar("Colors_MyBars", "HPToHSV",    colors.my_hp_to_hsv);
    colors.my_mp_from_hsv = GetScalar("Colors_MyBars", "MPFromHSV",  colors.my_mp_from_hsv);
    colors.my_mp_to_hsv   = GetScalar("Colors_MyBars", "MPToHSV",    colors.my_mp_to_hsv);
    colors.my_cp_from_hsv = GetScalar("Colors_MyBars", "CPFromHSV",  colors.my_cp_from_hsv);
    colors.my_cp_to_hsv   = GetScalar("Colors_MyBars", "CPToHSV",    colors.my_cp_to_hsv);

    // [Colors_Target]
    colors.target_hp_from_hsv = GetScalar("Colors_Target", "HPFromHSV", colors.target_hp_from_hsv);
    colors.target_hp_to_hsv   = GetScalar("Colors_Target", "HPToHSV",   colors.target_hp_to_hsv);

    // [Colors_TargetCircles]
    colors.target_gray_circle_bgr = GetScalar("Colors_TargetCircles", "GrayBGR", colors.target_gray_circle_bgr);
    colors.target_blue_circle_bgr = GetScalar("Colors_TargetCircles", "BlueBGR", colors.target_blue_circle_bgr);
    colors.target_red_circle_bgr  = GetScalar("Colors_TargetCircles", "RedBGR",  colors.target_red_circle_bgr);

    // [Colors_NPC]
    colors.npc_name_from_hsv        = GetScalar("Colors_NPC", "NameFromHSV", colors.npc_name_from_hsv);
    colors.npc_name_to_hsv          = GetScalar("Colors_NPC", "NameToHSV",   colors.npc_name_to_hsv);
    colors.npc_name_color_threshold = GetDouble("Colors_NPC", "NameColorThreshold", colors.npc_name_color_threshold);

    return true;
}

std::string Config::KeyName(Input::KeyboardKey k) const {
    switch (k) {
        case Input::KeyboardKey::F1:  return "F1";
        case Input::KeyboardKey::F2:  return "F2";
        case Input::KeyboardKey::F3:  return "F3";
        case Input::KeyboardKey::F4:  return "F4";
        case Input::KeyboardKey::F5:  return "F5";
        case Input::KeyboardKey::F6:  return "F6";
        case Input::KeyboardKey::F7:  return "F7";
        case Input::KeyboardKey::F8:  return "F8";
        case Input::KeyboardKey::F9:  return "F9";
        case Input::KeyboardKey::F10: return "F10";
        case Input::KeyboardKey::F11: return "F11";
        case Input::KeyboardKey::F12: return "F12";
        case Input::KeyboardKey::One:   return "1";
        case Input::KeyboardKey::Two:   return "2";
        case Input::KeyboardKey::Three: return "3";
        case Input::KeyboardKey::Four:  return "4";
        case Input::KeyboardKey::Five:  return "5";
        case Input::KeyboardKey::Six:   return "6";
        case Input::KeyboardKey::Seven: return "7";
        case Input::KeyboardKey::Eight: return "8";
        case Input::KeyboardKey::Nine:  return "9";
        case Input::KeyboardKey::Zero:  return "0";
        case Input::KeyboardKey::Num0:  return "Num0";
        case Input::KeyboardKey::Num1:  return "Num1";
        case Input::KeyboardKey::Num2:  return "Num2";
        case Input::KeyboardKey::Num3:  return "Num3";
        case Input::KeyboardKey::Num4:  return "Num4";
        case Input::KeyboardKey::Num5:  return "Num5";
        case Input::KeyboardKey::Num6:  return "Num6";
        case Input::KeyboardKey::Num7:  return "Num7";
        case Input::KeyboardKey::Num8:  return "Num8";
        case Input::KeyboardKey::Num9:  return "Num9";
        case Input::KeyboardKey::NumAsterisk: return "Num*";
        case Input::KeyboardKey::NumSlash:    return "Num/";
        case Input::KeyboardKey::NumPlus:     return "Num+";
        case Input::KeyboardKey::NumMinus:    return "Num-";
        case Input::KeyboardKey::Up:    return "Up";
        case Input::KeyboardKey::Down:  return "Down";
        case Input::KeyboardKey::Left:  return "Left";
        case Input::KeyboardKey::Right: return "Right";
        default: return "?";
    }
}

static std::string KeysToStr(const std::vector<Input::KeyboardKey>& keys,
                              const std::function<std::string(Input::KeyboardKey)>& kn) {
    std::string result;
    for (size_t i = 0; i < keys.size(); i++) {
        if (i > 0) result += ",";
        result += kn(keys[i]);
    }
    return result;
}

bool Config::Save(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "[Config] Не вдалося зберегти " << path << "\n";
        return false;
    }

    auto kn = [this](Input::KeyboardKey k) { return KeyName(k); };

    auto scalarStr = [](const cv::Scalar& s) {
        return std::to_string((int)s[0]) + "," +
               std::to_string((int)s[1]) + "," +
               std::to_string((int)s[2]);
    };

    const char* logLevelStr[] = {"DEBUG", "INFO", "WARNING", "ERROR", "NONE"};
    int ll = std::max(0, std::min(4, log_level));

    f << "[General]\n";
    f << "WindowTitle = " << window_title << "\n";
    f << "Debug = " << (debug ? "true" : "false") << "\n";
    f << "LogLevel = " << logLevelStr[ll] << "\n";
    f << "\n";
    f << "[Character]\n";
    f << "# Mage, Archer, Spoiler, Treasure Hunter, ...\n";
    f << "Class = " << char_class << "\n";
    f << "\n";
    f << "[Targeting]\n";
    f << "# Клавіші макросів /target на хотбарі (через кому).\n";
    f << "MacroKeys = " << KeysToStr(target_macro_keys, kn) << "\n";
    f << "\n";
    f << "[Attack]\n";
    f << "AttackKeys = " << KeysToStr(attack_keys, kn) << "\n";
    f << "AttackWait = " << attack_wait << "\n";
    f << "AttackWatchdog = " << attack_watchdog << "\n";
    // AttackDelays (порожньо = використовувати AttackWait для всіх)
    f << "AttackDelays = ";
    for (size_t i = 0; i < attack_delays.size(); i++) {
        if (i > 0) f << ",";
        f << attack_delays[i];
    }
    f << "\n";
    f << "SpoilKey = " << KeyName(spoil_key) << "\n";
    f << "SweepKey = " << KeyName(sweep_key) << "\n";
    f << "\n";
    f << "[Loot]\n";
    f << "LootKey = " << KeyName(loot_key) << "\n";
    f << "LootCount = " << loot_count << "\n";
    f << "\n";
    f << "[Potions]\n";
    f << "HP_Key = " << KeyName(hp_key) << "\n";
    f << "HP_Threshold = " << hp_threshold << "\n";
    f << "MP_Key = " << KeyName(mp_key) << "\n";
    f << "MP_Threshold = " << mp_threshold << "\n";
    f << "CP_Key = " << KeyName(cp_key) << "\n";
    f << "CP_Threshold = " << cp_threshold << "\n";
    f << "\n";
    f << "[Buffs]\n";
    f << "# Клавіші self-buff (через кому, порожньо = вимкнено)\n";
    f << "BuffKeys = " << KeysToStr(buff_keys, kn) << "\n";
    f << "BuffInterval = " << buff_interval << "\n";
    f << "BuffUseAltB = " << (buff_use_altb ? "true" : "false") << "\n";
    f << "BuffPostCombatCooldown = " << buff_post_combat_cooldown << "\n";
    f << "BuffTabX = " << buff_tab_x << "\n";
    f << "BuffTabY = " << buff_tab_y << "\n";
    f << "BuffProfileX = " << buff_profile_x << "\n";
    f << "BuffProfileY = " << buff_profile_y << "\n";
    f << "\n";
    f << "[Special]\n";
    f << "# Клавіша дії на мертвий таргет (Pokemon тощо); порожньо = вимкнено\n";
    f << "PokemonKey = " << (has_pokemon_key ? KeyName(pokemon_key) : "") << "\n";
    f << "\n";
    f << "[Telegram]\n";
    f << "BotToken = " << tg_token << "\n";
    f << "ChatID = " << tg_chat_id << "\n";
    f << "NotifyOnDeath = " << (tg_on_death ? "true" : "false") << "\n";
    f << "StatsInterval = " << tg_stats_interval << "\n";
    f << "\n";
    f << "[Stats]\n";
    f << "# Авто-збереження stats кожні N kills (0 = вимкнено)\n";
    f << "AutoSaveInterval = " << auto_save_kills << "\n";
    f << "\n";
    f << "[Movement]\n";
    f << "# Дефолт: стрілки. W/S/A/D відкривають чат в L2!\n";
    f << "Forward     = " << KeyName(move_forward)  << "\n";
    f << "Back        = " << KeyName(move_back)     << "\n";
    f << "RotateLeft  = " << KeyName(rotate_left)   << "\n";
    f << "RotateRight = " << KeyName(rotate_right)  << "\n";
    f << "\n";
    f << "[Navigation]\n";
    f << "# Memory-based навігація. Вмикати тільки після калібровки heading.\n";
    f << "Enabled        = " << (navigation.enabled      ? "true" : "false") << "\n";
    f << "AttackRange    = " << navigation.attack_range    << "\n";
    f << "AngleTolerance = " << navigation.angle_tolerance << "\n";
    f << "UseHeading     = " << (navigation.use_heading   ? "true" : "false") << "\n";
    f << "\n";
    f << "[Delays]\n";
    f << "Enabled      = " << (delays.enabled ? "true" : "false") << "\n";
    f << "AttackMeanMs = " << delays.attack_mean_ms  << "\n";
    f << "AttackStdMs  = " << delays.attack_std_ms   << "\n";
    f << "RotateMeanMs = " << delays.rotate_mean_ms  << "\n";
    f << "RotateStdMs  = " << delays.rotate_std_ms   << "\n";
    f << "WalkMeanMs   = " << delays.walk_mean_ms    << "\n";
    f << "WalkStdMs    = " << delays.walk_std_ms     << "\n";
    f << "PotionMeanMs = " << delays.potion_mean_ms  << "\n";
    f << "PotionStdMs  = " << delays.potion_std_ms   << "\n";
    f << "\n";
    f << "[Zone]\n";
    f << "# Обмеження зони фарму. Enabled=false = без обмежень.\n";
    f << "# Потрібен [MemReader] Enabled=true + відкалібровані XYZ offsets.\n";
    f << "Enabled = " << (zone_enabled ? "true" : "false") << "\n";
    f << "CenterX = " << zone_x      << "\n";
    f << "CenterY = " << zone_y      << "\n";
    f << "Radius  = " << zone_radius  << "\n";
    f << "\n";
    f << "[Rest]\n";
    f << "# Пауза при низькому MP. 0 = вимкнено.\n";
    f << "MPThreshold = " << mp_threshold << "\n";
    f << "\n";
    f << "[Fuzzy]\n";
    f << "# Нечітке порівняння назв мобів (Levenshtein). false = вимкнено.\n";
    f << "# Threshold: 1.0 = точна відповідність, 0.85 = 15% різниці дозволено.\n";
    f << "Enabled   = " << (fuzzy.enabled ? "true" : "false") << "\n";
    f << "Threshold = " << fuzzy.threshold << "\n";
    f << "\n";
    f << "[WeightedTargeting]\n";
    f << "Enabled        = " << (weighted_target.enabled ? "true" : "false") << "\n";
    f << "WeightDistance = " << weighted_target.w_distance  << "\n";
    f << "WeightLowHP    = " << weighted_target.w_low_hp    << "\n";
    f << "WeightFreshness= " << weighted_target.w_freshness << "\n";
    f << "MaxRange       = " << weighted_target.max_range   << "\n";
    f << "\n";
    f << "\n[TargetingTuning]\n";
    f << "# Параметри логіки TARGETING. Дефолти перевірені в підземеллях.\n";
    f << "MinimapDxThreshold    = " << targeting_tuning.minimap_dx_threshold     << "\n";
    f << "MinimapRotateLimit    = " << targeting_tuning.minimap_rotate_limit     << "\n";
    f << "DeadCyclesMacroSwitch = " << targeting_tuning.dead_cycles_macro_switch << "\n";
    f << "MacroFallbackUnreach  = " << targeting_tuning.macro_fallback_unreach   << "\n";
    f << "LongSearchWarnAt      = " << targeting_tuning.long_search_warn_at      << "\n";
    f << "\n";
    f << "[Threading]\n";
    f << "# Мультипоточність. Enabled=false → все в одному потоці (як раніше).\n";
    f << "# VisionThread: DetectNPCs async на Core VisionCore.\n";
    f << "# GeodataThread: FindPath async на Core GeodataCore.\n";
    f << "# Рекомендовано: перевір htop перед увімкненням.\n";
    f << "Enabled       = " << (threading.enabled        ? "true":"false") << "\n";
    f << "CPUAffinity   = " << (threading.cpu_affinity   ? "true":"false") << "\n";
    f << "MainCore      = " << threading.main_core      << "\n";
    f << "VisionThread  = " << (threading.vision_thread  ? "true":"false") << "\n";
    f << "VisionCore    = " << threading.vision_core    << "\n";
    f << "GeodataThread = " << (threading.geodata_thread ? "true":"false") << "\n";
    f << "GeodataCore   = " << threading.geodata_core   << "\n";
    f << "\n";
    f << "[Vision]\n";
    f << "UseRobustBarDetection = " << (use_robust_bar ? "true" : "false") << "\n";
    f << "\n";
    f << "[Colors_MyBars]\n";
    f << "HPFromHSV = " << scalarStr(colors.my_hp_from_hsv) << "\n";
    f << "HPToHSV   = " << scalarStr(colors.my_hp_to_hsv)   << "\n";
    f << "MPFromHSV = " << scalarStr(colors.my_mp_from_hsv) << "\n";
    f << "MPToHSV   = " << scalarStr(colors.my_mp_to_hsv)   << "\n";
    f << "CPFromHSV = " << scalarStr(colors.my_cp_from_hsv) << "\n";
    f << "CPToHSV   = " << scalarStr(colors.my_cp_to_hsv)   << "\n";
    f << "\n";
    f << "[Colors_Target]\n";
    f << "HPFromHSV = " << scalarStr(colors.target_hp_from_hsv) << "\n";
    f << "HPToHSV   = " << scalarStr(colors.target_hp_to_hsv)   << "\n";
    f << "\n";
    f << "[Colors_TargetCircles]\n";
    f << "GrayBGR = " << scalarStr(colors.target_gray_circle_bgr) << "\n";
    f << "BlueBGR = " << scalarStr(colors.target_blue_circle_bgr) << "\n";
    f << "RedBGR  = " << scalarStr(colors.target_red_circle_bgr)  << "\n";
    f << "\n";
    f << "[Colors_NPC]\n";
    f << "NameFromHSV        = " << scalarStr(colors.npc_name_from_hsv) << "\n";
    f << "NameToHSV          = " << scalarStr(colors.npc_name_to_hsv)   << "\n";
    f << "NameColorThreshold = " << colors.npc_name_color_threshold      << "\n";

    return true;
}

bool Config::Validate() const {
    bool ok = true;

    if (target_macro_keys.empty()) {
        std::cerr << "[CONFIG] УВАГА: MacroKeys порожній — таргетинг не працюватиме!\n";
        ok = false;
    }
    if (attack_keys.empty()) {
        std::cerr << "[CONFIG] УВАГА: AttackKeys порожній — атака не працюватиме!\n";
        ok = false;
    }

    // Перевірка конфлікту між macro і attack клавішами
    for (auto& mk : target_macro_keys) {
        for (auto& ak : attack_keys) {
            if (mk == ak) {
                std::cerr << "[CONFIG] УВАГА: Конфлікт клавіш — MacroKeys і AttackKeys мають спільну клавішу!\n";
                ok = false;
            }
        }
    }

    if (hp_threshold > 95 || hp_threshold < 10)
        std::cerr << "[CONFIG] УВАГА: HP_Threshold=" << hp_threshold << " — підозріле значення\n";

    if (!attack_delays.empty() && attack_delays.size() != attack_keys.size())
        std::cerr << "[CONFIG] УВАГА: AttackDelays має " << attack_delays.size()
                  << " елементів, AttackKeys має " << attack_keys.size()
                  << " — буде ротація\n";

    return ok;
}

// ANSI кольори для TUI
#define CYAN    "\033[1;36m"
#define YELLOW  "\033[1;33m"
#define GREEN   "\033[1;32m"
#define WHITE   "\033[1;37m"
#define RESET   "\033[0m"

static std::string ReadLine(const std::string& prompt, const std::string& current) {
    std::cout << YELLOW "  " << prompt << " (" << current << "): " RESET;
    std::string line;
    std::getline(std::cin, line);
    line = Trim(line);
    return line.empty() ? current : line;
}

bool Config::InteractiveSetup() {
    std::cout << "\n";
    std::cout << CYAN "╔══════════════════════════════════════════╗\n";
    std::cout <<       "║       rdga1bot — ElmoreLab Farm Bot      ║\n";
    std::cout <<       "╚══════════════════════════════════════════╝\n" RESET;
    std::cout << "\n";

    // Клас персонажа
    {
        std::string v = ReadLine("Клас [Mage/Archer/Spoiler/Treasure Hunter/...]", char_class);
        if (!v.empty()) char_class = v;
    }

    // Клавіші макросів /target
    {
        auto kn = [this](Input::KeyboardKey k) { return KeyName(k); };
        std::string cur = KeysToStr(target_macro_keys, kn);
        if (cur.empty()) cur = "порожньо";
        std::string v = ReadLine("Клавіші /target макросів", cur);
        if (v != cur && v != "порожньо") {
            auto mk = [&]() -> std::vector<Input::KeyboardKey> {
                std::vector<Input::KeyboardKey> res;
                std::istringstream ss(v);
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    tok = Trim(tok);
                    if (!tok.empty())
                        res.push_back(StringToKeyboardKey(tok, Input::KeyboardKey::F1));
                }
                return res;
            }();
            target_macro_keys = mk;
        }
    }

    // Клавіші атаки
    {
        auto kn = [this](Input::KeyboardKey k) { return KeyName(k); };
        std::string cur = KeysToStr(attack_keys, kn);
        if (cur.empty()) cur = "F1";
        std::string v = ReadLine("Клавіші атаки", cur);
        if (v != cur) {
            auto ak = [&]() -> std::vector<Input::KeyboardKey> {
                std::vector<Input::KeyboardKey> res;
                std::istringstream ss(v);
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    tok = Trim(tok);
                    if (!tok.empty())
                        res.push_back(StringToKeyboardKey(tok, Input::KeyboardKey::F1));
                }
                return res;
            }();
            attack_keys = ak;
        }
    }

    // Затримка між атаками
    {
        std::string v = ReadLine("Затримка між атаками", std::to_string(attack_wait).substr(0, 4));
        try { attack_wait = std::stod(v); } catch (...) {}
    }

    // Клавіша лута
    {
        std::string v = ReadLine("Клавіша лута", KeyName(loot_key));
        loot_key = StringToKeyboardKey(v, loot_key);
    }

    // Кількість підборів
    {
        std::string v = ReadLine("Кількість підборів", std::to_string(loot_count));
        try { loot_count = std::stoi(v); } catch (...) {}
    }

    // HP
    {
        std::string v = ReadLine("HP клавіша", KeyName(hp_key));
        hp_key = StringToKeyboardKey(v, hp_key);
    }
    {
        std::string v = ReadLine("HP поріг %", std::to_string(hp_threshold));
        try { hp_threshold = std::stoi(v); } catch (...) {}
    }

    // MP
    {
        std::string v = ReadLine("MP клавіша", KeyName(mp_key));
        mp_key = StringToKeyboardKey(v, mp_key);
    }
    {
        std::string v = ReadLine("MP поріг %", std::to_string(mp_threshold));
        try { mp_threshold = std::stoi(v); } catch (...) {}
    }

    // Buff
    {
        auto kn = [this](Input::KeyboardKey k) { return KeyName(k); };
        std::string cur = KeysToStr(buff_keys, kn);
        if (cur.empty()) cur = "порожньо";
        std::string v = ReadLine("Buff клавіші (порожньо = вимкнено)", cur);
        if (v != "порожньо" && v != cur) {
            auto bk = [&]() -> std::vector<Input::KeyboardKey> {
                std::vector<Input::KeyboardKey> res;
                std::istringstream ss(v);
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    tok = Trim(tok);
                    if (!tok.empty())
                        res.push_back(StringToKeyboardKey(tok, Input::KeyboardKey::F1));
                }
                return res;
            }();
            buff_keys = bk;
        } else if (v == "порожньо") {
            buff_keys.clear();
        }
    }
    {
        std::string v = ReadLine("Buff інтервал секунд", std::to_string(buff_interval));
        try { buff_interval = std::stoi(v); } catch (...) {}
    }

    // Telegram
    {
        std::string cur = tg_token.empty() ? "порожньо" : tg_token;
        std::string v = ReadLine("Telegram токен", cur);
        if (v != "порожньо") tg_token = v;
        else tg_token = "";
    }
    {
        std::string cur = tg_chat_id.empty() ? "порожньо" : tg_chat_id;
        std::string v = ReadLine("Telegram chat_id", cur);
        if (v != "порожньо") tg_chat_id = v;
        else tg_chat_id = "";
    }

    std::cout << "\n";
    std::cout << GREEN "Зберегти і стартувати? (y/n): " RESET;
    std::string answer;
    std::getline(std::cin, answer);
    answer = Trim(answer);
    std::transform(answer.begin(), answer.end(), answer.begin(), ::tolower);

    return answer == "y" || answer == "yes" || answer == "т" || answer.empty();
}

#pragma once

#include <vector>
#include <optional>
#include <array>

#include <opencv2/opencv.hpp>
#include "Config.h"

class Eyes
{
public:
    struct MyBars { cv::Rect hp_bar, mp_bar, cp_bar; };
    struct Me { int hp, mp, cp; };
    struct Target { int hp; };

    struct TrackableNPC {
        cv::Point center;
        std::uint32_t tracking_id;
        virtual std::uint32_t Id() const = 0;
    };

    struct NPC : TrackableNPC
    {
        enum class State { Default = 0, Hovered = 1, Selected = 2 };

        State state;
        std::uint32_t name_id;
        cv::Rect rect;

        std::uint32_t Id() const override   { return tracking_id > 0 ? tracking_id : CenterId(); }
        std::uint32_t CenterId() const      { return center.x << 16 | center.y; }
        bool Selected() const               { return state == State::Selected; }
        bool Hovered() const                { return state == State::Hovered; }
    };

    struct FarNPC : TrackableNPC
    {
        cv::Rect rect;
        std::uint32_t Id() const override { return tracking_id; }
    };

    // ── Мінімапа ─────────────────────────────────────────────────────────────
    struct MinimapDot {
        int   dx;     // відносно центру мінімапи: >0 праворуч, <0 ліворуч
        int   dy;     // >0 позаду, <0 спереду (Rotating Radar: гравець дивиться ВГОРУ)
        float dist;   // пікселів від центру
    };

    // ROI від правого краю вікна (window-relative)
    int m_minimap_roi_from_right = 185; // ширина зони пошуку від правого краю
    int m_minimap_roi_height     = 165; // висота зони
    // Центр кола в межах ROI (калібровано по calibrate.png 1366×768)
    int m_minimap_cx_in_roi = 95;
    int m_minimap_cy_in_roi = 89;
    int m_minimap_radius    = 78;  // радіус кола мінімапи в пікселях
    // Фільтри точок
    int   m_minimap_player_excl_r = 15;  // ігнорувати точки ближче N px до центру (гравець)
    float m_minimap_dot_area_min  = 4.0f;// мін площа контуру (шум < 4)

    bool m_use_robust_bar = true; // медіана 3 рядків замість середнього

    int m_blind_spot_radius     = 100;
    int m_npc_tracking_distance = 30;

    // NPC detection
    int m_npc_name_min_height               = 8;
    int m_npc_name_max_height               = 16;
    int m_npc_name_min_width                = 20;
    int m_npc_name_max_width                = 250;
    cv::Scalar m_npc_name_color_from_hsv    = {0, 0, 240};
    cv::Scalar m_npc_name_color_to_hsv      = {0, 0, 255};
    double m_npc_name_color_threshold       = 0.2;
    int m_npc_name_center_offset            = 17;

    // far NPC detection
    int m_far_npc_min_height    = 25;
    int m_far_npc_max_height    = 200;
    int m_far_npc_min_width     = 25;
    int m_far_npc_max_width     = 200;
    int m_far_npc_limit         = 10;

    // selected target detection
    int m_target_circle_area_height             = 25;
    int m_target_circle_area_width              = 25;
    cv::Scalar m_target_gray_circle_color_bgr   = {57, 60, 66, 255};
    cv::Scalar m_target_blue_circle_color_bgr   = {107, 48, 0, 255};
    cv::Scalar m_target_red_circle_color_bgr    = {0, 4, 132, 255};

    // my HP/MP/CP bars detection
    // ElmoreLab HP bar is ~10px tall; erosion kernel = min_height, so must be < bar height
    int m_my_bar_min_height             = 7;
    int m_my_bar_max_height             = 20;
    int m_my_bar_min_width              = 30;
    int m_my_bar_max_width              = 400;
    // Зона пошуку барів (StatusWnd ElmoreLab: posX=0 posY=0 width=179 height=80)
    // Обмежуємо пошук тільки до цієї зони — ігноруємо фон екрану
    int m_my_bar_search_x               = 0;
    int m_my_bar_search_y               = 0;
    int m_my_bar_search_w               = 180; // StatusWnd width=179 +1px
    int m_my_bar_search_h               = 80;  // точно StatusWnd height=80 (запас 100 захоплював пустельний фон!)
    // HSV defaults calibrated for ElmoreLab (ElmoreLab 2026-03-22)
    cv::Scalar m_my_hp_color_from_hsv   = {0, 75, 55};
    cv::Scalar m_my_hp_color_to_hsv     = {10, 255, 175};
    cv::Scalar m_my_mp_color_from_hsv   = {88, 30, 35};
    cv::Scalar m_my_mp_color_to_hsv     = {122, 255, 140};
    cv::Scalar m_my_cp_color_from_hsv   = {12, 80, 50};
    cv::Scalar m_my_cp_color_to_hsv     = {27, 255, 200};

    // target HP bar detection
    int m_target_hp_min_height              = 3;
    int m_target_hp_max_height              = 7;
    int m_target_hp_min_width               = m_my_bar_min_width;
    int m_target_hp_max_width               = m_my_bar_max_width;
    cv::Scalar m_target_hp_color_from_hsv   = {0, 100, 90};
    cv::Scalar m_target_hp_color_to_hsv     = {12, 220, 160};

    Eyes() :
        m_hsv_frames{},
        m_frame     {0},
        m_diffs     {}
    {}

    const std::optional<cv::Rect> &TargetHPBar() const { return m_target_hp_bar; }
    const std::optional<struct MyBars> &MyBars() const { return m_my_bars; }

    // Застосувати кольори з Config (після зміни налаштувань)
    void SetColors(const ColorConfig& c);

    // Встановити позицію TargetStatusWnd (з WindowsInfo.ini)
    void SetTargetWnd(int x, int y, int w, int h) {
        m_target_wnd_x = x; m_target_wnd_y = y;
        m_target_wnd_w = w; m_target_wnd_h = h;
    }

    // Авто-калібрування: -1 = нема змін; >=0 = нове x (для логування з Brain)
    int  GetAutoCalX() const    { return m_target_wnd_autocal_x; }
    int  GetTargetWndX() const  { return m_target_wnd_x; }
    void ClearAutoCalX()        { m_target_wnd_autocal_x = -1; }

    // Перевірка: чи не обрив/стіна попереду (за яскравістю нижньої зони)
    bool IsGroundAhead() const;

    void Open(const cv::Mat &bgr);
    // Fix #3: clone усунено — DetectFarNPCs() не використовується
    void Close()    { m_frame++; }
    void Reset()         { m_my_bars = {}; m_target_hp_bar = {}; }
    void ResetTarget()   { m_target_hp_bar = {}; }

    std::vector<NPC> DetectNPCs();
    std::vector<FarNPC> DetectFarNPCs();
    std::optional<Me> DetectMe();
    std::optional<Target> DetectTarget();

    // Мінімапа: список червоних точок (моби) відносно центру
    // Rotating Radar: dy<0=спереду, dy>0=позаду, dx>0=право, dx<0=ліво
    std::vector<MinimapDot> DetectMinimap() const;

    // Зберегти поточний кадр у файл (для налагодження)
    void SaveFrame(const std::string& path) const {
        if (!m_bgr.empty()) cv::imwrite(path, m_bgr);
    }

    // Template matching: шукає templ в поточному BGR кадрі.
    // Повертає центр знайденого прямокутника, або {} якщо maxVal < threshold.
    // out_score — якщо не nullptr, записує реальне значення maxVal (0..1).
    std::optional<cv::Point> FindTemplate(const cv::Mat& templ, float threshold = 0.8f,
                                          float* out_score = nullptr) const;

private:
    cv::Mat m_bgr;
    cv::Mat m_hsv;
    std::array<cv::Mat, 5> m_hsv_frames; // previous frames buffer
    std::array<cv::Mat, 5>::size_type m_frame; // current frame index

    std::optional<struct MyBars> m_my_bars;
    std::optional<cv::Rect> m_target_hp_bar;
    std::array<cv::Mat, 15> m_diffs; // used for far NPCs detection
    std::vector<NPC> m_npcs; // previously detected NPCs
    std::vector<FarNPC> m_far_npcs; // previously detected far NPCs

    std::optional<struct MyBars> DetectMyBars() const;
    std::optional<cv::Rect> DetectTargetHPBar() const;
    bool HasTargetName() const; // checks for text in target window (kill detection)
    int  DetectTargetHPDirect() const; // reads HP% from fixed position, -1 = no bar
    std::vector<std::vector<cv::Point>> FindMyBarContours(const cv::Mat &mask) const;
    NPC::State DetectNPCState(const cv::Rect &rect) const;

    // Позиція TargetStatusWnd (динамічна, читається з WindowsInfo.ini)
    // mutable: auto-калібрується в DetectTargetHPDirect() коли бар знайдено в іншому місці
    mutable int m_target_wnd_x = 598;
    int m_target_wnd_y = 0;
    int m_target_wnd_w = 179;
    int m_target_wnd_h = 46;
    // Прапор авто-калібрування: встановлюється коли m_target_wnd_x змінився
    mutable int m_target_wnd_autocal_x = -1; // -1 = нема; >=0 = нове значення (для логування)

    // Opt: Lazy HSV — конвертуємо повний кадр тільки при першому виклику за тік
    bool m_hsv_full_ready = false;
    void EnsureFullHSV();                        // заповнює m_hsv якщо ще порожній
    cv::Mat HSVForROI(const cv::Rect& roi) const; // повертає HSV sub-mat або локальну конверсію

    template<typename T>
    void CalculateTrackingIds(std::vector<T> &npcs) const;

    static bool IsRectInImage(const cv::Mat &image, const cv::Rect &rect)
        { return (rect & cv::Rect{0, 0, image.cols, image.rows}) == rect; }

    static int CalcBarPercentValue(
        const cv::Mat &bar,
        const cv::Scalar &from_color,
        const cv::Scalar &to_color,
        bool whole_bar = false
    );

    // Робастна версія: медіана по 3 рядках (25%/50%/75% висоти)
    static int CalcBarPercentValueRobust(
        const cv::Mat &bar,
        const cv::Scalar &from_color,
        const cv::Scalar &to_color,
        bool whole_bar = false
    );

    static std::uint32_t Hash(const cv::Mat &image);
};

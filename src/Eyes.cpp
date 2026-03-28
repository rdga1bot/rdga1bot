#include "Eyes.h"

#include <algorithm>
#include <limits>

void Eyes::SetColors(const ColorConfig& c) {
    m_npc_name_color_from_hsv    = c.npc_name_from_hsv;
    m_npc_name_color_to_hsv      = c.npc_name_to_hsv;
    m_npc_name_color_threshold   = c.npc_name_color_threshold;
    m_my_hp_color_from_hsv       = c.my_hp_from_hsv;
    m_my_hp_color_to_hsv         = c.my_hp_to_hsv;
    m_my_mp_color_from_hsv       = c.my_mp_from_hsv;
    m_my_mp_color_to_hsv         = c.my_mp_to_hsv;
    m_my_cp_color_from_hsv       = c.my_cp_from_hsv;
    m_my_cp_color_to_hsv         = c.my_cp_to_hsv;
    m_target_hp_color_from_hsv   = c.target_hp_from_hsv;
    m_target_hp_color_to_hsv     = c.target_hp_to_hsv;
    m_target_gray_circle_color_bgr = c.target_gray_circle_bgr;
    m_target_blue_circle_color_bgr = c.target_blue_circle_bgr;
    m_target_red_circle_color_bgr  = c.target_red_circle_bgr;
    // Скидаємо кешовані бари — потрібно детектувати з новими кольорами
    Reset();
}

void Eyes::Open(const cv::Mat &bgr)
{
    // Fix #2: shallow ref-counted copy замість clone() — економимо ~6MB copy/тік
    m_bgr = bgr;
    // Opt: lazy HSV — конвертуємо повний кадр тільки коли справді потрібно
    m_hsv_full_ready = false;
    // cvtColor і blind spot перенесено в EnsureFullHSV() та DetectNPCs()
}

void Eyes::EnsureFullHSV()
{
    if (m_hsv_full_ready) return;
    cv::cvtColor(m_bgr, m_hsv, cv::COLOR_BGR2HSV);
    m_hsv_full_ready = true;
}

cv::Mat Eyes::HSVForROI(const cv::Rect& roi) const
{
    // Якщо повний HSV вже є — sub-mat без копіювання
    if (m_hsv_full_ready) return m_hsv(roi);
    // Інакше — конвертуємо тільки маленький ROI
    cv::Mat hsv_roi;
    cv::cvtColor(m_bgr(roi), hsv_roi, cv::COLOR_BGR2HSV);
    return hsv_roi;
}

std::vector<Eyes::NPC> Eyes::DetectNPCs()
{
    // Opt: повний HSV потрібен тут — конвертуємо якщо ще не зроблено
    EnsureFullHSV();

    // extract regions with white NPC names
    cv::Mat white;
    cv::inRange(m_hsv, m_npc_name_color_from_hsv, m_npc_name_color_to_hsv, white);

    // increase white regions size
    cv::Mat mask;
    auto kernel = cv::getStructuringElement(cv::MORPH_RECT, {3, 3});
    cv::dilate(white, mask, kernel);

    // join words
    kernel = cv::getStructuringElement(cv::MORPH_RECT, {17, 5});
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);

    // remove noise
    kernel = cv::getStructuringElement(cv::MORPH_RECT, {11, 5});
    cv::erode(mask, mask, kernel);
    cv::dilate(mask, mask, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    std::vector<NPC> npcs;

    const int bcx = m_hsv.cols / 2, bcy = m_hsv.rows / 2;
    const int blind_r2 = m_blind_spot_radius * m_blind_spot_radius;

    for (const auto &contour : contours) {
        const auto rect = cv::boundingRect(contour);

        if (rect.height < m_npc_name_min_height || rect.height > m_npc_name_max_height ||
            rect.width < m_npc_name_min_width || rect.width > m_npc_name_max_width ||
            rect.width < rect.height * 2
        ) {
            continue;
        }

        // Blind spot: пропускаємо NPC в центрі екрану (гравець) — без деструктивного cv::circle
        const cv::Point npc_center = {rect.x + rect.width / 2, rect.y + rect.height / 2};
        const int bdx = npc_center.x - bcx, bdy = npc_center.y - bcy;
        if (bdx * bdx + bdy * bdy < blind_r2) continue;

        const auto target_image = white(rect);
        const auto threshold = cv::countNonZero(target_image) / target_image.total();

        if (threshold > m_npc_name_color_threshold) {
            continue;
        }

        NPC npc = {};
        npc.rect = rect;
        npc.center = {rect.x + rect.width / 2, rect.y + rect.height / 2 + m_npc_name_center_offset};
        npc.name_id = Hash(target_image);
        npc.state = DetectNPCState(rect);
        npcs.push_back(npc);
    }
    
    CalculateTrackingIds(npcs);

    m_npcs = npcs;
    return npcs;
}

std::vector<Eyes::FarNPC> Eyes::DetectFarNPCs()
{
    if (m_far_npc_limit <= 0) {
        return {};
    }
    EnsureFullHSV(); // DetectFarNPCs потребує повний m_hsv для diff
    
    // diff current frame with 3 previous frames
    cv::Mat diff_sum;

    for (int k = 1; k <= 3; k++) {
        if (m_frame < (size_t)k) continue;
        const auto frame = m_hsv_frames[(m_frame - k) % m_hsv_frames.size()];

        if (frame.empty()) {
            continue;
        }

        cv::Mat diff;
        cv::absdiff(frame, m_hsv, diff);

        if (diff_sum.empty()) {
            diff_sum = diff;
        } else {
            cv::bitwise_or(diff, diff_sum, diff_sum);
        }
    }

    // expand diff areas
    if (!diff_sum.empty()) {
        cv::cvtColor(diff_sum, diff_sum, cv::COLOR_BGR2GRAY);
        cv::threshold(diff_sum, diff_sum, 5, 255, cv::THRESH_BINARY);
        const auto kernel = cv::getStructuringElement(cv::MORPH_RECT, {21, 21});
        cv::dilate(diff_sum, diff_sum, kernel);
    }

    // multiply all diffs
    cv::Mat mask;

    for (const auto &diff : m_diffs) {
        if (diff.empty()) {
            continue;
        }

        if (mask.empty()) {
            mask = diff;
        } else {
            cv::bitwise_and(diff, mask, mask);
        }
    }

    m_diffs[m_frame % m_diffs.size()] = diff_sum;

    if (mask.empty()) {
        return {};
    }

    // remove noise
    const auto kernel = cv::getStructuringElement(cv::MORPH_RECT, {15, 15});
    cv::erode(mask, mask, kernel);
    cv::dilate(mask, mask, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    std::vector<FarNPC> npcs;

    for (const auto &contour : contours) {
        const auto rect = cv::boundingRect(contour);

        if (rect.y + rect.height > m_hsv.rows / 2 ||
            rect.height < m_far_npc_min_height || rect.height > m_far_npc_max_height ||
            rect.width < m_far_npc_min_width || rect.width > m_far_npc_max_width ||
            rect.width * 2 < rect.height || rect.height * 2 < rect.width
        ) {
            continue;
        }

        FarNPC npc = {};
        npc.rect = rect;
        npc.center = {npc.rect.x + npc.rect.width / 2, npc.rect.y + npc.rect.height / 2};
        npcs.push_back(npc);
    }

    // return only nearest NPCs
    std::sort(npcs.begin(), npcs.end(), [this](const FarNPC &a, const FarNPC &b) {
        return (std::min)(a.rect.width, a.rect.height) > (std::min)(b.rect.width, b.rect.height);
    });

    if (npcs.size() > m_far_npc_limit) {
        npcs.erase(npcs.begin() + m_far_npc_limit, npcs.end());
    }

    CalculateTrackingIds(npcs);

    m_far_npcs = npcs;
    return npcs;
}

std::optional<Eyes::Me> Eyes::DetectMe()
{
    if (!m_my_bars.has_value()) {
        // DetectMyBars потребує повний m_hsv — конвертуємо один раз при першому пошуку
        EnsureFullHSV();
        m_my_bars = DetectMyBars();
    }

    if (!m_my_bars.has_value()) {
        return {};
    }

    // Opt: бари знайдені й закешовані — конвертуємо тільки маленький ROI кожного бару.
    // Scan rightmost pixel: фон бару (S≈153, V≈60) відфільтровується V_min=70 в INI.
    // Білий текст (S=0) теж не матчить → знаходимо реальний правий край fill.
    auto calcBar = [&](const cv::Rect& bar_rect, const cv::Scalar& from, const cv::Scalar& to) {
        cv::Mat hsv_bar = HSVForROI(bar_rect);
        return m_use_robust_bar
            ? CalcBarPercentValueRobust(hsv_bar, from, to, false)
            : CalcBarPercentValue(hsv_bar, from, to, false);
    };

    Me me = {};
    me.hp = calcBar(m_my_bars->hp_bar, m_my_hp_color_from_hsv, m_my_hp_color_to_hsv);
    me.mp = calcBar(m_my_bars->mp_bar, m_my_mp_color_from_hsv, m_my_mp_color_to_hsv);
    me.cp = calcBar(m_my_bars->cp_bar, m_my_cp_color_from_hsv, m_my_cp_color_to_hsv);
    return me;
}

int Eyes::DetectTargetHPDirect() const {
    // TargetStatusWnd: posX=598, posY=0, width=179, height=46 (WindowsInfo.ini)
    // Calibration (2026-03-22):
    //   y=25,31 — bar frame (H=10, S=135, V=49), always present — IGNORE
    //   y=28-30 — HP fill: H=1, S=210-211, V=111-171 when alive
    //             background: H=1, S=118, V=127 when empty (dead/no target)
    // Key: HP fill S≈210 >> background S≈118. Use S≥190 to separate.
    // TargetStatusWnd: posX=598, posY=0, width=179, height=46 (WindowsInfo.ini)
    // Fill rows y=27..32 only (skip frame rows y=25,31 which have S=135 always).
    // BAR_WIDTH_100 = 152: measured fill pixel count at 100% HP (2026-03-22).
    // Using ROI width as denominator gives wrong % because bar doesn't fill full ROI.
    static constexpr int BAR_WIDTH_100 = 152;
    const int x1 = m_target_wnd_x, y1 = m_target_wnd_y + 27;
    const int x2 = m_target_wnd_x + m_target_wnd_w, y2 = m_target_wnd_y + 32;
    if (m_bgr.empty() || m_bgr.cols <= x2 || m_bgr.rows <= y2) return -1;

    // Opt: конвертуємо тільки 179×5 пікселів TargetStatusWnd (не весь кадр)
    cv::Mat roi_hsv;
    cv::cvtColor(m_bgr(cv::Rect(x1, y1, x2 - x1, y2 - y1)), roi_hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;
    // HP fill: H=0-10, S≥190, V=30-200. Background S≈118 is rejected.
    // V мін знижено 80→30: темно-червоний бар при низькому HP має V=47-78 (вимірювання 2026-03-22).
    cv::inRange(roi_hsv, cv::Scalar(0, 190, 30), cv::Scalar(10, 255, 200), mask);

    int best_count = 0;
    for (int r = 0; r < mask.rows; r++) {
        int cnt = cv::countNonZero(mask.row(r));
        if (cnt > best_count) best_count = cnt;
    }

    if (best_count > 0) {
        int hp = best_count * 100 / BAR_WIDTH_100;
        if (hp > 100) hp = 100;
        if (hp < 1)   hp = 1;
        return hp;
    }

    // fill=0 в поточній позиції: або моб мертвий, або TargetStatusWnd перемістилось.
    // Скануємо всю ширину екрану на тих же рядках y=27..32 — якщо бар існує в іншому місці,
    // авто-калібруємо m_target_wnd_x.
    // Ця ширина ~5 рядків × 1366px = ~6800px — дуже швидко.
    const int scan_w = m_bgr.cols;
    if (scan_w <= 0 || y2 > m_bgr.rows) return 0;
    cv::Mat wide_roi_hsv;
    cv::cvtColor(m_bgr(cv::Rect(0, y1, scan_w, y2 - y1)), wide_roi_hsv, cv::COLOR_BGR2HSV);
    cv::Mat wide_mask;
    cv::inRange(wide_roi_hsv, cv::Scalar(0, 190, 30), cv::Scalar(10, 255, 200), wide_mask);

    int scan_best = 0, scan_best_row = -1;
    for (int r = 0; r < wide_mask.rows; r++) {
        int cnt = cv::countNonZero(wide_mask.row(r));
        if (cnt > scan_best) { scan_best = cnt; scan_best_row = r; }
    }

    // scan_best >= 8 → ≥5% HP (справжній бар, не шум/іконки)
    // Обмеження: new_x має бути в межах ±150px від базової позиції WindowsInfo.ini
    // — запобігає хибним спрацюванням від UI елементів (buff icons, інші панелі)
    if (scan_best >= 8) {
        // Бар знайдено — знаходимо лівий край щоб оновити m_target_wnd_x
        const uchar* ptr = wide_mask.ptr(scan_best_row);
        int new_x = x1; // fallback
        for (int c = 0; c < wide_mask.cols; c++) {
            if (ptr[c]) { new_x = c; break; }
        }
        // Санітарна перевірка: new_x в межах ±150px від WindowsInfo.ini значення
        if (std::abs(new_x - m_target_wnd_x_base) <= 150) {
            if (std::abs(new_x - m_target_wnd_x) > 5) {
                m_target_wnd_autocal_x = m_target_wnd_x; // старе x — для логу "old → new"
                m_target_wnd_x = new_x;
            }
            int hp = scan_best * 100 / BAR_WIDTH_100;
            if (hp > 100) hp = 100;
            if (hp < 1)   hp = 1;
            return hp;
        }
    }

    return 0; // бар не знайдено ніде → моб дійсно мертвий
}

bool Eyes::HasTargetName() const {
    // TargetStatusWnd: posX=598, posY=0, width=179, height=46 (WindowsInfo.ini)
    // Scan the name text area (top ~20px of target window) for non-background pixels.
    // Background = fire/lava texture with S≈118-122 (highly saturated).
    // NPC name text = white/silver with S≤50, V≥130.
    const int x1 = m_target_wnd_x, y1 = m_target_wnd_y + 2;
    const int x2 = m_target_wnd_x + m_target_wnd_w - 2, y2 = m_target_wnd_y + 22;
    if (m_bgr.empty() || m_bgr.cols <= x2 || m_bgr.rows <= y2) return false;
    // Opt: конвертуємо тільки 176×20 пікселів TargetStatusWnd (не весь кадр)
    cv::Mat roi_hsv;
    cv::cvtColor(m_bgr(cv::Rect(x1, y1, x2 - x1, y2 - y1)), roi_hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;
    cv::inRange(roi_hsv, cv::Scalar(0, 0, 130), cv::Scalar(180, 50, 255), mask);
    return cv::countNonZero(mask) >= 5;
}

std::optional<Eyes::Target> Eyes::DetectTarget()
{
    // 1. Check if target window has name text (distinguishes "no target" from "target selected")
    if (!HasTargetName()) {
        m_target_hp_bar = {};
        return {};
    }

    // 2. Read HP directly from fixed TargetStatusWnd position (WindowsInfo.ini)
    //    Dark red bar V=30-85 vs background V=127 — no overlap, no false positives.
    int hp_direct = DetectTargetHPDirect();
    if (hp_direct >= 0) {
        // hp_direct=0  → bar found but empty (HP=0, mob dead)
        // hp_direct>0  → bar found, mob alive with approximate HP%
        Target target = {};
        target.hp = hp_direct;
        return target;
    }

    // 3. Fallback: bar not found at fixed position (UI layout differs?)
    //    Use cached scan-based detection (потребує повний HSV)
    if (!m_target_hp_bar.has_value()) {
        EnsureFullHSV();
        m_target_hp_bar = DetectTargetHPBar();
    }
    if (!m_target_hp_bar.has_value()) {
        Target target = {};
        target.hp = 50; // name visible, can't read HP → assume alive
        return target;
    }

    Target target = {};
    const cv::Mat bar = m_hsv(m_target_hp_bar.value());
    target.hp = m_use_robust_bar
        ? CalcBarPercentValueRobust(bar, m_target_hp_color_from_hsv, m_target_hp_color_to_hsv, true)
        : CalcBarPercentValue(bar, m_target_hp_color_from_hsv, m_target_hp_color_to_hsv, true);
    return target;
}

std::optional<struct Eyes::MyBars> Eyes::DetectMyBars() const
{
    // Обмежуємо пошук зоною StatusWnd — ігноруємо фон екрану
    const int sx = std::max(0, m_my_bar_search_x);
    const int sy = std::max(0, m_my_bar_search_y);
    const int sw = std::min(m_my_bar_search_w, m_hsv.cols - sx);
    const int sh = std::min(m_my_bar_search_h, m_hsv.rows - sy);
    if (sw <= 0 || sh <= 0) return {};
    const cv::Rect search_rect{sx, sy, sw, sh};
    const cv::Mat search_hsv = m_hsv(search_rect);

    // extract red regions with red HP bar (тільки в StatusWnd)
    cv::Mat mask;
    cv::inRange(search_hsv, m_my_hp_color_from_hsv, m_my_hp_color_to_hsv, mask);

    const auto contours = FindMyBarContours(mask);

    // search for CP bar above and MP bar below
    for (const auto &contour : contours) {
        // rect координати відносні до search_rect — конвертуємо в абсолютні
        const auto filled_rect = cv::boundingRect(contour) + search_rect.tl();

        if (filled_rect.height < m_my_bar_min_height || filled_rect.height > m_my_bar_max_height ||
            filled_rect.width < m_my_bar_min_width || filled_rect.width > m_my_bar_max_width
        ) {
            continue;
        }

        // Розширюємо HP rect до правого краю search_area — потрібно для CalcBarPercentValue
        // (функція скануює весь рядок від правого краю, порожня частина = background)
        const int full_bar_right = sx + sw;
        const cv::Rect rect{filled_rect.x, filled_rect.y,
                            full_bar_right - filled_rect.x, filled_rect.height};

        // Шукаємо CP і MP в Y-зонах відносно HP бару (окремо, щоб уникнути злиття)
        // CP — вище HP (приблизно -2..-10 рядків від HP top)
        // MP — нижче HP (приблизно +3..+20 рядків від HP bottom)
        const int hp_y = rect.y;
        const int hp_bottom = rect.y + rect.height;
        const int hp_x = rect.x;
        const int hp_w = rect.width;

        // CP zone: від hp_y-15 до hp_y-1
        const int cp_zone_top    = std::max(0, hp_y - 15);
        const int cp_zone_bottom = std::max(0, hp_y - 1);
        // MP zone: від hp_bottom+1 до hp_bottom+20
        const int mp_zone_top    = std::min(m_hsv.rows - 1, hp_bottom + 1);
        const int mp_zone_bottom = std::min(m_hsv.rows - 1, hp_bottom + 20);

        if (cp_zone_bottom <= cp_zone_top || mp_zone_bottom <= mp_zone_top) continue;

        const cv::Rect cp_zone_rect{hp_x, cp_zone_top, hp_w, cp_zone_bottom - cp_zone_top};
        const cv::Rect mp_zone_rect{hp_x, mp_zone_top, hp_w, mp_zone_bottom - mp_zone_top};

        if (!IsRectInImage(m_hsv, cp_zone_rect) || !IsRectInImage(m_hsv, mp_zone_rect)) continue;

        cv::Mat cp_mask, mp_mask;
        cv::inRange(m_hsv(cp_zone_rect), m_my_cp_color_from_hsv, m_my_cp_color_to_hsv, cp_mask);
        cv::inRange(m_hsv(mp_zone_rect), m_my_mp_color_from_hsv, m_my_mp_color_to_hsv, mp_mask);

        const int cp_px = cv::countNonZero(cp_mask);
        const int mp_px = cv::countNonZero(mp_mask);

        // Мінімальний поріг: bar повинен мати хоч 10% заповнення в зоні
        const int min_px = hp_w * (cp_zone_bottom - cp_zone_top) / 10;
        if (cp_px < min_px || mp_px < min_px) continue;

        // Знаходимо фактичний Y-діапазон CP і MP барів всередині зон.
        // Зберігаємо точний rect бару (не всю зону) — бо CalcBarPercentValueRobust
        // сэмплює рядки 25/50/75% зони і промахується повз ~8px бар у 20px зоні.
        auto findBarRect = [&](const cv::Mat& mask, const cv::Rect& zone_rect) -> cv::Rect {
            std::vector<std::vector<cv::Point>> cc;
            cv::Mat mask_copy = mask.clone();
            cv::findContours(mask_copy, cc, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
            if (!cc.empty()) {
                // Найбільший контур = сам бар
                auto best = cc[0];
                for (const auto& c : cc)
                    if (cv::contourArea(c) > cv::contourArea(best)) best = c;
                cv::Rect cr = cv::boundingRect(best);
                // Конвертуємо в абсолютні coords та розширюємо до full bar width
                cr.x += zone_rect.x;
                cr.y += zone_rect.y;
                cr.x  = hp_x;
                cr.width = hp_w;
                cr.height = std::max(cr.height, m_my_bar_min_height);
                return cr;
            }
            return zone_rect; // fallback: повна зона
        };

        struct MyBars my_bars = {};
        my_bars.hp_bar = rect;
        my_bars.cp_bar = findBarRect(cp_mask, cp_zone_rect);
        my_bars.mp_bar = findBarRect(mp_mask, mp_zone_rect);
        return my_bars;
    }

    return {};
}

std::optional<cv::Rect> Eyes::DetectTargetHPBar() const
{
    // extract red regions with red HP bar
    cv::Mat mask;
    cv::inRange(m_hsv, m_target_hp_color_from_hsv, m_target_hp_color_to_hsv, mask);
    
    // remove noise
    const auto kernel = cv::getStructuringElement(cv::MORPH_RECT, {25, m_target_hp_min_height});
    cv::erode(mask, mask, kernel);
    cv::dilate(mask, mask, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    for (const auto &contour : contours) {
        const auto rect = cv::boundingRect(contour);

        if (rect.height < m_target_hp_min_height || rect.height > m_target_hp_max_height ||
            rect.width < m_target_hp_min_width || rect.width > m_target_hp_max_width
        ) {
            continue;
        }

        return rect;
    }

    return {};
}

std::vector<std::vector<cv::Point>> Eyes::FindMyBarContours(const cv::Mat &mask) const
{
    // remove noise: ширина 3 видаляє вузькі (<3px) артефакти між CP та HP барами
    auto kernel = cv::getStructuringElement(cv::MORPH_RECT, {3, m_my_bar_min_height});
    cv::erode(mask, mask, kernel);
    cv::dilate(mask, mask, kernel);

    // join parts of the bar (gap up to 40px — for MP bar icon/border in L2 UI)
    kernel = cv::getStructuringElement(cv::MORPH_RECT, {40, 1});
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    return contours;
}

Eyes::NPC::State Eyes::DetectNPCState(const cv::Rect &rect) const
{
    // expand rect
    const auto expanded_rect = rect +
        cv::Size(m_target_circle_area_width * 2, m_target_circle_area_height - rect.height) +
        cv::Point(-m_target_circle_area_width, -(m_target_circle_area_height - rect.height) / 2);
    
    if (!IsRectInImage(m_hsv, expanded_rect)) {
        return NPC::State::Default;
    }

    cv::Mat bgr = m_bgr(expanded_rect);
    cv::rectangle(bgr, {m_target_circle_area_width, 0, rect.width, m_target_circle_area_height}, 0, -1);

    // extract circles
    cv::Mat gray;
    cv::inRange(bgr, m_target_gray_circle_color_bgr, m_target_gray_circle_color_bgr, gray);
    cv::Mat blue;
    cv::inRange(bgr, m_target_blue_circle_color_bgr, m_target_blue_circle_color_bgr, blue);
    cv::Mat red;
    cv::inRange(bgr, m_target_red_circle_color_bgr, m_target_red_circle_color_bgr, red);
    cv::Mat mask;
    cv::bitwise_or(gray, blue, mask);
    cv::bitwise_or(mask, red, mask);

    // increase regions size
    const auto kernel = cv::getStructuringElement(cv::MORPH_RECT, {5, 5});
    cv::dilate(mask, mask, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    if (contours.size() < 2) {
        return NPC::State::Default;
    }

    bool gray_found = cv::countNonZero(gray) > 0;

    // compare each contour to find pair
    for (const auto &contour1 : contours) {
        for (const auto &contour2 : contours) {
            if (contour1 == contour2) {
                continue;
            }

            const auto rect1 = cv::boundingRect(contour1);
            const auto rect2 = cv::boundingRect(contour2);

            if (rect1.y != rect2.y || rect1.size() != rect2.size()) {
                continue;
            }

            return gray_found ? NPC::State::Hovered : NPC::State::Selected;
        }
    }

    return NPC::State::Default;
}

template<typename T>
void Eyes::CalculateTrackingIds(std::vector<T> &npcs) const
{
    std::uint32_t max_tracking_id = 0;

    for (auto &npc : npcs) {
        npc.tracking_id = 0;

        for (const auto &previous_npc : m_npcs) {
            const auto distance = std::hypot(previous_npc.center.x - npc.center.x, previous_npc.center.y - npc.center.y);

            if (distance <= m_npc_tracking_distance) {
                npc.tracking_id = previous_npc.tracking_id;
                max_tracking_id = (std::max)(max_tracking_id, previous_npc.tracking_id);
                break;
            }
        }
    }

    std::uint32_t tracking_id = max_tracking_id;

    for (auto &npc : npcs) {
        if (npc.tracking_id == 0) {
            npc.tracking_id = ++tracking_id;
        }
    }
}

int Eyes::CalcBarPercentValue(
    const cv::Mat &bar,
    const cv::Scalar &from_color,
    const cv::Scalar &to_color,
    bool whole_bar
) {
    if (bar.rows < 1 || bar.depth() != CV_8U || bar.channels() < 3) return 0;

    const auto row = bar.ptr<uchar>(bar.rows / 2);
    auto channel = (bar.cols - 1) * bar.channels();
    auto cols = bar.cols;
    auto color_found = false;

    // loop mid row
    for (; channel > 0; channel -= bar.channels()) {
        if (row[channel + 0] >= from_color[0] && row[channel + 0] <= to_color[0] &&
            row[channel + 1] >= from_color[1] && row[channel + 1] <= to_color[1] &&
            row[channel + 2] >= from_color[2] && row[channel + 2] <= to_color[2]
        ) {
            if (!whole_bar) {
                break;
            } else {
                color_found = true;
            }
        } else if (color_found) {
            return 0;
        } else {
            cols--;
        }
    }

    return cols * 100 / bar.cols;
}

bool Eyes::IsGroundAhead() const {
    if (m_bgr.empty()) return true;
    int cx = m_bgr.cols / 2;
    int cy = m_bgr.rows - 30;
    if (cy < 10 || cx < 30) return true; // безпечний дефолт

    int x1 = std::max(0, cx - 30);
    int x2 = std::min(m_bgr.cols, cx + 30);
    int y1 = std::max(0, cy - 10);
    int y2 = std::min(m_bgr.rows, cy + 10);
    if (x2 <= x1 || y2 <= y1) return true;

    cv::Mat roi = m_bgr(cv::Rect(x1, y1, x2 - x1, y2 - y1));
    cv::Scalar mean = cv::mean(roi);
    // Якщо середня яскравість < 30 — підозріло темно (обрив/void)
    return (mean[0] + mean[1] + mean[2]) / 3.0 > 30.0;
}

// ── Навігація: детекція перешкод ─────────────────────────────────────────────

bool Eyes::IsCharacterMoving() const {
    if (m_bgr.empty()) return true; // безпечний дефолт

    // ROI: центральна ігрова зона, уникаємо UI
    // Вікно ~1366×768: відрізаємо верхні 150px (бари/мінімапа) + нижні 200px (хотбар/чат)
    // та ліві 200px і праві 200px (UI панелі)
    const int W = m_bgr.cols, H = m_bgr.rows;
    const int rx = std::min(200, W / 6);
    const int ry = std::min(150, H / 5);
    const int rw = std::max(100, W - rx * 2);
    const int rh = std::max(100, H - ry - std::min(200, H / 4));
    if (rx + rw > W || ry + rh > H || rw < 50 || rh < 50) return true;

    cv::Mat curr = m_bgr(cv::Rect(rx, ry, rw, rh));

    if (m_nav_prev_frame.empty() || m_nav_prev_frame.size() != curr.size()) {
        curr.copyTo(m_nav_prev_frame);
        return true; // перший виклик — вважаємо що рухаємось
    }

    cv::Mat diff;
    cv::absdiff(curr, m_nav_prev_frame, diff);
    const double mean_diff = cv::mean(diff)[0]; // середня різниця пікселів (0-255)
    curr.copyTo(m_nav_prev_frame);

    // > 3.0 = помітний рух на сцені
    return mean_diff > 3.0;
}

bool Eyes::IsWallAhead() const {
    if (m_bgr.empty()) return false;

    // ROI: центр екрану — куди дивиться персонаж при ходьбі
    // Уникаємо верхній UI (y<150) і нижній UI (hotbar/chat)
    const int W = m_bgr.cols, H = m_bgr.rows;
    const int cx = W / 2;
    const int cy = H / 2;
    const int rw = std::min(300, W / 5);
    const int rh = std::min(200, H / 4);
    const int rx = cx - rw / 2;
    const int ry = cy - rh / 4; // трохи вище центру — де з'являється стіна попереду
    if (rx < 0 || ry < 0 || rx + rw > W || ry + rh > H) return false;

    cv::Mat gray;
    cv::cvtColor(m_bgr(cv::Rect(rx, ry, rw, rh)), gray, cv::COLOR_BGR2GRAY);

    // Sobel X (вертикальні краї) + Sobel Y (горизонтальні краї)
    cv::Mat sobel_x, sobel_y, abs_x, abs_y;
    cv::Sobel(gray, sobel_x, CV_16S, 1, 0, 3);
    cv::Sobel(gray, sobel_y, CV_16S, 0, 1, 3);
    cv::convertScaleAbs(sobel_x, abs_x);
    cv::convertScaleAbs(sobel_y, abs_y);

    const double mean_v = cv::mean(abs_x)[0]; // вертикальні краї
    const double mean_h = cv::mean(abs_y)[0]; // горизонтальні краї

    // Стіна попереду: вертикальних ребер значно більше ніж горизонтальних
    // і загальна кількість ребер висока (не просто порожній коридор)
    return (mean_v > mean_h * 1.5) && (mean_v > 12.0);
}

float Eyes::GetMovementFlow() const {
    if (m_bgr.empty()) return 0.0f;

    // Зменшуємо розмір для швидкості (~4x швидше LK на half-res)
    cv::Mat small;
    cv::resize(m_bgr, small, {}, 0.5, 0.5, cv::INTER_LINEAR);

    cv::Mat gray;
    cv::cvtColor(small, gray, cv::COLOR_BGR2GRAY);

    if (m_nav_prev_gray.empty() || m_nav_prev_gray.size() != gray.size()) {
        gray.copyTo(m_nav_prev_gray);
        return 0.0f; // перший виклик
    }

    // Сітка точок у центральній ігровій зоні (half-res координати)
    // Вікно 683×384 (half 1366×768): уникаємо верхні 75px + нижні 100px + бокові 100px
    // Статична сітка точок — будується один раз, не виділяє пам'ять щотіку
    static const std::vector<cv::Point2f> pts = []() {
        std::vector<cv::Point2f> p;
        for (int y = 75; y < 285; y += 40)
            for (int x = 100; x < 580; x += 40)
                p.push_back({(float)x, (float)y});
        return p;
    }();

    if (pts.empty()) { gray.copyTo(m_nav_prev_gray); return 0.0f; }

    // Lucas-Kanade pyramidal optical flow
    std::vector<cv::Point2f> next_pts;
    std::vector<uchar> status;
    std::vector<float> err;
    const cv::TermCriteria criteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 20, 0.03);
    cv::calcOpticalFlowPyrLK(m_nav_prev_gray, gray, pts, next_pts,
                              status, err, cv::Size(15, 15), 2, criteria);

    float total = 0.0f;
    int count = 0;
    for (size_t i = 0; i < pts.size(); i++) {
        if (!status[i]) continue;
        const float dx = next_pts[i].x - pts[i].x;
        const float dy = next_pts[i].y - pts[i].y;
        total += std::sqrt(dx * dx + dy * dy);
        count++;
    }

    gray.copyTo(m_nav_prev_gray);
    // Масштабуємо back до full-res еквіваленту (*2) для інтуїтивного порогу
    return count > 0 ? (total / count) * 2.0f : 0.0f;
}

float Eyes::GetMinimapFlow() const {
    if (m_bgr.empty()) return 0.0f;

    const int W = m_bgr.cols;
    // Мінімапа ROI: верхній правий кут вікна
    const cv::Rect mm_roi(W - m_minimap_roi_from_right, 0,
                          m_minimap_roi_from_right, m_minimap_roi_height);
    if (!IsRectInImage(m_bgr, mm_roi)) return 0.0f;

    cv::Mat minimap_bgr = m_bgr(mm_roi);
    cv::Mat gray;
    cv::cvtColor(minimap_bgr, gray, cv::COLOR_BGR2GRAY);

    if (m_minimap_flow_prev_gray.empty() ||
        m_minimap_flow_prev_gray.size() != gray.size()) {
        gray.copyTo(m_minimap_flow_prev_gray);
        return 0.0f; // перший виклик
    }

    // Знаходимо характерні точки у попередньому кадрі мінімапи
    std::vector<cv::Point2f> pts;
    cv::goodFeaturesToTrack(m_minimap_flow_prev_gray, pts,
                            /*maxCorners=*/40,
                            /*qualityLevel=*/0.01,
                            /*minDistance=*/5.0);

    if (pts.size() < 4) {
        // Замало точок (порожня мінімапа або однотонний фон) — зберігаємо і виходимо
        gray.copyTo(m_minimap_flow_prev_gray);
        return 0.0f;
    }

    // Lucas-Kanade pyramidal optical flow між двома кадрами мінімапи
    std::vector<cv::Point2f> next_pts;
    std::vector<uchar> status;
    std::vector<float> err;
    const cv::TermCriteria criteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 15, 0.03);
    cv::calcOpticalFlowPyrLK(m_minimap_flow_prev_gray, gray, pts, next_pts,
                              status, err, cv::Size(11, 11), 2, criteria);

    float total = 0.0f;
    int count = 0;
    for (size_t i = 0; i < pts.size(); i++) {
        if (!status[i]) continue;
        const float dx = next_pts[i].x - pts[i].x;
        const float dy = next_pts[i].y - pts[i].y;
        total += std::sqrt(dx * dx + dy * dy);
        count++;
    }

    gray.copyTo(m_minimap_flow_prev_gray);
    return count > 0 ? (total / count) : 0.0f;
}

// ─────────────────────────────────────────────────────────────────────────────

int Eyes::CalcBarPercentValueRobust(
    const cv::Mat &bar,
    const cv::Scalar &from_color,
    const cv::Scalar &to_color,
    bool whole_bar
) {
    if (bar.rows < 3) {
        return CalcBarPercentValue(bar, from_color, to_color, whole_bar);
    }
    // Скануємо 3 рядки: 25%, 50%, 75% висоти → медіана
    int rows[3] = {bar.rows / 4, bar.rows / 2, bar.rows * 3 / 4};
    int values[3];
    for (int i = 0; i < 3; i++) {
        cv::Mat row = bar.row(rows[i]);
        values[i] = CalcBarPercentValue(row, from_color, to_color, whole_bar);
    }
    std::sort(values, values + 3);
    return values[1]; // медіана
}

std::vector<Eyes::MinimapDot> Eyes::DetectMinimap() const
{
    if (m_bgr.empty()) return {};

    const int W = m_bgr.cols;
    const int H = m_bgr.rows;

    // ROI: правий верхній кут вікна
    const int roi_x = std::max(0, W - m_minimap_roi_from_right);
    const int roi_y = 0;
    const int roi_w = std::min(m_minimap_roi_from_right, W);
    const int roi_h = std::min(m_minimap_roi_height, H);
    if (roi_w <= 0 || roi_h <= 0) return {};

    cv::Mat roi_bgr;
    if (m_bgr.channels() == 4)
        cv::cvtColor(m_bgr(cv::Rect(roi_x, roi_y, roi_w, roi_h)),
                     roi_bgr, cv::COLOR_BGRA2BGR);
    else
        roi_bgr = m_bgr(cv::Rect(roi_x, roi_y, roi_w, roi_h));

    // Шукаємо червоні точки (HSV: H=0-20 або 165-180, S>=80, V>=60)
    cv::Mat hsv;
    cv::cvtColor(roi_bgr, hsv, cv::COLOR_BGR2HSV);

    cv::Mat mask1, mask2, mask;
    // S≥100, H upper=20: виміряно H=10-19 (1417px) >> H=0-9 (35px) у підземелля
    cv::inRange(hsv, cv::Scalar(  0, 100, 80), cv::Scalar( 20, 255, 255), mask1);
    cv::inRange(hsv, cv::Scalar(165, 100, 80), cv::Scalar(180, 255, 255), mask2);
    cv::bitwise_or(mask1, mask2, mask);

    // Знаходимо контури (точки мобів)
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    const float excl_r2 = (float)(m_minimap_player_excl_r * m_minimap_player_excl_r);
    const float max_r2  = (float)((m_minimap_radius + 5) * (m_minimap_radius + 5));

    std::vector<MinimapDot> dots;
    for (const auto& c : contours) {
        const float area = (float)cv::contourArea(c);
        if (area < m_minimap_dot_area_min) continue;

        // Центр контуру
        cv::Moments m = cv::moments(c);
        if (m.m00 == 0.0) continue;
        const float cx = (float)(m.m10 / m.m00);
        const float cy = (float)(m.m01 / m.m00);

        const float dx = cx - (float)m_minimap_cx_in_roi;
        const float dy = cy - (float)m_minimap_cy_in_roi;
        const float d2 = dx * dx + dy * dy;

        if (d2 < excl_r2) continue; // занадто близько — гравець
        if (d2 > max_r2)  continue; // поза колом мінімапи

        dots.push_back({ (int)std::round(dx), (int)std::round(dy),
                         std::sqrt(d2), false });
    }

    // ── Вибраний моб: фіолетовий/маджента ореол (H=130-160, S>80, V>80) ──────
    // Після /target МобНейм гра показує фіолетове сяйво навколо точки на мінімапі.
    // Rotating Radar в ElmoreLab: пульсуючий ореол з'являється ~щоразу через 1-2 кадри.
    cv::Mat purple_mask;
    cv::inRange(hsv, cv::Scalar(130, 80, 80), cv::Scalar(165, 255, 255), purple_mask);

    std::vector<std::vector<cv::Point>> purple_contours;
    cv::findContours(purple_mask, purple_contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    // Центри фіолетових областей (dx/dy відносно центру мінімапи)
    struct PurpleDot { float dx, dy; };
    std::vector<PurpleDot> purple_dots;
    for (const auto& c : purple_contours) {
        if (cv::contourArea(c) < m_minimap_dot_area_min) continue;
        cv::Moments m = cv::moments(c);
        if (m.m00 == 0.0) continue;
        const float pdx = (float)(m.m10 / m.m00) - (float)m_minimap_cx_in_roi;
        const float pdy = (float)(m.m01 / m.m00) - (float)m_minimap_cy_in_roi;
        const float pd2 = pdx * pdx + pdy * pdy;
        if (pd2 < excl_r2 || pd2 > max_r2) continue;
        purple_dots.push_back({ pdx, pdy });
    }

    // Позначаємо ту червону точку, яка найближча до фіолетового ореолу (≤15px)
    static constexpr float kPurpleMatchRadius2 = 15.0f * 15.0f;
    if (!purple_dots.empty()) {
        for (auto& dot : dots) {
            for (const auto& pd : purple_dots) {
                const float ddx = (float)dot.dx - pd.dx;
                const float ddy = (float)dot.dy - pd.dy;
                if (ddx * ddx + ddy * ddy <= kPurpleMatchRadius2) {
                    dot.selected = true;
                    break;
                }
            }
        }
    }

    // Сортуємо від найближчого до найдальшого
    std::sort(dots.begin(), dots.end(),
              [](const MinimapDot& a, const MinimapDot& b){ return a.dist < b.dist; });
    return dots;
}

std::optional<cv::Point> Eyes::FindTemplate(const cv::Mat& templ, float threshold,
                                             float* out_score) const
{
    if (m_bgr.empty() || templ.empty()) return {};
    if (templ.cols > m_bgr.cols || templ.rows > m_bgr.rows) return {};

    // matchTemplate вимагає однаковий тип — приводимо обидва до BGR (3ch)
    cv::Mat img3, tmpl3;
    if (m_bgr.channels() == 4)
        cv::cvtColor(m_bgr, img3, cv::COLOR_BGRA2BGR);
    else
        img3 = m_bgr;

    if (templ.channels() == 4)
        cv::cvtColor(templ, tmpl3, cv::COLOR_BGRA2BGR);
    else
        tmpl3 = templ;

    cv::Mat result;
    cv::matchTemplate(img3, tmpl3, result, cv::TM_CCOEFF_NORMED);

    double maxVal;
    cv::Point maxLoc;
    cv::minMaxLoc(result, nullptr, &maxVal, nullptr, &maxLoc);

    if (out_score) *out_score = (float)maxVal;
    if (maxVal < (double)threshold) return {};

    // Повертаємо центр знайденого шаблону
    return cv::Point{maxLoc.x + templ.cols / 2, maxLoc.y + templ.rows / 2};
}

std::uint32_t Eyes::Hash(const cv::Mat &image)
{
    // djb2 hash
    std::uint32_t hash = 5381;
    const auto total = image.total();

    for (std::size_t i = 0; i < total; ++i) {
        hash = ((hash << 5) + hash) ^ *(image.data + i);
    }

    return hash;
}

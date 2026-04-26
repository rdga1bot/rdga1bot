// BotBT_Target.cpp
// Target selector subtree: actTarget + 7 вузлів пошуку цілі.
//
// actTgtInit       — ініціалізація пошуку + breadcrumb checkpoint.
// actTgtDeadTarget — мертвий таргет (hp=0 або kl_mob_died) → ESC/macro.
// actTgtMinimap    — ротація до найближчого моба з мінімапу.
// actTgtF2AndMacro — F2 nexttarget + macro fallback + pokemon-logic.
// actTgtNavigation — stuck detection + breadcrumbs + memory navigation.
// actTgtGeoPath    — NavMesh + geodata waypoints + WalkForward.
// actTgtPatrol     — patrol + rotate + explore (завжди Running).
//
// tgtHandleMinimap        — логіка ротації + наближення до dot.
// tgtSendF2AndMacro       — відправка F2 + macro послідовності.
// tgtHandlePatrolAndRotate — patrol rotation + stuck recovery.
#include "BotBehaviorTree.h"
#include "Input.h"
#include "Geodata.h"
#include "navmesh_builder.h"
#include <cmath>

// actTarget — замінено піддеревом tgt_root в MR28. Не використовується.
BTStatus BotBehaviorTree::actTarget(GameState& gs) {
    (void)gs;
    return BTStatus::Failure;
}

// ─────────────────────────────────────────────────────────────────────────────
// TARGET ПІДДЕРЕВО (MR28) — статичні BT callback вузли
// ─────────────────────────────────────────────────────────────────────────────

BTStatus BotBehaviorTree::actTgtInit(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.m_active_branch = "Target";

    if (!gs.hands_ready) {
        self.m_tgt_not_ready_count++;
        if (self.m_tgt_not_ready_count == 20 || self.m_tgt_not_ready_count % 100 == 0)
            gs.log("[WARNING] TARGETING: IsReady=false вже " +
                std::to_string(self.m_tgt_not_ready_count) + " тіків — Input thread завис?");
        return BTStatus::Running;
    }
    self.m_tgt_not_ready_count = 0;

    if (!self.m_tgt_active) {
        self.resetTargetState(gs);
        self.m_tgt_active = true;
    }

    if (gs.coords_valid)
        self.addCrumb(gs.player_x, gs.player_y, gs.player_z, gs.cfg.breadcrumbs);

    return BTStatus::Failure; // передати до actTgtDeadTarget
}

BTStatus BotBehaviorTree::actTgtDeadTarget(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto r = s_self->tgtHandleDeadTarget(gs);
    return r.value_or(BTStatus::Failure);
}

BTStatus BotBehaviorTree::actTgtMinimap(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;

    self.m_tgt_macro_attempts++;

    // Обчислюємо і зберігаємо map_ref для downstream вузлів
    const auto& minimap_dots = gs.minimap_dots;
    self.m_tgt_map_ref          = nullptr;
    self.m_tgt_map_ref_selected = false;
    for (const auto& d : minimap_dots) {
        if (d.selected) {
            self.m_tgt_map_ref          = &d;
            self.m_tgt_map_ref_selected = true;
            break;
        }
    }
    if (!self.m_tgt_map_ref && !minimap_dots.empty())
        self.m_tgt_map_ref = &minimap_dots[0];

    self.tgtHandleMinimap(gs, self.m_tgt_map_ref, self.m_tgt_map_ref_selected);
    return BTStatus::Failure; // передати до actTgtF2AndMacro
}

BTStatus BotBehaviorTree::actTgtF2AndMacro(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    // streak==0 = атаки йдуть, HP моба змінюється → таргет не міняємо.
    // streak>0 = атаки не йдуть (ідемо до макро-таргету або застрягли) → F2 шукає ближчого.
    if (gs.has_target && self.m_atk_unreachable_streak == 0) {
        gs.log("[F2] SKIP has_tgt=1 streak=0 (атаки йдуть)");
        return BTStatus::Failure;
    }
    gs.log("[F2] SEND has_tgt=" + std::to_string(gs.has_target ? 1 : 0)
        + " streak=" + std::to_string(self.m_atk_unreachable_streak)
        + " dots=" + std::to_string(gs.minimap_dots.size())
        + " attempts=" + std::to_string(self.m_tgt_macro_attempts));
    s_self->tgtSendF2AndMacro(gs);
    return BTStatus::Failure; // передати до actTgtNavigation
}

BTStatus BotBehaviorTree::actTgtNavigation(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    auto r = self.tgtHandleNavigation(gs, self.m_tgt_map_ref);
    return r.value_or(BTStatus::Failure);
}

BTStatus BotBehaviorTree::actTgtGeoPath(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    auto r = self.tgtHandleGeoPath(gs, self.m_tgt_map_ref);
    return r.value_or(BTStatus::Failure);
}

BTStatus BotBehaviorTree::actTgtPatrol(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.tgtHandlePatrolAndRotate(gs, self.m_tgt_map_ref);
    gs.hands.Send(150);
    return BTStatus::Running; // останній вузол — завжди Running
}

// ─────────────────────────────────────────────────────────────────────────────
// actTarget ПІДФУНКЦІЇ
// ─────────────────────────────────────────────────────────────────────────────

std::optional<BTStatus> BotBehaviorTree::tgtHandleDeadTarget(GameState& gs) {
    if (!gs.target.has_value() || gs.target->hp != 0) {
        m_tgt_dead_esc_count = 0;
        if (gs.target.has_value() && gs.target->hp > 0)
            m_tgt_dead_cycles_total = 0;
        return std::nullopt;
    }

    if (m_tgt_pokemon_targeted) {
        gs.log("[Pokemon] sweep (чекаємо анімацію)...");
        gs.hands.Delay(1500);
        gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
        m_tgt_pokemon_targeted = false;
        m_tgt_dead_esc_count = 0;
        gs.hands.Send(100);
        return BTStatus::Running;
    }
    m_tgt_dead_esc_count++;
    if (m_tgt_dead_esc_count == 1) {
        gs.log("[TARGETING] Мертвий таргет hp=0 ×1 → чекаємо підтвердження...");
        gs.hands.Send(250);
        return BTStatus::Running;
    }
    gs.log("[TARGETING] Мертвий таргет hp=0 → ESC ×" +
        std::to_string(m_tgt_dead_esc_count - 1));
    if (m_tgt_dead_esc_count <= 6) {
        gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
        gs.hands.Send(100);
        return BTStatus::Running;
    }
    // Після 5 спроб — fallthrough до F2/macro
    m_tgt_dead_esc_count = 0;
    m_tgt_dead_cycles_total++;
    if (m_tgt_dead_cycles_total >= gs.cfg.targeting_tuning.dead_cycles_macro_switch
        && !gs.cfg.target_macro_keys.empty()) {
        gs.log("[TARGETING] Dead-target loop ×" +
            std::to_string(m_tgt_dead_cycles_total) + " → /target макрос");
        m_tgt_macro_attempts++;
        gs.hands.TargetMacro(m_tgt_macro_idx);
        m_tgt_macro_idx = (m_tgt_macro_idx + 1) % (int)gs.cfg.target_macro_keys.size();
        // 1000ms: сервер повинен встигнути обробити /target і оновити таргет.
        // 300ms було замало → chain ×3→×4→×5 в логах.
        gs.hands.Send(1000);
        return BTStatus::Running;
    }
    gs.log("[TARGETING] Мертвий таргет не зникає після 5 ESC → пробуємо F2");
    return std::nullopt;
}

void BotBehaviorTree::tgtHandleMinimap(GameState& gs,
        const Eyes::MinimapDot* map_ref, bool map_ref_selected) {
    const int kMinimapDxThreshold = gs.cfg.targeting_tuning.minimap_dx_threshold;
    const int kMinimapRotateLimit = gs.cfg.targeting_tuning.minimap_rotate_limit;

    if (map_ref && m_tgt_minimap_rotate_count < kMinimapRotateLimit) {
        const int dx = map_ref->dx;
        const int dy = map_ref->dy;
        const char* who = map_ref_selected ? "Вибраний" : "Найближчий";
        if (dx < -kMinimapDxThreshold) {
            // MR75: відстежуємо прогрес dx — чи ротація наближає нас до моба?
            bool no_progress = (m_tgt_prev_dx < 0 && std::abs(dx - m_tgt_prev_dx) < 4);
            m_tgt_prev_dx = dx;
            m_tgt_dx_stuck_count = no_progress ? m_tgt_dx_stuck_count + 1 : 0;
            gs.hands.RotateLeft(RandMs(m_tgt_rd_rotate.get(), gs, 120));
            m_tgt_minimap_rotate_count++;
            gs.log(std::string("[MAP] ") + who + " моб ліворуч (dx=" +
                std::to_string(dx) + ", rot=" +
                std::to_string(m_tgt_minimap_rotate_count) + ") → RotateLeft");
            if (m_tgt_dx_stuck_count >= 3) {
                // dx не змінюється: моб за стіною/геодата → WalkForward без умов
                gs.log("[MAP] dx stuck×" + std::to_string(m_tgt_dx_stuck_count) +
                    " (dx=" + std::to_string(dx) + ") → WalkForward (bypass geodata)");
                gs.hands.WalkForward(RandMs(m_tgt_rd_walk.get(), gs, 800));
                m_tgt_nav_prev_was_walk = true;
                m_tgt_dx_stuck_count = 0;
                m_tgt_minimap_rotate_count = 0;
                m_tgt_prev_dx = 0;
            }
        } else if (dx > kMinimapDxThreshold) {
            bool no_progress = (m_tgt_prev_dx > 0 && std::abs(dx - m_tgt_prev_dx) < 4);
            m_tgt_prev_dx = dx;
            m_tgt_dx_stuck_count = no_progress ? m_tgt_dx_stuck_count + 1 : 0;
            gs.hands.RotateRight(RandMs(m_tgt_rd_rotate.get(), gs, 120));
            m_tgt_minimap_rotate_count++;
            gs.log(std::string("[MAP] ") + who + " моб праворуч (dx=" +
                std::to_string(dx) + ", rot=" +
                std::to_string(m_tgt_minimap_rotate_count) + ") → RotateRight");
            if (m_tgt_dx_stuck_count >= 3) {
                gs.log("[MAP] dx stuck×" + std::to_string(m_tgt_dx_stuck_count) +
                    " (dx=" + std::to_string(dx) + ") → WalkForward (bypass geodata)");
                gs.hands.WalkForward(RandMs(m_tgt_rd_walk.get(), gs, 800));
                m_tgt_nav_prev_was_walk = true;
                m_tgt_dx_stuck_count = 0;
                m_tgt_minimap_rotate_count = 0;
                m_tgt_prev_dx = 0;
            }
        } else {
            m_tgt_minimap_rotate_count = 0;
            m_tgt_dx_stuck_count = 0;
            m_tgt_prev_dx = 0;
            if (dy > 30) {
                gs.hands.RotateRight(RandMs(m_tgt_rd_rotate.get(), gs, 700));
                gs.log(std::string("[MAP] ") + who + " моб позаду (dy=" +
                    std::to_string(dy) + ") → розворот 180°");
            }
        }
    } else if (map_ref && m_tgt_minimap_rotate_count >= kMinimapRotateLimit) {
        m_tgt_minimap_rotate_count = 0;
        m_tgt_prev_dx = 0;
        gs.log("[MAP] Ліміт ротацій → WalkForward до моба (dx=" +
            std::to_string(map_ref->dx) + ")");
        // MR75: WalkForward без умови IsGroundAhead — зміна позиції важливіша за перевірку
        gs.hands.WalkForward(RandMs(m_tgt_rd_walk.get(), gs, 600));
        m_tgt_nav_prev_was_walk = true;
    } else if (!map_ref) {
        m_tgt_minimap_rotate_count = 0;
        m_tgt_prev_dx = 0;
        m_tgt_dx_stuck_count = 0;
    }
}

void BotBehaviorTree::tgtSendF2AndMacro(GameState& gs) {
    // Основний метод: F2 /nexttarget
    gs.hands.NextTarget();

    // Скидаємо unreachable flag тільки якщо є БЛИЗЬКІ моби (minimap_close_threat)
    // І streak невеликий (< 5 послідовних unreachable).
    // MR76: обмежуємо до 3 скидань поспіль — якщо всі 14 мобів за стіною,
    // "close on minimap" ніколи не стане reachable і ми ніколи не дійдемо до force cycle.
    // Root cause 17-хвилинного gap: 540 close-resets у сесії, force#1 ніколи не accumulate.
    static constexpr int kUnreachStreakForceRetarget = 5;
    static constexpr int kCloseResetMax = 3;  // MR76
    if (gs.minimap_close_threat && m_attack_was_unreachable
        && m_atk_unreachable_streak < kUnreachStreakForceRetarget
        && m_close_unreachable_count < kCloseResetMax) {
        m_close_unreachable_count++;
        m_attack_was_unreachable = false;
        gs.log("[TARGETING] Близькі моби (minimap) → скидаємо unreachable ×"
               + std::to_string(m_close_unreachable_count) + ", F2 вже відіслано");
        return;
    }
    if (m_atk_unreachable_streak >= kUnreachStreakForceRetarget) {
        m_atk_streak_force_count++;
        gs.log("[TARGETING] Streak ×" + std::to_string(m_atk_unreachable_streak)
            + " force#" + std::to_string(m_atk_streak_force_count)
            + " → форсуємо повний цикл (ігноруємо minimap_close_threat)");
        m_atk_unreachable_streak = 0;

        // Після 3 форс-циклів поспіль (~75с без kills) — ESC + WalkForward,
        // щоб бот фізично вийшов зі stuck zone (стіна / різниця рівнів).
        // MR76: WalkForward(4000) — 4с руху вперед перед наступним targeting.
        static constexpr int kForceEscAfter = 3;
        if (m_atk_streak_force_count >= kForceEscAfter) {
            gs.log("[TARGETING] force#" + std::to_string(m_atk_streak_force_count)
                + " → ESC + WalkForward (вихід зі stuck zone)");
            gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            gs.hands.Send(300);
            gs.hands.WalkForward(4000);  // MR76: фізично виходимо із stuck area
            const_cast<GameState&>(gs).has_target = false;
            m_atk_streak_force_count = 0;
            m_attack_was_unreachable = false;
            m_close_unreachable_count = 0;  // MR76
            return; // не надсилаємо F2/macro — patrol сам знайде
        }
    }

    // /target макроси: тільки після N невдалих F2
    const int kMacroFallbackAfterUnreach = gs.cfg.targeting_tuning.macro_fallback_unreach;
    const int kMacroFallbackAfter = gs.cfg.targeting_tuning.macro_fallback_after > 0
                                    ? gs.cfg.targeting_tuning.macro_fallback_after : 10;
    const bool macro_fallback = !gs.cfg.target_macro_keys.empty()
                                && m_tgt_macro_attempts > (m_attack_was_unreachable
                                    ? kMacroFallbackAfterUnreach : kMacroFallbackAfter)
                                && m_tgt_macro_attempts % 2 == 0;
    if (macro_fallback) {
        gs.hands.Delay(80);
        gs.hands.TargetMacro(m_tgt_macro_idx);
        m_tgt_macro_idx = (m_tgt_macro_idx + 1) % (int)gs.cfg.target_macro_keys.size();
    }

    // Pokemon макрос: ОДИН РАЗ на початку TARGETING циклу
    if (gs.cfg.has_pokemon_key && !m_tgt_pokemon_fired && m_tgt_macro_attempts == 1) {
        m_tgt_pokemon_fired = true;
        gs.hands.Delay(50);
        gs.hands.PressKeyboardKey(gs.cfg.pokemon_key);
        m_tgt_pokemon_targeted = true;
        gs.log("[Pokemon] макрос");
    }
}

std::optional<BTStatus> BotBehaviorTree::tgtHandleNavigation(GameState& gs,
        const Eyes::MinimapDot* map_ref) {
    (void)map_ref;

    // ── Navigation stuck detection ────────────────────────────────────────────
    if (gs.cfg.nav_stuck_detection) {
        const bool is_moving = gs.eyes.IsCharacterMoving();

        if (m_tgt_nav_prev_was_walk) {
            m_tgt_nav_prev_was_walk = false;

            if (m_tgt_running_to_mob) {
                const double run_secs = secsSince(m_tgt_run_started);
                if (run_secs >= 15.0) {
                    const int rotation_ms = std::min(
                        900 + (m_tgt_nav_stuck_recoveries / 2) * 450, 1800);
                    gs.hands.WalkBack(300);
                    if (m_tgt_nav_stuck_recoveries % 2 == 0) {
                        gs.hands.RotateRight(rotation_ms);
                        gs.log("[NAV] Біг " + std::to_string((int)run_secs) +
                            "с без таргету → WalkBack + RotateRight(" +
                            std::to_string(rotation_ms) + "мс)");
                    } else {
                        gs.hands.RotateLeft(rotation_ms);
                        gs.log("[NAV] Біг " + std::to_string((int)run_secs) +
                            "с без таргету → WalkBack + RotateLeft(" +
                            std::to_string(rotation_ms) + "мс)");
                    }
                    m_tgt_nav_stuck_recoveries++;
                    m_tgt_run_started = now();
                    m_tgt_nav_prev_was_walk = true;
                }
            } else {
                const float flow    = gs.cfg.nav_flow_detection
                    ? gs.eyes.GetMovementFlow() : -1.0f;
                const float mm_flow = gs.cfg.nav_flow_detection
                    ? gs.eyes.GetMinimapFlow() : -1.0f;
                const bool actually_moved = is_moving || (flow > 1.5f) || (mm_flow > 0.3f);
                if (!actually_moved) {
                    m_tgt_walk_stuck_count++;
                    gs.log("[NAV] Не рухаємось після WalkForward ×" +
                        std::to_string(m_tgt_walk_stuck_count) +
                        (flow >= 0 ? " flow=" + std::to_string(flow).substr(0,4) : ""));
                    if (m_tgt_walk_stuck_count >= gs.cfg.nav_stuck_threshold) {
                        m_tgt_walk_stuck_count = 0;
                        const int rotation_ms = std::min(
                            900 + (m_tgt_nav_stuck_recoveries / 2) * 450, 1800);
                        gs.hands.WalkBack(300);
                        if (m_tgt_nav_stuck_recoveries % 2 == 0) {
                            gs.hands.RotateRight(rotation_ms);
                            gs.log("[NAV] Застряг ×" +
                                std::to_string(m_tgt_nav_stuck_recoveries + 1) +
                                " → WalkBack + RotateRight(" +
                                std::to_string(rotation_ms) + "мс)");
                        } else {
                            gs.hands.RotateLeft(rotation_ms);
                            gs.log("[NAV] Застряг ×" +
                                std::to_string(m_tgt_nav_stuck_recoveries + 1) +
                                " → WalkBack + RotateLeft(" +
                                std::to_string(rotation_ms) + "мс)");
                        }
                        gs.hands.WalkForward(500);
                        m_tgt_nav_prev_was_walk = true;
                        m_tgt_nav_stuck_recoveries++;

                        // Breadcrumbs: ініціація backtrack
                        if (gs.cfg.breadcrumbs.enabled && gs.coords_valid
                            && !m_backtracking
                            && m_tgt_nav_stuck_recoveries >= gs.cfg.breadcrumbs.stuck_threshold) {
                            auto bc = findBacktrackCrumb(gs.player_x, gs.player_y,
                                                         gs.cfg.breadcrumbs.backtrack_range);
                            if (bc.has_value()) {
                                m_backtracking = true;
                                gs.log("[BREADCRUMB] Застряг ×" +
                                    std::to_string(m_tgt_nav_stuck_recoveries) +
                                    " → backtrack до (" +
                                    std::to_string((int)bc->x) + "," +
                                    std::to_string((int)bc->y) + ")");
                            }
                        }
                    }
                } else {
                    m_tgt_walk_stuck_count = 0;
                    m_tgt_nav_stuck_recoveries = 0;
                    if (flow > 0)
                        gs.log("[NAV] Рух ок, flow=" + std::to_string(flow).substr(0,4));
                }
            }
        }
    }

    // ── Breadcrumbs: виконання повернення ────────────────────────────────────
    if (m_backtracking && gs.coords_valid && gs.navigate_to_mob) {
        auto bc = findBacktrackCrumb(gs.player_x, gs.player_y,
                                     gs.cfg.breadcrumbs.backtrack_range);
        if (bc.has_value()) {
            L2Character dummy{};
            dummy.x = bc->x; dummy.y = bc->y; dummy.z = bc->z;
            dummy.hp = 100.f; dummy.hpMax = 100.f;
            if (!gs.navigate_to_mob(dummy)) {
                m_backtracking = false;
                m_tgt_nav_stuck_recoveries = 0;
                gs.log("[BREADCRUMB] Точку досягнуто → відновлюємо пошук");
            } else {
                gs.hands.Send(200);
                return BTStatus::Running;
            }
        } else {
            m_backtracking = false;
            gs.log("[BREADCRUMB] Крихти вичерпано");
        }
    }

    // ── Memory навігація ──────────────────────────────────────────────────────
    // RL override: NavigateMemory форсує навігацію навіть якщо cfg.navigation.enabled=false
    const bool rl_nav_forced = m_rl_model
        && m_rl_suggested_action == LinearQModel::Action::NavigateMemory
        && m_rl_action_confidence > 0.5f
        && !gs.kl_mobs.empty()
        && gs.coords_valid && gs.navigate_to_mob;

    if ((gs.cfg.navigation.enabled || rl_nav_forced) && !gs.kl_mobs.empty()) {
        auto kl_mobs = gs.kl_mobs;
        kl_mobs.erase(std::remove_if(kl_mobs.begin(), kl_mobs.end(),
            [&gs](const L2Character& mob) {
                return gs.is_blacklisted && gs.is_blacklisted(mob.objectID);
            }), kl_mobs.end());
        if (!kl_mobs.empty()) {
            std::optional<L2Character> nearest;
            if (rl_nav_forced && !gs.cfg.navigation.enabled) {
                // NavigateMemory override: завжди найближчий (не зважений)
                gs.log("[RL] NavigateMemory override → nav до найближчого моба");
                if (gs.find_nearest_mob)
                    nearest = gs.find_nearest_mob(kl_mobs, gs.player_x, gs.player_y,
                                                  gs.cfg.weighted_target.max_range);
            } else if (gs.cfg.weighted_target.enabled && gs.select_target) {
                nearest = gs.select_target(kl_mobs, gs.player_x, gs.player_y);
            } else if (gs.find_nearest_mob) {
                nearest = gs.find_nearest_mob(kl_mobs, gs.player_x, gs.player_y,
                                              gs.cfg.weighted_target.max_range);
            }
            // RL: підтвердження TargetNearest / TargetWeighted (BT вже виконує цю дію)
            if (m_rl_model && nearest.has_value()) {
                const bool is_nearest  = m_rl_suggested_action == LinearQModel::Action::TargetNearest;
                const bool is_weighted = m_rl_suggested_action == LinearQModel::Action::TargetWeighted;
                if (is_nearest || is_weighted) {
                    const std::string who = nearest->name.empty()
                        ? ("ID=" + std::to_string(nearest->objectID)) : nearest->name;
                    gs.log("[RL] " + std::string(is_nearest ? "TargetNearest" : "TargetWeighted")
                        + " підтверджено → " + who);
                }
            }
            if (nearest.has_value() && gs.navigate_to_mob) {
                bool navigated = gs.navigate_to_mob(*nearest);
                if (navigated) {
                    std::string who = nearest->name.empty()
                        ? ("ID=" + std::to_string(nearest->objectID))
                        : nearest->name;
                    gs.log("[NAV-MEM] → " + who +
                        " hp=" + std::to_string((int)nearest->hpPercent()) + "%");
                    gs.hands.Send(150);
                    return BTStatus::Running;
                }
                // Запит шляху через GeodataWorker
                if (!m_tgt_geo_path_ready && !m_tgt_pending_path.has_value()
                    && gs.geodata && gs.geodata->IsLoaded(nearest->x, nearest->y)
                    && gs.coords_valid) {
                    PathRequest req;
                    req.sx = gs.player_x; req.sy = gs.player_y; req.sz = gs.player_z;
                    req.ex = nearest->x;  req.ey = nearest->y;  req.ez = nearest->z;
                    req.maxRange = gs.cfg.knownlist_max_range;
                    req.id = ++m_tgt_path_req_id;
                    m_tgt_pending_path = req;
                }
            }
        }
    }

    return std::nullopt;
}

std::optional<BTStatus> BotBehaviorTree::tgtHandleGeoPath(GameState& gs,
        const Eyes::MinimapDot* map_ref) {
    const int kMinimapDxThreshold = gs.cfg.targeting_tuning.minimap_dx_threshold;

    // ── NavMesh FindPath ──────────────────────────────────────────────────────
    if (gs.navmesh && gs.navmesh->IsValid()
        && gs.coords_valid && !gs.kl_mobs.empty()
        && !m_tgt_geo_path_ready) {
        const L2Character* tgt = nullptr;
        float best_d2 = 1e12f;
        for (const auto& mob : gs.kl_mobs) {
            if (mob.isDead || !mob.isAlive()) continue;
            float dx = mob.x - gs.player_x, dy = mob.y - gs.player_y;
            float d2 = dx*dx + dy*dy;
            if (d2 < best_d2) { best_d2 = d2; tgt = &mob; }
        }
        if (tgt) {
            auto nm_path = gs.navmesh->FindPath(
                gs.player_x, gs.player_y, gs.player_z,
                tgt->x,      tgt->y,      tgt->z);
            if (!nm_path.empty()) {
                m_tgt_geo_path       = nm_path;
                m_tgt_geo_path_idx   = 0;
                m_tgt_geo_path_id    = ++m_tgt_path_req_id;
                m_tgt_geo_path_ready = true;
                gs.log("[NAVMESH] Шлях: " + std::to_string(nm_path.size()) + " точок");
            }
        }
    }

    // ── Geodata waypoint following ────────────────────────────────────────────
    if (gs.cfg.navigation.enabled && m_tgt_geo_path_ready
        && m_tgt_geo_path_idx < m_tgt_geo_path.size() && gs.coords_valid) {
        auto [wx, wy] = m_tgt_geo_path[m_tgt_geo_path_idx];
        float dx_wp = wx - gs.player_x;
        float dy_wp = wy - gs.player_y;
        float dist_wp = std::sqrt(dx_wp * dx_wp + dy_wp * dy_wp);
        if (dist_wp < 300.f) {
            m_tgt_geo_path_idx++;
            gs.log("[GEO] Waypoint " + std::to_string(m_tgt_geo_path_idx) +
                "/" + std::to_string(m_tgt_geo_path.size()) + " досягнуто");
        } else {
            L2Character wp{};
            wp.x = wx; wp.y = wy; wp.z = gs.player_z;
            wp.hp = 100.f; wp.hpMax = 100.f;
            if (gs.navigate_to_mob && gs.navigate_to_mob(wp)) {
                gs.log("[GEO] → waypoint " + std::to_string(m_tgt_geo_path_idx) +
                    " dist=" + std::to_string((int)dist_wp));
                gs.hands.Send(150);
                return BTStatus::Running;
            }
        }
        if (m_tgt_geo_path_idx >= m_tgt_geo_path.size()) {
            m_tgt_geo_path_ready = false;
            gs.log("[GEO] Шлях пройдено");
        }
    }

    // WalkForward за unreachable_streak вилучено (MR73):
    // Спрацьовувало під час нормального бою через KL-HP false positives →
    // бот крокував у протилежну від моба сторону після 3-5 атак.
    // Реальний вихід зі застрягань — tgtHandlePatrolAndRotate (MR65, 20с без руху).
    (void)map_ref;

    return std::nullopt;
}

void BotBehaviorTree::tgtHandlePatrolAndRotate(GameState& gs,
        const Eyes::MinimapDot* map_ref) {
    const auto& minimap_dots = gs.minimap_dots;
    const bool minimap_empty = minimap_dots.empty();
    const bool long_search   = (m_tgt_macro_attempts >= 20)
                               && (m_tgt_macro_attempts % 10 == 0);

    if (m_tgt_macro_attempts % 5 == 0 && (minimap_empty || long_search)) {
        gs.hands.Delay(50);
        m_tgt_step_count++;
        gs.stats.RecordTargetingFailure();
        m_rl_sig_targeting_failed = true;

        // RL override: активувати patrol раніше порогу якщо RL рекомендує
        const bool rl_patrol_boost = m_rl_model
            && m_rl_suggested_action == LinearQModel::Action::Patrol
            && m_rl_action_confidence > 0.5f;
        const bool patrol_ready = gs.cfg.patrol_enabled
            && !gs.cfg.patrol_path.empty()
            && (
                (m_tgt_macro_attempts >= gs.cfg.patrol_trigger_attempts
                 && m_tgt_macro_attempts % 5 == 0)
                || rl_patrol_boost  // RL override: без % 5 обмеження
            );

        if (patrol_ready) {
            if (rl_patrol_boost)
                gs.log("[RL] Patrol override → patrol крок " +
                    std::to_string(m_tgt_patrol_step_idx % (int)gs.cfg.patrol_path.size() + 1));
            const auto& step = gs.cfg.patrol_path[
                m_tgt_patrol_step_idx % (int)gs.cfg.patrol_path.size()];
            int pat_step_display = m_tgt_patrol_step_idx % (int)gs.cfg.patrol_path.size() + 1;
            int pat_total = (int)gs.cfg.patrol_path.size();
            switch (step.dir) {
                case Config::PatrolStep::Dir::Forward:
                    gs.hands.WalkForward(step.ms);
                    m_tgt_nav_prev_was_walk = true;
                    gs.log("[PATROL] Крок " + std::to_string(pat_step_display) +
                        "/" + std::to_string(pat_total) +
                        " → Forward(" + std::to_string(step.ms) + "мс)");
                    break;
                case Config::PatrolStep::Dir::Back:
                    gs.hands.WalkBack(step.ms);
                    gs.log("[PATROL] Крок " + std::to_string(pat_step_display) +
                        "/" + std::to_string(pat_total) +
                        " → Back(" + std::to_string(step.ms) + "мс)");
                    break;
                case Config::PatrolStep::Dir::RotateLeft:
                    gs.hands.RotateLeft(step.ms);
                    gs.log("[PATROL] Крок " + std::to_string(pat_step_display) +
                        "/" + std::to_string(pat_total) +
                        " → RotateLeft(" + std::to_string(step.ms) + "мс)");
                    break;
                case Config::PatrolStep::Dir::RotateRight:
                    gs.hands.RotateRight(step.ms);
                    gs.log("[PATROL] Крок " + std::to_string(pat_step_display) +
                        "/" + std::to_string(pat_total) +
                        " → RotateRight(" + std::to_string(step.ms) + "мс)");
                    break;
            }
            m_tgt_patrol_step_idx++;
        } else {
            if ((m_tgt_macro_attempts / 5) % 2 == 0)
                gs.hands.RotateRight(RandMs(m_tgt_rd_rotate.get(), gs, 350));
            else
                gs.hands.RotateLeft(RandMs(m_tgt_rd_rotate.get(), gs, 350));
            // Примусовий вихід зі стіни: якщо >20с немає жодного руху →
            // WalkForward незалежно від IsGroundAhead (стіна = нескінченна ротація).
            const double secs_no_walk = secsSince(m_tgt_last_walk_time);
            const bool force_escape = !m_tgt_nav_prev_was_walk && secs_no_walk > 20.0;

            const bool explore_trigger = !patrol_ready && !m_tgt_nav_prev_was_walk
                && (gs.eyes.IsGroundAhead()
                    && ((minimap_empty && m_tgt_macro_attempts % 15 == 0)
                        || (!minimap_empty && m_tgt_macro_attempts % 20 == 0)));

            if (force_escape) {
                gs.hands.WalkForward(1500);
                m_tgt_nav_prev_was_walk = true;
                m_tgt_last_walk_time    = now();
                gs.log("[TARGETING] Спроба " + std::to_string(m_tgt_macro_attempts) +
                    " — force escape (>20с без руху, стіна?)");
            } else if (explore_trigger) {
                gs.hands.WalkForward(1200);
                m_tgt_nav_prev_was_walk = true;
                m_tgt_last_walk_time    = now();
                gs.log("[TARGETING] Спроба " + std::to_string(m_tgt_macro_attempts) +
                    " — розвідка вперед (" +
                    (minimap_empty ? "мінімапа порожня" : "dot недосяжний") + ")");
            } else {
                gs.log("[TARGETING] Спроба " + std::to_string(m_tgt_macro_attempts) +
                    " — ротація (мінімапа порожня)");
            }
        }
    } else if (m_tgt_macro_attempts % 5 == 0) {
        gs.stats.RecordTargetingFailure();
        m_rl_sig_targeting_failed = true;
        gs.log("[TARGETING] Спроба " + std::to_string(m_tgt_macro_attempts) + " — шукаємо...");
    }

    // Попередження при довгому пошуку
    const int kWarnAt = gs.cfg.targeting_tuning.long_search_warn_at;
    if (kWarnAt > 0 && m_tgt_macro_attempts > kWarnAt
        && m_tgt_macro_attempts % kWarnAt == 0) {
        std::string dots_info = minimap_dots.empty()
            ? "мінімапа порожня"
            : ("dots=" + std::to_string(minimap_dots.size()) +
               " dx=" + std::to_string(minimap_dots[0].dx));
        std::string kl_info = (gs.kl_alive_count > 0)
            ? " KL_alive=" + std::to_string(gs.kl_alive_count) : "";
        gs.log("[TARGETING] Довгий пошук ×" + std::to_string(m_tgt_macro_attempts) +
            " — " + dots_info + kl_info +
            " rot=" + std::to_string(m_tgt_minimap_rotate_count));
    }

    (void)map_ref; // map_ref доступний для майбутніх розширень
}


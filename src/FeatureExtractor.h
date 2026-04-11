#pragma once
#include <Eigen/Dense>
#include <algorithm>
#include "game_state.h"

// FeatureExtractor: перетворює GameState в числовий вектор для Q-моделі.
// Всі ознаки нормалізовані в [0, 1].
// ВАЖЛИВО: змінювати NUM_FEATURES тільки разом з LinearQModel —
// зміна інвалідує збережені weights.json.
namespace FeatureExtractor {

static constexpr int NUM_FEATURES = 10;

inline Eigen::VectorXf extract(const GameState& gs) {
    Eigen::VectorXf f = Eigen::VectorXf::Zero(NUM_FEATURES);

    // [0] HP гравця, нормалізований 0..1
    f[0] = gs.hp_valid ? std::clamp(gs.hp / 100.f, 0.f, 1.f) : 0.f;

    // [1] MP гравця, нормалізований 0..1
    f[1] = gs.hp_valid ? std::clamp(gs.mp / 100.f, 0.f, 1.f) : 0.f;

    // [2] Чи є активна ціль (binary)
    f[2] = gs.has_target ? 1.f : 0.f;

    // [3] HP поточної цілі 0..1 (0 якщо цілі немає)
    f[3] = (gs.has_target && gs.target.has_value())
           ? std::clamp(gs.target->hp / 100.f, 0.f, 1.f)
           : 0.f;

    // [4] Кількість живих мобів поблизу (з KnownList), норм до 10
    f[4] = std::clamp(gs.kl_alive_count / 10.f, 0.f, 1.f);

    // [5] Кількість точок на мінімапі, норм до 5
    f[5] = std::clamp((float)gs.minimap_dots.size() / 5.f, 0.f, 1.f);

    // [6] Час від останнього kill, норм до 60с (1.0 = 60+ с без kill)
    f[6] = std::clamp((float)gs.secs_since_last_kill / 60.f, 0.f, 1.f);

    // [7] Час від останнього бафу, норм до 900с
    f[7] = std::clamp((float)gs.secs_since_last_buff / 900.f, 0.f, 1.f);

    // [8] Персонаж мертвий (binary)
    f[8] = gs.is_dead ? 1.f : 0.f;

    // [9] Grace period активний (binary)
    f[9] = gs.in_grace ? 1.f : 0.f;

    return f;
}

} // namespace FeatureExtractor

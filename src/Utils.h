#pragma once

#include <optional>
#include <string>

#include <opencv2/opencv.hpp>

#include "Capture.h"
#include "Input.h"

#define LOCKED(ms) Locked(ms, __FILE__, __LINE__)

std::optional<cv::Mat> BitmapToImage(const ::Capture::Bitmap &bitmap);
cv::Scalar VectorToScalar(const std::vector<int> &vector, const cv::Scalar &default_val);
::Input::KeyboardKey StringToKeyboardKey(const std::string &string, ::Input::KeyboardKey default_val);
bool Locked(int ms, const std::string &file, int line);

// ── Fuzzy string matching ─────────────────────────────────────────────────────
// Відстань Левенштейна: мінімум операцій (insert/delete/replace). O(n*m) час.
int LevenshteinDistance(const std::string& a, const std::string& b);

// Подібність рядків: 0.0 (різні) .. 1.0 (ідентичні).
double StringSimilarity(const std::string& a, const std::string& b);

// Нечітка відповідність з порогом threshold (0.0..1.0, дефолт 0.85).
inline bool FuzzyMatch(const std::string& a, const std::string& b,
                       double threshold = 0.85) {
    return StringSimilarity(a, b) >= threshold;
}

#include "Utils.h"

#include <map>
#include <chrono>

std::optional<cv::Mat> BitmapToImage(const Capture::Bitmap &bitmap)
{
    if (bitmap.data == nullptr ||
        bitmap.rows <= 0 ||
        bitmap.cols <= 0 ||
        bitmap.width <= 0 ||
        bitmap.height <= 0
    ) {
        return {};
    }

    return cv::Mat(
        bitmap.rows,
        bitmap.cols,
        CV_8UC(bitmap.bits / 8),
        bitmap.data
    )({0, 0, bitmap.width, bitmap.height});
}

cv::Scalar VectorToScalar(const std::vector<int> &vector, const cv::Scalar &default_val)
{
    if (vector.size() >= 3) {
        return cv::Scalar(vector[0], vector[1], vector[2], vector.size() > 3 ? vector[3] : 255);
    }

    return default_val;
}

::Input::KeyboardKey StringToKeyboardKey(const std::string &string, ::Input::KeyboardKey default_val)
{
    if (string == "F1") return ::Input::KeyboardKey::F1;
    if (string == "F2") return ::Input::KeyboardKey::F2;
    if (string == "F3") return ::Input::KeyboardKey::F3;
    if (string == "F4") return ::Input::KeyboardKey::F4;
    if (string == "F5") return ::Input::KeyboardKey::F5;
    if (string == "F6") return ::Input::KeyboardKey::F6;
    if (string == "F7") return ::Input::KeyboardKey::F7;
    if (string == "F8") return ::Input::KeyboardKey::F8;
    if (string == "F9") return ::Input::KeyboardKey::F9;
    if (string == "F10") return ::Input::KeyboardKey::F10;
    if (string == "F11") return ::Input::KeyboardKey::F11;
    if (string == "F12") return ::Input::KeyboardKey::F12;

    if (string == "1") return ::Input::KeyboardKey::One;
    if (string == "2") return ::Input::KeyboardKey::Two;
    if (string == "3") return ::Input::KeyboardKey::Three;
    if (string == "4") return ::Input::KeyboardKey::Four;
    if (string == "5") return ::Input::KeyboardKey::Five;
    if (string == "6") return ::Input::KeyboardKey::Six;
    if (string == "7") return ::Input::KeyboardKey::Seven;
    if (string == "8") return ::Input::KeyboardKey::Eight;
    if (string == "9") return ::Input::KeyboardKey::Nine;
    if (string == "0") return ::Input::KeyboardKey::Zero;

    if (string == "Num0") return ::Input::KeyboardKey::Num0;
    if (string == "Num1") return ::Input::KeyboardKey::Num1;
    if (string == "Num2") return ::Input::KeyboardKey::Num2;
    if (string == "Num3") return ::Input::KeyboardKey::Num3;
    if (string == "Num4") return ::Input::KeyboardKey::Num4;
    if (string == "Num5") return ::Input::KeyboardKey::Num5;
    if (string == "Num6") return ::Input::KeyboardKey::Num6;
    if (string == "Num7") return ::Input::KeyboardKey::Num7;
    if (string == "Num8") return ::Input::KeyboardKey::Num8;
    if (string == "Num9") return ::Input::KeyboardKey::Num9;
    if (string == "Num*") return ::Input::KeyboardKey::NumAsterisk;
    if (string == "Num/") return ::Input::KeyboardKey::NumSlash;
    if (string == "Num+") return ::Input::KeyboardKey::NumPlus;
    if (string == "Num-") return ::Input::KeyboardKey::NumMinus;

    return default_val;
}

std::map<std::string, std::chrono::time_point<std::chrono::steady_clock>> m_locks;

bool Locked(int ms, const std::string &file, int line)
{
    const auto key = file + ":" + std::to_string(line);
    const auto lock = m_locks.find(key);
    const auto now = std::chrono::steady_clock::now();

    if (lock == m_locks.end()) {
        m_locks[key] = now + std::chrono::milliseconds(ms);
        return true;
    } else if (lock->second < now) {
        m_locks.erase(lock);
        return false;
    }

    return true;
}

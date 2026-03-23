#pragma once

#include <vector>
#include <string>
#include <optional>

class Options
{
public:
    Options(int argc, char *argv[]) : m_options{&argv[0], &argv[0] + argc} {}

    bool Has(const std::string &option) const { return Find(option, false).has_value(); }
    std::string String(const std::string &option, const std::string &default_val = "") const;
    int Int(const std::string &option, int default_val = 0) const;
    double Double(const std::string &option, double default_val = 0.0) const;
    bool Bool(const std::string &option, bool default_val = false) const;
    std::vector<std::string> StringVector(const std::string &option, const std::vector<std::string> &default_val = {}) const;
    std::vector<int> IntVector(const std::string &option, const std::vector<int> &default_val = {}) const;

private:
    const std::vector<std::string> m_options;

    std::optional<std::string> Find(const std::string &option, bool check_next = true) const;
};

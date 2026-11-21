#pragma once

#include <CLI/CLI.hpp>

struct Aec3Cli {
    CLI::App* root = nullptr;
    int run() const;
};

Aec3Cli add_aec3(CLI::App& app);

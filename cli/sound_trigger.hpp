#pragma once

#include <CLI/CLI.hpp>

struct SoundTriggerCli {
    CLI::App* root = nullptr;
    int run() const;
};

SoundTriggerCli add_sound_trigger(CLI::App& app);

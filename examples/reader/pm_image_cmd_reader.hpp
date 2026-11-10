#pragma once

#include "pm_image_cli_state.hpp"
#include <CLI/CLI.hpp>

int pm_image_cmd_reader(CLI::App& app, PmImageCliState& st);
void pm_image_register_reader(CLI::App& app, PmImageCliState& s);

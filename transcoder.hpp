#pragma once

#include <filesystem>

void transcodeToDnXHR(
    const std::filesystem::path& inputPath,
    const std::filesystem::path& outputPath);

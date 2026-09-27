#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "transcoder.hpp"

struct Config {
    std::string input_dir;
    std::string output_dir;
    int interval;
    bool delete_source;
    int max_workers;
};

static bool running = true;

static Config createConfig(const std::filesystem::path& configPath)
{
    using json = nlohmann::json;

    Config config{
        .input_dir = "input",
        .output_dir = "output",
        .interval = 3600,
        .delete_source = false,
        .max_workers = 2
    };

    const json data{
        {"input_dir", config.input_dir},
        {"output_dir", config.output_dir},
        {"interval", config.interval},
        {"delete_source", config.delete_source},
        {"max_workers", config.max_workers}
    };

    std::ofstream output(configPath);
    if (!output) {
        throw std::runtime_error("Failed to create config.json");
    }

    output << data.dump(4) << '\n';
    std::cout << "Successful create config: " << configPath << '\n';
    return config;
}

[[nodiscard]]
static Config initConfig()
{
    namespace fs = std::filesystem;
    using json = nlohmann::json;

    const fs::path configPath = fs::current_path() / "config.json";

    if (!fs::exists(configPath)) {
        return createConfig(configPath);
    }

    if (!fs::is_regular_file(configPath)) {
        throw std::runtime_error("config.json is not a regular file");
    }

    std::ifstream input(configPath);
    if (!input) {
        throw std::runtime_error("Failed to read config.json");
    }

    json data;
    input >> data;

    Config config{
        .input_dir = data.value("input_dir", "input"),
        .output_dir = data.value("output_dir", "output"),
        .interval = data.value("interval", 3600),
        .delete_source = data.value("delete_source", false),
        .max_workers = data.value("max_workers", 2)
    };

    if (config.interval <= 0) {
        throw std::runtime_error("Invalid interval");
    }
    if (config.max_workers <= 0) {
        throw std::runtime_error("Invalid max_workers");
    }

    return config;
}

static bool isVideoFile(const std::filesystem::path& path)
{
    if (!std::filesystem::is_regular_file(path)) {
        return false;
    }

    std::string extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });

    return extension == ".mp4" ||
           extension == ".mkv" ||
           extension == ".mov" ||
           extension == ".avi" ||
           extension == ".mxf";
}

class ConversionManager {
public:
    ConversionManager(const int maxWorkers, const bool deleteSource)
        : maxWorkers_(static_cast<std::size_t>(maxWorkers)),
          deleteSource_(deleteSource)
    {
    }

    ~ConversionManager()
    {
        for (auto& task : tasks_) {
            if (task->worker.joinable()) {
                task->worker.join();
            }
        }
    }

    void scan(
        const std::filesystem::path& inputDirectory,
        const std::filesystem::path& outputDirectory)
    {
        namespace fs = std::filesystem;

        fs::create_directories(inputDirectory);
        fs::create_directories(outputDirectory);
        removeFinishedTasks();

        for (const auto& entry : fs::directory_iterator(inputDirectory)) {
            if (!isVideoFile(entry.path())) {
                continue;
            }

            if (tasks_.size() >= maxWorkers_) {
                break;
            }

            const fs::path inputPath = entry.path();
            const fs::path outputPath =
                outputDirectory / (inputPath.stem().string() + ".mov");

            if (fs::exists(outputPath)) {
                log("Skip existing file: " + outputPath.string());
                continue;
            }

            const std::string key = fs::absolute(inputPath)
                                        .lexically_normal()
                                        .string();
            {
                std::lock_guard lock(stateMutex_);
                if (!processing_.insert(key).second) {
                    log("Skip already processing: " + inputPath.string());
                    continue;
                }
            }

            submit(inputPath, outputPath, key);
        }
    }

private:
    struct Task {
        std::thread worker;
        std::atomic_bool finished{false};
    };

    void submit(
        const std::filesystem::path& inputPath,
        const std::filesystem::path& outputPath,
        const std::string& key)
    {
        auto task = std::make_unique<Task>();
        Task* taskState = task.get();

        try {
            task->worker = std::thread(
                [this, inputPath, outputPath, key, taskState] {
                    process(inputPath, outputPath, key, taskState);
                });
        } catch (...) {
            std::lock_guard lock(stateMutex_);
            processing_.erase(key);
            throw;
        }

        tasks_.push_back(std::move(task));
    }

    void process(
        const std::filesystem::path& inputPath,
        const std::filesystem::path& outputPath,
        const std::string& key,
        Task* taskState) noexcept
    {
        namespace fs = std::filesystem;
        fs::path temporaryPath = outputPath;
        temporaryPath += ".part";

        try {
            log("Converting: " + inputPath.string());
            transcodeToDnXHR(inputPath, temporaryPath);

            if (!fs::is_regular_file(temporaryPath)) {
                throw std::runtime_error("temporary output was not created");
            }

            std::error_code error;
            fs::rename(temporaryPath, outputPath, error);
            if (error) {
                throw std::system_error(error, "rename temporary output");
            }

            log("Finished: " + outputPath.string());

            if (deleteSource_) {
                error.clear();
                const bool removed = fs::remove(inputPath, error);
                if (error) {
                    log("Failed to delete source file " +
                        inputPath.string() + ": " + error.message());
                } else if (removed) {
                    log("Deleted source file: " + inputPath.string());
                }
            }
        } catch (const std::exception& error) {
            log("Conversion failed for " + inputPath.string() + ": " +
                error.what());
            std::error_code removeError;
            fs::remove(temporaryPath, removeError);
        }

        {
            std::lock_guard lock(stateMutex_);
            processing_.erase(key);
        }
        taskState->finished.store(true, std::memory_order_release);
    }

    void removeFinishedTasks()
    {
        auto iterator = tasks_.begin();
        while (iterator != tasks_.end()) {
            if ((*iterator)->finished.load(std::memory_order_acquire)) {
                if ((*iterator)->worker.joinable()) {
                    (*iterator)->worker.join();
                }
                iterator = tasks_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    void log(const std::string& message)
    {
        std::lock_guard lock(logMutex_);
        std::cout << message << '\n';
    }

    const std::size_t maxWorkers_;
    const bool deleteSource_;
    std::mutex stateMutex_;
    std::mutex logMutex_;
    std::unordered_set<std::string> processing_;
    std::vector<std::unique_ptr<Task>> tasks_;
};

int main()
{
    try {
        const Config config = initConfig();
        std::cout << "Working directory: "
                  << std::filesystem::current_path() << '\n';

        ConversionManager manager(config.max_workers, config.delete_source);
        int count = 0;

        while (running) {
            if (count <= 0) {
                manager.scan(config.input_dir, config.output_dir);
                count = config.interval;
            } else {
                --count;
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}

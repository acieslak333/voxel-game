#include "core/App.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

// Entry point. Everything interesting lives in vg::App; main() just parses a
// couple of optional flags, runs it, and turns any uncaught exception into a
// clean non-zero exit + error message.
//
// Flags:
//   --frames N        Run N frames then exit (headless smoke-testing / CI).
//   --screenshot PATH Write the final frame to PATH as a PNG, then exit.
int main(int argc, char** argv) {
    long maxFrames = -1; // run until the window is closed
    std::string screenshotPath;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            maxFrames = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshotPath = argv[++i];
            if (maxFrames < 0) {
                maxFrames = 2; // render a couple of frames then capture
            }
        } else {
            std::cerr << "Unknown argument: " << argv[i] << '\n';
            return EXIT_FAILURE;
        }
    }

    try {
        vg::App app;
        app.run(maxFrames, screenshotPath);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

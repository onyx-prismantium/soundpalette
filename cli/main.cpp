#include <cstdio>

#include "soundpalette/version.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    std::fprintf(stderr, "soundpalette %s: no subcommand implemented yet (M0 scaffold)\n",
                 sp::kVersionString);
    return 2;
}

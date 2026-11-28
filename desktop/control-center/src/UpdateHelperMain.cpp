#include "UpdateHelperCore.h"

#include <iostream>

int main(int argc, char **argv)
{
    // Production binary uses strictly hardcoded system paths and never reads
    // binary or lock paths from caller environment variables.
    const zethropol::update_helper::HelperConfig productionConfig{};
    return zethropol::update_helper::runHelper(
        argc, argv, productionConfig, std::cout, std::cerr);
}

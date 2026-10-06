#include "../version.hpp"
#include "../control_cli.hpp"
int main(int argc, char** argv) {
    if (tuntom::print_version_if_requested(argc, argv, "tuntomctl")) return 0;
    return tuntom_control_main(argc, argv);
}

#include "ephemeris.h"

int main(int argc, char** argv) {
    try {
        return eph::run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "ephemeris: " << e.what() << "\n";
        return 1;
    }
}

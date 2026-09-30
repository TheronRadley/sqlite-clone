// test_main.cpp — entry point for the test binary.
#include <cstring>
#include <string>

#include "test_util.hpp"

int main(int argc, char** argv) {
    std::string filter = argc > 1 ? argv[1] : "";
    return test::run_all(filter);
}

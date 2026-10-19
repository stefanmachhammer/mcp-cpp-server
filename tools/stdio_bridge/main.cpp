#include <cstdio>
#include <string>

#include "mcp/stdio_bridge.hpp"

int main(int argc, char** argv) {
    mcp::BridgeOptions opt;
    std::string err;
    if (!mcp::parse_bridge_args(argc, argv, opt, &err)) {
        std::fprintf(stderr, "mcp-stdio-bridge %s", err.c_str());
        return err.rfind("options:", 0) == 0 ? 0 : 2;
    }
    return mcp::run_stdio_bridge(opt);
}

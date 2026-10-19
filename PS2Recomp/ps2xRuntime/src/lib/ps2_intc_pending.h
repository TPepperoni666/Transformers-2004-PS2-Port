#pragma once
// Hardware causes dispatched on the EE executor; implementation in ps2_memory.cpp.
#include <cstdint>
#include <vector>
namespace ps2intc {
    void queue(uint32_t cause);
    std::vector<uint32_t> consume();
}

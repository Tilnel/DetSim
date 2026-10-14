#ifndef DETSIM_SSTATE_READER_H
#define DETSIM_SSTATE_READER_H
#include "core/state/state.h"
#include "core/state/sysstate_store.h"
#include "cereal/archives/binary.hpp"
#include <sstream>
#include <stdexcept>

inline sys_state decode_metadata(hash_type hash, const std::vector<uint8_t> &data) {
    sys_state state;
    std::istringstream input(std::string(data.begin(), data.end()));
    { cereal::BinaryInputArchive archive(input); archive(state); }
    if (input.peek() != std::char_traits<char>::eof() || state.identity_hash() != hash)
        throw std::runtime_error("metadata identity/layout mismatch (check NP)");
    for (hash_type child : state.ts_hash)
        if (!child) throw std::runtime_error("zero tracee-state hash");
    state.ss_hash = hash;
    return state;
}
#endif

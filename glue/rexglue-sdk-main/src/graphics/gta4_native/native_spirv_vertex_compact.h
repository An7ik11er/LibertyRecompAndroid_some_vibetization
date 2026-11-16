#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rex::graphics::gta4_native {

inline bool CompactSpirvVertexInputs(
    std::vector<uint32_t>& spirv, uint32_t max_locations,
    std::unordered_map<uint32_t, uint32_t>& new_to_orig) {
  new_to_orig.clear();
  if (spirv.size() < 5 || spirv[0] != 0x07230203u || !spirv[3]) {
    return false;
  }
  const uint32_t id_bound = spirv[3];

  std::unordered_set<uint32_t> input_variables;
  // Step 1: Find all OpVariable with StorageClass::Input (1)
  size_t cursor = 5;
  while (cursor < spirv.size()) {
    const uint32_t count = spirv[cursor] >> 16;
    const uint16_t opcode = uint16_t(spirv[cursor] & 0xFFFFu);
    if (!count || cursor + count > spirv.size()) return false;
    const uint32_t* words = spirv.data() + cursor;
    if (opcode == 59 /* OpVariable */ && count >= 4 && words[2] < id_bound && words[3] == 1 /* Input */) {
      input_variables.insert(words[2]);
    }
    cursor += count;
  }

  if (input_variables.empty()) return true;

  // Step 2: Find all locations for input variables
  std::vector<uint32_t> locations;
  bool needs_compacting = false;
  cursor = 5;
  while (cursor < spirv.size()) {
    const uint32_t count = spirv[cursor] >> 16;
    const uint16_t opcode = uint16_t(spirv[cursor] & 0xFFFFu);
    if (!count || cursor + count > spirv.size()) return false;
    const uint32_t* words = spirv.data() + cursor;
    if (opcode == 71 /* OpDecorate */ && count >= 4 && words[2] == 30 /* Location */) {
      if (input_variables.count(words[1])) {
        const uint32_t loc = words[3];
        locations.push_back(loc);
        if (loc >= max_locations) {
          needs_compacting = true;
        }
      }
    }
    cursor += count;
  }

  if (!needs_compacting) {
    for (uint32_t loc : locations) {
      new_to_orig[loc] = loc;
    }
    return true;
  }

  // Sort and remove duplicates
  std::sort(locations.begin(), locations.end());
  locations.erase(std::unique(locations.begin(), locations.end()), locations.end());

  std::unordered_map<uint32_t, uint32_t> orig_to_new;
  for (uint32_t i = 0; i < locations.size(); ++i) {
    orig_to_new[locations[i]] = i;
    new_to_orig[i] = locations[i];
  }

  // Step 3: Rewrite the locations in-place in the SPIR-V words
  cursor = 5;
  while (cursor < spirv.size()) {
    const uint32_t count = spirv[cursor] >> 16;
    const uint16_t opcode = uint16_t(spirv[cursor] & 0xFFFFu);
    if (!count || cursor + count > spirv.size()) return false;
    uint32_t* words = spirv.data() + cursor;
    if (opcode == 71 /* OpDecorate */ && count >= 4 && words[2] == 30 /* Location */) {
      if (input_variables.count(words[1])) {
        const auto it = orig_to_new.find(words[3]);
        if (it != orig_to_new.end()) {
          words[3] = it->second;
        }
      }
    }
    cursor += count;
  }

  return true;
}

}  // namespace rex::graphics::gta4_native

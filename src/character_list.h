#pragma once
#include <string>
#include <vector>
#include <cstring>
#include "player_character.h"

class CharacterList {
  std::vector<std::string> names;
public:
  void Set(const std::string& text) {
    names.clear();
    size_t start = 0;
    while (start < text.size()) {
      size_t end = text.find(',', start);
      if (end == std::string::npos) end = text.size();
      size_t first = text.find_first_not_of(" \t", start);
      size_t last = text.find_last_not_of(" \t", end - 1);
      if (first < end && last != std::string::npos && last >= first)
        names.push_back(text.substr(first, last - first + 1));
      start = end + 1;
    }
  }
  bool Contains(int character) const {
    const char* name = character ? playercharacter::ResourceName(character) : nullptr;
    if (!name) return false;
    for (const auto& item : names) if (!_stricmp(item.c_str(), name)) return true;
    return false;
  }
};

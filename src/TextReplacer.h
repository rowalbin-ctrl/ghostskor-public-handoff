#pragma once
#include <map>
#include <string>

class TextReplacer {
public:
  static bool LoadTranslations(const std::string &filename);
};

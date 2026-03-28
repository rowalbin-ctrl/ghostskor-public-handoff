#include "TextReplacer.h"
#include "TranslationStore.h"
#include "Utils.h"

bool TextReplacer::LoadTranslations(const std::string &filename) {
  return TranslationStore::EnsureLoadedFromPath(filename);
}

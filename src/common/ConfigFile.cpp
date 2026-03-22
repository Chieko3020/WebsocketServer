#include "common/ConfigFile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace config_file {
namespace {

void toLowerAscii(std::string* s) {
  for (char& c : *s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
}

std::string trim(std::string s) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

bool parseBool(const std::string& v, bool* out) {
  std::string t = v;
  toLowerAscii(&t);
  if (t == "1" || t == "true" || t == "yes" || t == "on") {
    *out = true;
    return true;
  }
  if (t == "0" || t == "false" || t == "no" || t == "off") {
    *out = false;
    return true;
  }
  return false;
}

}  // namespace

bool load(const std::string& path, std::unordered_map<std::string, std::string>* out, std::string* err) {
  out->clear();
  std::ifstream ifs(path.c_str());
  if (!ifs) {
    if (err) *err = "cannot open file";
    return false;
  }
  std::string line;
  std::size_t lineNo = 0;
  while (std::getline(ifs, line)) {
    ++lineNo;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;

    std::size_t eq = line.find('=');
    if (eq == std::string::npos) {
      if (err) {
        std::ostringstream oss;
        oss << path << ":" << lineNo << ": missing '='";
        *err = oss.str();
      }
      return false;
    }
    std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    if (!val.empty() && val.front() == '"' && val.back() == '"' && val.size() >= 2) {
      val = val.substr(1, val.size() - 2);
    }

    if (key.empty()) {
      if (err) {
        std::ostringstream oss;
        oss << path << ":" << lineNo << ": empty key";
        *err = oss.str();
      }
      return false;
    }
    toLowerAscii(&key);
    (*out)[key] = val;
  }
  return true;
}

std::string getString(const std::unordered_map<std::string, std::string>& m, const std::string& key,
                      const std::string& defaultVal) {
  std::string k = key;
  toLowerAscii(&k);
  auto it = m.find(k);
  if (it == m.end()) return defaultVal;
  return it->second;
}

int getInt(const std::unordered_map<std::string, std::string>& m, const std::string& key, int defaultVal) {
  std::string s = getString(m, key, "");
  if (s.empty()) return defaultVal;
  char* end = nullptr;
  long v = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str()) return defaultVal;
  return static_cast<int>(v);
}

long getLong(const std::unordered_map<std::string, std::string>& m, const std::string& key, long defaultVal) {
  std::string s = getString(m, key, "");
  if (s.empty()) return defaultVal;
  char* end = nullptr;
  long v = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str()) return defaultVal;
  return v;
}

bool getBool(const std::unordered_map<std::string, std::string>& m, const std::string& key, bool defaultVal) {
  std::string s = getString(m, key, "");
  if (s.empty()) return defaultVal;
  bool b = false;
  if (parseBool(s, &b)) return b;
  return defaultVal;
}

}  // namespace config_file

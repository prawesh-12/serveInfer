#include "hardwareReport.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace {

std::string jsonEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char ch : input) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

bool extractString(const std::string& json, const std::string& key, std::string& out) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) {
    return false;
  }
  out = match[1].str();
  return true;
}

bool extractUnsigned(const std::string& json, const std::string& key, std::size_t& out) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*([0-9]+)");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) {
    return false;
  }
  try {
    out = static_cast<std::size_t>(std::stoull(match[1].str()));
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

bool extractBool(const std::string& json, const std::string& key, bool& out) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*(true|false)");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) {
    return false;
  }
  out = match[1] == "true";
  return true;
}

// Split before the per-key extractors run, or every device reads device 0's fields.
std::vector<std::string> splitObjects(const std::string& array) {
  std::vector<std::string> out;
  int depth = 0;
  bool inString = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i < array.size(); ++i) {
    const char ch = array[i];
    if (inString) {
      if (ch == '\\') {
        ++i;
      } else if (ch == '"') {
        inString = false;
      }
      continue;
    }
    if (ch == '"') {
      inString = true;
    } else if (ch == '{') {
      if (depth == 0) {
        start = i;
      }
      ++depth;
    } else if (ch == '}') {
      --depth;
      if (depth == 0) {
        out.push_back(array.substr(start, i - start + 1));
      }
    }
  }
  return out;
}

bool sliceGpuArray(const std::string& json, std::string& out) {
  const std::size_t keyAt = json.find("\"gpus\"");
  if (keyAt == std::string::npos) {
    return false;
  }
  const std::size_t open = json.find('[', keyAt);
  if (open == std::string::npos) {
    return false;
  }
  int depth = 0;
  bool inString = false;
  for (std::size_t i = open; i < json.size(); ++i) {
    const char ch = json[i];
    if (inString) {
      if (ch == '\\') {
        ++i;
      } else if (ch == '"') {
        inString = false;
      }
      continue;
    }
    if (ch == '"') {
      inString = true;
    } else if (ch == '[') {
      ++depth;
    } else if (ch == ']') {
      --depth;
      if (depth == 0) {
        out = json.substr(open, i - open + 1);
        return true;
      }
    }
  }
  return false;
}

}  // namespace

std::size_t mbToBytes(long long mb) {
  if (mb <= 0) {
    return 0;
  }
  return static_cast<std::size_t>(mb) * 1024ull * 1024ull;
}

long long bytesToMb(std::size_t bytes) {
  return static_cast<long long>(bytes / (1024ull * 1024ull));
}

std::string hardwareReportToJson(const HardwareReport& report) {
  std::string out = "{\"probeOk\":";
  out += report.probeOk ? "true" : "false";
  out += ",\"note\":\"" + jsonEscape(report.note) + "\",\"ramTotalBytes\":" +
         std::to_string(report.ram.totalBytes) + ",\"ramAvailableBytes\":" +
         std::to_string(report.ram.availableBytes) + ",\"gpus\":[";
  for (std::size_t i = 0; i < report.gpus.size(); ++i) {
    const GpuDevice& gpu = report.gpus[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"name\":\"" + jsonEscape(gpu.name) + "\",\"description\":\"" +
           jsonEscape(gpu.description) + "\",\"freeBytes\":" + std::to_string(gpu.freeBytes) +
           ",\"totalBytes\":" + std::to_string(gpu.totalBytes) + "}";
  }
  out += "]}";
  return out;
}

bool parseHardwareReport(const std::string& json, HardwareReport& out) {
  HardwareReport parsed;
  if (!extractBool(json, "probeOk", parsed.probeOk)) {
    return false;
  }
  std::string gpuArray;
  if (!sliceGpuArray(json, gpuArray)) {
    return false;
  }
  extractString(json, "note", parsed.note);
  extractUnsigned(json, "ramTotalBytes", parsed.ram.totalBytes);
  extractUnsigned(json, "ramAvailableBytes", parsed.ram.availableBytes);

  for (const std::string& object : splitObjects(gpuArray)) {
    GpuDevice gpu;
    extractString(object, "name", gpu.name);
    extractString(object, "description", gpu.description);
    if (!extractUnsigned(object, "freeBytes", gpu.freeBytes) ||
        !extractUnsigned(object, "totalBytes", gpu.totalBytes)) {
      // Worse than no entry: a zero here would be planned against as free VRAM.
      return false;
    }
    parsed.gpus.push_back(gpu);
  }

  out = std::move(parsed);
  return true;
}

HostMemory readHostMemory(const std::string& meminfoPath) {
  HostMemory memory;
  std::ifstream in(meminfoPath);
  if (!in.is_open()) {
    return memory;
  }

  // Every value in /proc/meminfo is stated in kB, whatever the field.
  const std::regex pattern("^(MemTotal|MemAvailable):\\s+([0-9]+)\\s+kB");
  std::string line;
  while (std::getline(in, line)) {
    std::smatch match;
    if (!std::regex_search(line, match, pattern)) {
      continue;
    }
    std::size_t bytes = 0;
    try {
      bytes = static_cast<std::size_t>(std::stoull(match[2].str())) * 1024ull;
    } catch (const std::exception&) {
      continue;
    }
    if (match[1] == "MemTotal") {
      memory.totalBytes = bytes;
    } else {
      memory.availableBytes = bytes;
    }
  }
  return memory;
}

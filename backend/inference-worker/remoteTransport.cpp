#include "remoteTransport.h"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <vector>

namespace {

std::string envOr(const char* name, const char* fallback) {
  const char* value = std::getenv(name);
  return value == nullptr || value[0] == '\0' ? std::string(fallback) : std::string(value);
}

long envNumber(const char* name, long fallback) {
  const std::string raw = envOr(name, "");
  if (raw.empty()) {
    return fallback;
  }
  char* end = nullptr;
  errno = 0;
  const long value = std::strtol(raw.c_str(), &end, 10);
  if (end == raw.c_str() || errno != 0 || value <= 0) {
    return fallback;
  }
  return value;
}

std::string jsonEscape(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 16);
  for (const char ch : input) {
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
        if (static_cast<unsigned char>(ch) < 0x20) {
          char escaped[7];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(ch));
          out += escaped;
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

void appendCodePoint(std::string& out, unsigned long codePoint) {
  if (codePoint < 0x80) {
    out.push_back(static_cast<char>(codePoint));
  } else if (codePoint < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else if (codePoint < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  }
}

bool readHex4(const std::string& value, std::size_t at, unsigned long& out) {
  if (at + 4 > value.size()) {
    return false;
  }
  out = 0;
  for (std::size_t i = at; i < at + 4; ++i) {
    const char ch = value[i];
    out <<= 4;
    if (ch >= '0' && ch <= '9') {
      out |= static_cast<unsigned long>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      out |= static_cast<unsigned long>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      out |= static_cast<unsigned long>(ch - 'A' + 10);
    } else {
      return false;
    }
  }
  return true;
}

std::string jsonUnescape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 >= value.size()) {
      out.push_back(value[i]);
      continue;
    }
    ++i;
    switch (value[i]) {
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'u': {
        unsigned long codePoint = 0;
        if (!readHex4(value, i + 1, codePoint)) {
          out.push_back(value[i]);
          break;
        }
        i += 4;
        // JSON.stringify emits astral characters as a surrogate pair, so rejoin one here.
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF && i + 6 < value.size() &&
            value[i + 1] == '\\' && value[i + 2] == 'u') {
          unsigned long low = 0;
          if (readHex4(value, i + 3, low) && low >= 0xDC00 && low <= 0xDFFF) {
            codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
            i += 6;
          }
        }
        appendCodePoint(out, codePoint);
        break;
      }
      default:
        out.push_back(value[i]);
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
  out = jsonUnescape(match[1].str());
  return true;
}

bool extractLong(const std::string& json, const std::string& key, long& out) {
  const std::regex pattern("\"" + key + "\"\\s*:\\s*(-?[0-9]+)");
  std::smatch match;
  if (!std::regex_search(json, match, pattern)) {
    return false;
  }
  out = std::strtol(match[1].str().c_str(), nullptr, 10);
  return true;
}

struct ChildResult {
  bool spawned = false;
  bool timedOut = false;
  int exitStatus = -1;
  std::string output;
  std::string error;
};

// Same shape as Supervisor::runHardwareProbeChild. Writing is polled too, so a child
// that never reads its stdin cannot wedge the worker on a large prompt.
ChildResult runTransportChild(const std::string& binary, const std::string& script,
                              const std::string& input, int timeoutMs) {
  ChildResult result;

  int toChild[2] = {-1, -1};
  int fromChild[2] = {-1, -1};
  if (pipe(toChild) != 0) {
    result.error = std::string("pipe for remote transport failed: ") + std::strerror(errno);
    return result;
  }
  if (pipe(fromChild) != 0) {
    result.error = std::string("pipe for remote transport failed: ") + std::strerror(errno);
    close(toChild[0]);
    close(toChild[1]);
    return result;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    result.error = std::string("fork for remote transport failed: ") + std::strerror(errno);
    close(toChild[0]);
    close(toChild[1]);
    close(fromChild[0]);
    close(fromChild[1]);
    return result;
  }

  if (pid == 0) {
    close(toChild[1]);
    close(fromChild[0]);
    dup2(toChild[0], STDIN_FILENO);
    dup2(fromChild[1], STDOUT_FILENO);
    close(toChild[0]);
    close(fromChild[1]);
    char* argv[] = {const_cast<char*>(binary.c_str()), const_cast<char*>(script.c_str()), nullptr};
    execvp(argv[0], argv);
    _exit(127);
  }

  close(toChild[0]);
  close(fromChild[1]);
  result.spawned = true;

  // A child that dies before reading its stdin would otherwise take the worker down with it.
  struct sigaction ignorePipe {};
  struct sigaction previousPipe {};
  ignorePipe.sa_handler = SIG_IGN;
  sigemptyset(&ignorePipe.sa_mask);
  sigaction(SIGPIPE, &ignorePipe, &previousPipe);

  std::size_t sent = 0;
  bool writeOpen = true;
  bool readOpen = true;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
  while (readOpen) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      result.timedOut = true;
      break;
    }

    pollfd watched[2]{};
    int count = 0;
    int writeSlot = -1;
    int readSlot = -1;
    if (writeOpen) {
      watched[count].fd = toChild[1];
      watched[count].events = POLLOUT;
      writeSlot = count++;
    }
    watched[count].fd = fromChild[0];
    watched[count].events = POLLIN;
    readSlot = count++;

    const int ready = poll(watched, static_cast<nfds_t>(count), static_cast<int>(remaining.count()));
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      result.error = std::string("poll on remote transport failed: ") + std::strerror(errno);
      break;
    }
    if (ready == 0) {
      result.timedOut = true;
      break;
    }

    if (writeSlot >= 0 && watched[writeSlot].revents != 0) {
      if ((watched[writeSlot].revents & POLLOUT) != 0) {
        const ssize_t written = write(toChild[1], input.data() + sent, input.size() - sent);
        if (written > 0) {
          sent += static_cast<std::size_t>(written);
        } else if (written < 0 && errno != EINTR && errno != EAGAIN) {
          writeOpen = false;
        }
      } else {
        writeOpen = false;
      }
      if (sent >= input.size()) {
        writeOpen = false;
      }
      if (!writeOpen) {
        close(toChild[1]);
        toChild[1] = -1;
      }
    }

    if (watched[readSlot].revents != 0) {
      char buffer[4096];
      const ssize_t got = read(fromChild[0], buffer, sizeof(buffer));
      if (got > 0) {
        result.output.append(buffer, static_cast<std::size_t>(got));
      } else if (got == 0) {
        readOpen = false;
      } else if (errno != EINTR && errno != EAGAIN) {
        readOpen = false;
      }
    }
  }

  sigaction(SIGPIPE, &previousPipe, nullptr);
  if (toChild[1] >= 0) {
    close(toChild[1]);
  }
  close(fromChild[0]);

  if (result.timedOut) {
    kill(pid, SIGKILL);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  result.exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

}  // namespace

std::string buildRemoteRequestJson(const RemoteRequest& request) {
  return "{\"prompt\":\"" + jsonEscape(request.prompt) + "\",\"endpoint\":\"" +
         jsonEscape(request.endpoint) + "\"}\n";
}

bool parseRemoteResponseJson(const std::string& output, RemoteResponse& out) {
  std::size_t end = output.find_last_not_of("\r\n \t");
  if (end == std::string::npos) {
    return false;
  }
  const std::size_t start = output.find_last_of('\n', end);
  const std::string line =
      output.substr(start == std::string::npos ? 0 : start + 1, std::string::npos);

  long status = 0;
  if (!extractLong(line, "status", status)) {
    return false;
  }
  out.status = status;
  out.text.clear();
  out.error.clear();
  extractString(line, "text", out.text);
  extractString(line, "error", out.error);
  return true;
}

// Without a credential this returns nothing at all, so the tier refuses by name
// instead of failing mid-request.
RemoteTransport makeRemoteTransport() {
  if (envOr("EDGE_SARVAM_API_KEY", "").empty()) {
    return {};
  }

  const std::string binary = envOr("EDGE_NODE_BIN", "node");
  const std::string script =
      envOr("EDGE_REMOTE_TRANSPORT_SCRIPT", "./backend/remote/sarvamTransport.js");
  const int callTimeoutMs = static_cast<int>(envNumber("EDGE_REMOTE_TIMEOUT_MS", 30000));
  // The child owns the same budget, so it gets a grace window to report its own timeout first.
  const int childTimeoutMs = callTimeoutMs + 1000;

  return [binary, script, childTimeoutMs](const RemoteRequest& request) {
    RemoteResponse response;
    const ChildResult child =
        runTransportChild(binary, script, buildRemoteRequestJson(request), childTimeoutMs);

    if (!child.spawned) {
      response.error = child.error;
      return response;
    }
    if (child.timedOut) {
      response.status = 504;
      response.error = "remote transport child exceeded " + std::to_string(childTimeoutMs) +
                       "ms and was killed";
      return response;
    }
    if (!parseRemoteResponseJson(child.output, response)) {
      response.status = 0;
      response.error =
          child.exitStatus == 0
              ? "remote transport child emitted no readable response line"
              : "remote transport child exited " + std::to_string(child.exitStatus) +
                    " without a response line";
      return response;
    }
    return response;
  };
}

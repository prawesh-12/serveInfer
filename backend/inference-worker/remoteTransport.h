#pragma once

#include <string>

#include "inferenceBackend.h"

// The one-line JSON contract with backend/remote/sarvamTransport.js. Declared here
// so the wire format is testable without spawning anything.
std::string buildRemoteRequestJson(const RemoteRequest& request);

// Reads the child's last response line. False means nothing usable came back.
bool parseRemoteResponseJson(const std::string& output, RemoteResponse& out);

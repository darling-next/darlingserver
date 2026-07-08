#pragma once

#include <string>

namespace DarlingServer::TestDiagnostics {

bool enabled();
bool consumeFault(const char* name);
void traceLine(const std::string& line);

}

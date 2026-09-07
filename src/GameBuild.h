#pragma once

// This maintenance candidate supports the unmodified August 2026 Steam SP EXE.
// Every engine address is meaningful only after the file and unpacked code pass.
namespace GameBuild {
bool IsSupported();
bool RuntimeReady();
void WriteStatus(const char* status);
}

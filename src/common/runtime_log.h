#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>

namespace pulse::diagnostics::runtime {
// Only fixed event/field identifiers and numeric values are accepted. Callers
// must never encode paths, search terms, URLs or document content as identifiers.
struct Field { const char* name; uint64_t value; };
struct Options {
    uint64_t rotate_bytes = 2 * 1024 * 1024;
    uint32_t heartbeat_ms = 60000;
};
bool Initialize(const std::wstring& data_root, const char* component, Options options = {}) noexcept;
bool Enabled() noexcept;
void Event(const char* name, std::initializer_list<Field> fields = {}) noexcept;
uint64_t NextId() noexcept;
void Shutdown() noexcept;
}

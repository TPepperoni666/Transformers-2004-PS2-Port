// The SDK signature payload is deliberately absent from source distributions.
// Setup supplies a hash-verified upstream header in the private work directory.
#ifdef PS2RECOMP_SCE_DATABASE_HEADER
#include PS2RECOMP_SCE_DATABASE_HEADER
#else
#include <string_view>
namespace ps2recomp::sce_symbol_database {
inline constexpr std::string_view kSymbolsJsonChunks[] = {"{}"};
inline constexpr std::string_view kTreeJsonChunks[] = {"{}"};
}
#endif

/* The runtime's texture hash (runtime/pgraph.h hash_bytes) as a DLL for tools/texextract.py, so the keys it computes
 * offline are the ones the renderer computes. Built by texextract.py. */
#include "pgraph.h"

__declspec(dllexport) uint64_t texhash(const void *p, size_t n, uint64_t seed) { return hash_bytes(p, n, seed); }

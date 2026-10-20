// Compile-only checks for `acquire_owner({})`, which must not reach the legacy bool overload.
// owner_braced_check.cmake builds this once per CASE_* and expects the refusals to fail and
// CASE_COMPILING (every spelling that must still work) to build.
#include "completion/api.h"

namespace j = kronuz::journal;

template <class T> void probe(T& io, bool flag) {
#if defined(CASE_COMPILING)
	(void)io.acquire_owner(true);
	(void)io.acquire_owner(false);
	(void)io.acquire_owner(flag);
	(void)io.acquire_owner(0);
	(void)io.acquire_owner(j::OwnerLockOptions{});
	(void)io.acquire_owner({j::OwnerLockMode::file});
	(void)io.acquire_owner({.mode = j::OwnerLockMode::directory});
#else
	(void)flag;
	(void)io.acquire_owner({});
#endif
}

#if defined(CASE_IO)
template void probe<j::IO>(j::IO&, bool);
#elif defined(CASE_POSIX)
template void probe<j::PosixIO>(j::PosixIO&, bool);
#else
template void probe<j::IO>(j::IO&, bool);
template void probe<j::PosixIO>(j::PosixIO&, bool);
#endif

int main() {}

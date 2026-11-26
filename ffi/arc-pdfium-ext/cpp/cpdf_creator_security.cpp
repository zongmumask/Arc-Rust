// Arc PDF — CPDF_Creator encryption override methods.
//
// We deliberately do NOT compile the full Core cpdf_creator.cpp here, because
// that causes duplicate symbol conflicts with the version already compiled
// into the vendored Pdfium.xcframework. Instead we provide ONLY the one new
// method SetNewSecurityHandler() that Arc PDF needs. RemoveSecurity() is
// already implemented by Core (we only moved its declaration from private
// to public in the modified header).
//
// Strategy for accessing private members:
// 1. First include EVERY header that cpdf_creator.h would transitively pull
//    in (both STL and Core) WITHOUT the `#define private public` hack.
//    This primes their include guards so they won't be re-included while
//    the hack is active.
// 2. Then `#define private public`, include cpdf_creator.h (which now skips
//    all its transitive includes thanks to step 1), `#undef private`.
// 3. Now we can write methods that access Creator's private members.

// ---- Step 1: Prime include guards (no hack active) ----
#include <stdint.h>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "core/fxcrt/fx_stream.h"
#include "core/fxcrt/mask.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"

// ---- Step 2: Now safely enable the hack and include cpdf_creator.h ----
#define private public
#include "cpdf_creator.h"
#undef private

// ---- Step 3: Additional headers needed for our implementation ----
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_security_handler.h"

void CPDF_Creator::SetNewSecurityHandler(
    RetainPtr<CPDF_Dictionary> encrypt_dict,
    RetainPtr<CPDF_SecurityHandler> handler) {
  encrypt_dict_ = std::move(encrypt_dict);
  new_encrypt_dict_.Reset();  // Clear any previous internal clone
  security_handler_ = std::move(handler);
  security_changed_ = true;
}

#pragma once

// Dieser Header lebt nur im Test-Bereich und kapselt die Catch2-Spezialisierung,
// damit Produktions-Code nicht von Catch2-Interna abhängt.

// Catch2 v3 (vcpkg): Haupt-API steckt in catch2/*, aber catch_all.hpp zieht alles Notwendige.
// Catch2 v2: <catch.hpp> ist die einzige zentrale Header-Datei.

#if __has_include(<catch2/catch_all.hpp>)
  #include <catch2/catch_all.hpp>
#elif __has_include(<catch.hpp>)
  #include <catch.hpp>
#else
  // Falls kein Catch verfügbar ist, diesen Header einfach leise überspringen.
  #define GRINPP_NO_CATCH
#endif

#ifndef GRINPP_NO_CATCH
  #include <string>
  #include <Crypto/Models/BigInteger.h>

  namespace Catch {
    template <size_t NUM_BYTES, class ALLOC>
    struct StringMaker<CBigInteger<NUM_BYTES, ALLOC>> {
      static std::string convert(const CBigInteger<NUM_BYTES, ALLOC>& v) {
        return v.ToHex();
      }
    };
  } // namespace Catch
#endif
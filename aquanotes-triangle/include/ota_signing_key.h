#pragma once

// Public OTA verification key. The matching private key must remain offline
// on the controlled release workstation and must never be flashed or deployed.
static constexpr const char OTA_SIGNING_PUBLIC_KEY[] = R"KEY(-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEzJk3Jdxn775AeMNgl63cF69UZAMz
P/GxOUMZNaKwIYslMp+WvbeELeTHffNosK1AJiC5DsBKjbYLN4eZALJXDQ==
-----END PUBLIC KEY-----
)KEY";

static constexpr const char OTA_SIGNING_KEY_ID[] = "b7aa2aa6e416c024";

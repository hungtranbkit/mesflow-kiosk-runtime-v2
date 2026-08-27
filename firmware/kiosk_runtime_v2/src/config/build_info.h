#pragma once

// Own version line, independent of the legacy kiosk firmware's numbering.
// Keep in sync with the top-level VERSION file. (No automated build-time
// read of VERSION into this macro yet -- Phase 1 scope; bump both by hand.)
#define KIOSK_RUNTIME_VERSION "0.15.0"

// Filled at build time via arduino-cli --build-property; falls back to a
// visible placeholder if not injected, so a manual IDE build still shows
// something rather than silently going stale.
#ifndef KIOSK_BUILD_ID
#define KIOSK_BUILD_ID "unknown-build"
#endif

// --- Build profile (Phase 1, §30) ---
// Exactly one of these must be defined, injected via
// scripts/build-dev.sh / scripts/build-prod.sh (compiler.cpp.extra_flags).
// Never decided at runtime -- a profile is a property of the compiled
// artifact, not something a flag could flip after flashing.
#if !defined(MESFLOW_PROFILE_DEV) && !defined(MESFLOW_PROFILE_PROD)
// No profile injected (e.g. building straight from an IDE): default to DEV,
// since this entire project is currently DO-NOT-DEPLOY-TO-PRODUCTION
// (README) -- but say so loudly rather than silently assuming.
#define MESFLOW_PROFILE_DEV 1
#warning "No MESFLOW_PROFILE_* defined -- defaulting to MESFLOW_PROFILE_DEV. Use scripts/build-dev.sh or scripts/build-prod.sh."
#endif

#if defined(MESFLOW_PROFILE_DEV) && defined(MESFLOW_PROFILE_PROD)
#error "Both MESFLOW_PROFILE_DEV and MESFLOW_PROFILE_PROD are defined -- exactly one must be set."
#endif

#if defined(MESFLOW_PROFILE_PROD)
#define MESFLOW_PROFILE_NAME "PRODUCTION"
#else
#define MESFLOW_PROFILE_NAME "DEV"
#endif

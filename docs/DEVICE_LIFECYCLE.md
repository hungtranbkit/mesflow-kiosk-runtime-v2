# Device Lifecycle & Migration

**Provisioning states are now implemented and live-verified — see
docs/PROVISIONING.md for the actual DEV-path mechanism, states'
runtime-behavior effects, and hardware evidence.** Everything else on this
page (backend registration, certificates, capability negotiation,
compatibility matrix, legacy migration) is still Phase 1/5 design, not
implemented.

## Provisioning states

```text
UNPROVISIONED -> PROVISIONING -> ACTIVE -> SUSPENDED -> REVOKED
```

## Provisioning flow (target)

```text
flash trusted base firmware
  -> create/inject device identity
  -> backend register
  -> assign site/station/profile
  -> issue device certificate
  -> first mTLS bootstrap
  -> ACTIVE
```

Must be documented before implementation: certificate rotation, certificate
expiry, device stolen, device replacement, device retirement, factory
reset, re-provision. No shared fleet token, ever.

## Desired / reported config

Backend-managed desired config: station, profile, brightness, volume,
workflow, UI bundle, timeouts, retry policy, feature flags, enabled/disabled.
Device reports current values. Mismatch = `OUT_OF_SYNC`.

## Capability negotiation

Bootstrap reports scanner/keypad/touch/speaker presence, PSRAM, display
size, runtime version, supported schema versions, offline features
supported. Backend must never send a UI feature the device didn't declare
support for.

## Compatibility matrix (example shape)

```text
runtime 6.2 -> schema 1-3
runtime 6.3 -> schema 1-4
bundle 52 requires schema 4   (a runtime-6.2 device must not receive bundle 52)
```

## Legacy coexistence — no big-bang cutover

The legacy firmware (`mesflow/esp-kiosk`) keeps running independently.
Backend endpoint split, if/when needed:

```text
/kiosk/v1 -> legacy
/kiosk/v2 -> new runtime
```

Migration path, per device:

```text
1 device (DEV) -> 1 kiosk canary -> small group -> station group -> full fleet
```

v1 and v2 may run concurrently for an extended period. v1 is not deleted
until v2 has proven stable for a meaningful duration. Nothing in this repo
implements or schedules that migration yet — this document only fixes the
target sequence.

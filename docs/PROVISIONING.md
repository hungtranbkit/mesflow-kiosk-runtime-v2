# Provisioning

**Status: DEV-path skeleton implemented and verified live** (provision →
reboot → identity persists across multiple subsequent reboots, confirmed
on real hardware). Full factory/fleet provisioning is NOT built — see
"What's NOT here" below.

## Device identity — three different things (§4)

```text
device_id       logical/business identity MESFlow assigns. "" until provisioned.
hardware_id     immutable-ish, derived from the chip's eFuse MAC. Always
                available, never provisioned, never business-meaningful alone.
provisioning_state   gates runtime behavior (below).
```

Invariant 12: no shared fleet secret/default identity. A factory-fresh unit
has `device_id = ""` and `provisioning_state = UNPROVISIONED` — it does
NOT default to something like `"KIOSK-01"` and start talking to a real
backend (§36). Phase 0 used to do exactly the forbidden thing (auto-derive
a `"KIOSK-DEV-XXXX"` device_id from the MAC and treat it as the real
identity) — Phase 1 fixes this by splitting `hardware_id` (always derivable,
never the business identity) from `device_id` (`kiosk::security::DeviceIdentity`,
`security/device_identity.*`).

## Lifecycle

```text
UNPROVISIONED -> PROVISIONING -> ACTIVE -> SUSPENDED -> REVOKED
```

| State | Runtime behavior |
|---|---|
| UNPROVISIONED / PROVISIONING | Persistent identity screen instead of the normal waiting screen ("THIET BI CHUA CAU HINH" + hardware_id, so whoever is provisioning the unit knows which physical device this is). Scans are acknowledged locally (§12 immediate feedback still applies) but rejected with `IDENTITY_NOT_PROVISIONED` — the backend is never contacted. |
| ACTIVE | Normal runtime. Scans are sent. |
| SUSPENDED | Identity screen ("THIET BI TAM DUNG"). Scans rejected with `IDENTITY_INVALID`, backend never contacted for business events. |
| REVOKED | Identity screen ("THIET BI DA BI THU HOI"). Scans rejected with `IDENTITY_REVOKED`. |

**The `*`-hold Wi-Fi recovery flow works in every state, including
UNPROVISIONED** (§40) — it exists specifically to make an unreachable
device reachable again, and must never depend on business identity
existing yet. Verified live this session: the recovery AP is named after
`device_id` when provisioned, falling back to `hardware_id` when not
(`WifiSetupPortal::compute_ap_credentials`).

## DEV-path provisioning (serial console, mirrors `wifi:`/`api-endpoint:`)

```text
provision:<device_id>   assigns device_id, moves state to ACTIVE, reboots
suspend                 forces SUSPENDED (testing §5's behavior, KIOSK-079/080)
revoke                  forces REVOKED
```

Verified live: `provision:KIOSK-LASER-01` → reboot → `/debug/device-state`
showed `device_id: "KIOSK-LASER-01"`, `provisioning_state: "ACTIVE"` — and
this persisted correctly across every subsequent reboot this session (NVS
namespace `kiosk_identity`, separate from `kiosk_v2`'s Wi-Fi/backend config
and `kiosk_seq`'s sequence high-water-mark — three separate NVS namespaces,
so clearing one doesn't affect the others).

## What's NOT here (honest gaps)

- No real factory/fleet provisioning flow (assign device_id + site/station
  + certificate via a real backend-driven process). `tools/provision_device/`
  from the original task list is not built this phase — the serial command
  above is the entire "provisioning skeleton" for Phase 1.
- No PIN/auth protection on the DEV serial commands — anyone with the USB
  cable can `provision:`/`suspend:`/`revoke:` a unit. Fine for a dev bench,
  not for a fielded device.
- No certificate issuance tied to provisioning (see docs/SECURITY.md —
  certificate lifecycle is a data-model skeleton only, not wired to a real
  PKI or to this provisioning flow yet).
- No backend-driven suspend/revoke — `suspend`/`revoke` are DEV-only local
  commands for testing the runtime behavior, not something a real backend
  can trigger yet (that needs Phase 5's command/fleet-management pipeline).

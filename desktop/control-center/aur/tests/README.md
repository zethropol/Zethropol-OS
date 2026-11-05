# AUR test scope

The portable suite runs core worker, receipt, integrity, sandbox argument,
provider, deadline, replay/concurrency, cancellation and timeout cases with
unprivileged temporary fixtures. Unix sockets and a compiler are required.

```sh
python3 -B -m unittest discover -s desktop/control-center/aur/tests -v
cmake -S desktop/control-center -B desktop/control-center/build -DBUILD_TESTING=ON
cmake --build desktop/control-center/build -j 2
ctest --test-dir desktop/control-center/build --output-on-failure -j 3
```

Sixty historical rollout/capture cases require separately retained exact public
host evidence in ignored build directories; some also require the original
public trust policy. They are explicitly opt-in and reported as skipped in a
clean checkout. The production artifacts, private keys and personal host captures
are not published as test fixtures. This opt-in never bypasses runtime checks:

```sh
ZETHROPOL_TEST_HISTORICAL_EVIDENCE=1 python3 -B -m unittest discover -s desktop/control-center/aur/tests -v
```

With the flag set, missing or changed pinned evidence must fail those tests.
The portable suite's skipped count is part of the result, not production PASS.
On the existing source workspace, before separating historical tests, all 532
then-existing Python cases passed against its retained local evidence. The independently
exported source tree reports 481 passed and 60 skipped (541 discovered after the new regression cases).

`live_sandbox.py`, `live_signed_root.py`, `live_lifecycle.py` and `live_polkit.py`
exercise actual namespaces, benign makepkg builds/signatures, user cgroups and
noninteractive authorization denial. They use temporary fixtures and do not
install real user packages. LAB trust overrides are confined to explicit LAB
drivers; they do not open the production broker. The unsigned fixture preflight
is separately compiled; the normal REQUIRED signature verifier rejects it.

Real PolicyKit grants, guest native transactions, Btrfs snapshot restoration,
power-loss and boot verification still require the separately identified
disposable environment in [VM_ACCEPTANCE_PLAN.md](../VM_ACCEPTANCE_PLAN.md).
A namespace or container shares the host kernel and does not certify independent
boot/power-loss recovery.

`test_native_domain_transactions.py` additionally runs real libalpm transactions
against two independent ephemeral PUBLIC trust domains in fresh processes. Its
C probe compiles only with ZETHROPOL_LAB_ONLY, refuses root and is never a
production helper. It neither certifies broker native-domain integration nor
substitutes for guest PolicyKit/snapshot/boot/recovery acceptance.

## Native broker integration (2026-11-06)

The broker now uses a fresh fixed-domain native child for AUR preview/install;
AUR archives never enter the official ALPM handle. Shared Rust tests execute
real signed transactions only in private LAB roots, including foreign signature,
state/plan drift, replay, cancellation and interrupted-commit refusal. The
private `--aur-native` transport accepts no client-selected trust domain.
Production admission remains closed pending approved PUBLIC trust/artifact
inputs and actual deployed root/guest acceptance. See
[the integration report](../../../../services/update/AUR_NATIVE_BROKER_INTEGRATION.md).

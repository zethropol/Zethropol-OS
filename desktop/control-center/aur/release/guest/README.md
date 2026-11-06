Actual disposable guest evidence, 2026-11-06. **Production AUR admission remains CLOSED.**

`guest-evidence.json` contains bounded guest captures, byte hashes, QMP observations and explicit acceptance limits. These are mechanism results, not release-owner or signing-role approval.

`ClosedAurUiProbe.cpp` is the executed guest Qt entry probe. `partial-power-loss.py.executed` is a non-installed execution-source record for the identified VM, not a host command or a production worker. Neither is included in release staging or opens the AUR gate.

The running guest and all disk/serial evidence remain in the private local `.aur-vm-runtime` directory. Earlier failed observations remain labelled; only the independently observed held extraction window is counted as the power-cut case.

# Reliability and recovery

## Save history

Before changing a NAND file, the runtime keeps a flushed previous copy in
`save-backups/` beside `nand/` in the user's data folder. History is bounded
to five completed copies per file, outside the emulated NAND. Temporary
copies are not listed as recoverable saves.

A safe save writes into NAND's `/tmp`. Its close checks earlier write
failures, flush and close, then backs up the last complete save and replaces
it. If any required step fails, the previous save remains. Direct writes
back up once before their first modification and report write/flush errors;
they retain the Wii API's in-place semantics. A backup-storage failure is a
save failure, logged with its reason, so the game cannot report a safe save
as successful while its previous copy was discarded.

Open **F1 → Game → Previous saves** to select a version. **Restore and
restart** queues the selection, saves settings and restarts. On startup,
before the guest opens any files, the runtime stages that version, backs up
the current save and replaces it. Staging occurs before history pruning, so
restoring the oldest retained version remains safe. The pre-restore version
is itself recoverable. Restore requests reject paths outside history and
symlinked paths. A failed request remains available for diagnosis in
`restore-save.txt`; its error is reported before boot.

This is file history. A multi-file game's transaction boundaries are not
inferred, and this is not a general NAND save-state system. Each entry shows
which file will be restored and its UTC backup time.

## Controllers

The default single-player mode still accepts all controllers. Optional
player-one and player-two assignments are saved. Identity uses the device's
GUID and serial where available, otherwise GUID/name and an instance slot.
Indistinguishable pads without serials are assigned by connection order when
both are absent; software cannot recover a physical distinction the driver
does not expose. Reset automatic order in Controls to assign them again.
Disconnecting one pad leaves its player slot reserved. The other player does
not move. Reconnecting the identifiable pad reclaims its slot. A device
explicitly assigned to one player is excluded from the other player's
automatic choice. Missing explicit devices stay disconnected.

Controls also expose stick/trigger thresholds, pointer dead zone, rumble
strength, volume and optional background muting. Release thresholds are five
percentage points below press thresholds. Focus loss clears physical input
and stops rumble; rumble uses renewable short leases. Closing settings waits
for gameplay controls to be released before forwarding them again.

## Updates and checks

The source launchers remember a changed disc only after a successful build,
and validate `sys/main.dol`, `sys/fst.bin` and `files/`. The Windows kit's
build stamp includes hashes of the kit archive, recompiler, plan and flags,
so an update using the same version number still rebuilds. Executable copies
are staged; replacing an existing Windows executable keeps its previous
copy in `Punch-Out.exe.previous`.

Ordinary builds compile/link `punchout_runtime_check` with empty stand-ins
for generated game functions. It is a build check, not a playable game. CI
checks all runtime sources on each platform and runs Linux graphical tests
under Xvfb/Mesa. `QILIN_REQUIRE_GRAPHICS=1` turns absent graphics into a test
failure there. Machines without graphics can still skip those tests normally.
Audio tests use actual SDL dummy-device callbacks while producers change
rates and volume. Scheduler/save/audio tests also run with ASan and UBSan;
leak checking is disabled because live guest threads intentionally survive
to process exit. These checks need no disc; actual fight timing still needs
matched game captures (see `timing-validation.md`).

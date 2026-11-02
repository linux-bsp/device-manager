# Repository Guidelines

## Structure
`ota/` contains the C management server, OTA CLI, firmware daemon, shared protocol and embedded Web page. `firmware/` owns the standalone A/B engine; `fwctl/` contains the device CLI and peripheral health adapter. `logging/` provides logctl/logctld. Keep board layouts and Buildroot package definitions in the sibling bsp-base-external repository.

## Build and Test
Use `make host` for device-server and ota-cli, and `make device` with the target compiler for fwctl, fwctld, logctl and logctld. Supply dependency paths through CPPFLAGS and LDFLAGS. Buildroot installs host tools under host/bin and device tools under target/usr/sbin. Never execute a target binary on the build host without the matching emulator/sysroot.

Run `python3 tests/test_firmware.py`, `make -C logging test` and `make -C ota test`. Board integration tests expect the surrounding workspace. Test rollover, process lifetime, corrupted input and interrupted firmware transactions. Preserve existing on-media metadata and network protocol compatibility when refactoring source organization.

## Style and Boundaries
Use C11 or GNU C11, four spaces in new userspace code, and existing indentation in migrated files. Compile with Wall, Wextra and Werror. Use concise `[module] [INFO|WARN|ERROR]` logs. Human-facing hashes use eight characters; checks and audit records use full SHA256. SOL is a read-only log transport, not an interactive shell. The user explicitly enables credential-free debugging: server --sol-debug-no-auth and device auth=none. Do not reintroduce a credential prompt in this mode. Never put device credentials in firmware archives; keep them in separately provisioned storage. Device firmware operations must remain independent of a reachable management server.

## Releases and Commits
Keep the firmware tag v0.0.1 unless explicitly instructed otherwise. Test, build and publish authorized firmware changes, then commit the matching source only after the intended device reports success following reboot and health checks. An OTA success does not validate an uninstalled bootloader or durable storage that was not provisioned. Preserve source snapshots and validation results. Use concise `feat:`, `fix:` or `refactor:` commits; push each commit to the configured remote, then update and push dependent workspace gitlinks. Never auto-format device storage or overwrite earlier release artifacts.

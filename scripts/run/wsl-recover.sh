#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Force-stop a wedged WSL2 VM from Windows-side tooling (Git Bash, or another WSL instance's interop).
# Plain `wsl --shutdown` waits for the instance and can block forever when the guest kernel is stuck; --force does not.
set -u
exec wsl.exe --shutdown --force

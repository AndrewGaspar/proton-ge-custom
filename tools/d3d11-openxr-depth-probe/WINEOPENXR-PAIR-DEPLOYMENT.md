# WineOpenXR matched-pair deployment

The WineOpenXR PE and Unix libraries use a generated private dispatch ABI.
Always build, package, deploy, and roll back both files as one pair. Public PE
exports, ELF `NEEDED` entries, and SONAME are not sufficient compatibility
checks.

For GE-Proton10-32 the active pair is:

- PE: `files/lib/wine/x86_64-windows/wineopenxr.dll`
- Unix: `files/lib/wine/x86_64-unix/wineopenxr.so`

The pair used for the 2026-08-14 staged-color and Walkabout A/B campaign is
pinned as follows:

| Artifact | SHA-256 |
|---|---|
| GE and prefix-local `wineopenxr.dll` | `5d81cdcf216c32ed4c29e3d6d5b58b1f6522568dbbef60422460fbb576e51c9b` |
| GE `wineopenxr.so` | `daf53de43cc8df0e7428b13bd51b5d12533b55a431a7971f6d531c3a2ab20e2f` |
| GE and prefix-local native DXVK `d3d11.dll` | `f5b94695a2f810128f375b66e643cb9d38a75ebae6ce48addeda9f10d30ae7a3` |
| GE and prefix-local native DXVK `dxgi.dll` | `18acad33d52582876abf8b67fe1b2293ae3b68d46db50bb4a84b74626fc6775b` |

These hashes were rechecked against all seven deployed files during the
documentation audit. They are experiment provenance, not a promise that a
future build should retain the same bytes. The guarded runners remain the
executable authority and must reject any unpinned replacement.

Before replacement, stop only the Meta prefix with its exact wineserver, copy
both active files into one timestamped rollback directory, and record their
hashes. Install each candidate through a temporary file in the destination
directory followed by an atomic rename. Verify both installed hashes before
starting the prefix. A prefix-local `drive_c/windows/system32/wineopenxr.dll`
copy must receive the same PE and be backed up and restored with the pair.

Run `verify_wineopenxr_pair.py` against the candidate PE, Unix library, and the
prepared tree's generated `loader_thunks.h` before deployment. Then cold-start
with the guarded probe's `bootstrap` stage. Require its expected PASS marker,
normal exit, no Xalia/XRizer or OpenXR-session creation, and no new NVIDIA Xid.
Only then proceed through guarded `draw`, `unity`, and one game launch, stopping
at the first failed guard or device loss. Keep
`WINEOPENXR_STAGED_CROSS_GPU_COLOR=1` scoped to those processes; do not set it
globally.

Rollback means stopping only the exact Meta-prefix wineserver, atomically
restoring the saved GE PE, prefix-local PE, and GE Unix library, verifying all
three hashes, and leaving unrelated Wine processes untouched.

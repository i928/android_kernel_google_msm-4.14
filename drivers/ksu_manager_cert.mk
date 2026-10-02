# sunfish: our own self-built KSU-Next manager (i928 KSU-Next Manager),
# signed with ~/ksu-manager-build/keystore/ksu-manager-release.jks.
# Computed via apksigner verify --print-certs + manual v2 signing-block
# cert extraction (2048-bit RSA, minimal DN -> 746-byte DER, comfortably
# under CERT_MAX_LENGTH).
# Key rotated 2026-10-02: the previous key (cert 0x2ea, b22ee43b...) had its
# private key published on GitHub (device lmi repo, security/ksu_manager/), so
# any app signed with it could be crowned manager. New keystore:
# ~/ksu-manager-build/keystore/ksu-manager-2026-10.jks; the .pk8 stays local.
KSU_NEXT_MANAGER_SIZE := 0x2f3
KSU_NEXT_MANAGER_HASH := e77745f99a7cc40536d9ceb3fe050463065bbe4eb4f45f8f34691564a6b51459

# /product/app/<Module>/<Module>.apk doesn't encode the package name in its
# path (no "<pkg>-<hash>" segment for crown_manager()'s path parsing to find),
# so it needs the real package name given explicitly -- see the
# get_pkg_from_apk_path fallback in kernel/manager/throne_tracker.c. Requires
# the submodule (KernelSU-Next) to be at or past 893fb52d: fb4a650f runs a
# manager scan at init for non-late-load kernels, c969b344 adds the fallback
# that consumes this variable, and 893fb52d skips a pre-filter that would
# otherwise reject every /product/app candidate outright once this variable
# is defined. Verified 2026-09-03: fixes both "Unsupported" and "cannot load
# module zip" for our /product-baked manager, no boot regression (confirmed
# via a per-commit mini-bisect after an earlier report of a hang -- the hang
# did not reproduce on retest, consistent with the known pervasive
# intermittent boot issue, not this fix).
#
# Renamed 2026-09-09 from com.rifsxd.ksunext to a neutral package so root/manager
# detectors (duck detector) that key on the well-known KernelSU-Next package name
# no longer find the baked manager. Must match the manager APK's applicationId
# (build.gradle.kts). The code namespace stays com.rifsxd.ksunext; only the
# installed package id changed. NOT com.android.*/com.google.* -- a system-named
# package signed with a non-platform key is itself a detection tell.
KSU_MANAGER_PACKAGE := dev.i928.mgr

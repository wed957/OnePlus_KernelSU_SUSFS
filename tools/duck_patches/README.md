# Enhanced Duck builds

Both patches target Duck-Detector-Refactoring revision
8b44188c355e82c5ddc1c121735c36ec881e7703.

- enhanced_timing_probes.patch fixes the isolated KSU supercall path and SELinux
  timing variants, requires parser reachability before reporting timing coverage,
  and makes UID-only package visibility differences informational. It includes
  the evidence records and regression tests.
- adb_diagnostics.patch adds an optional ADB command interface, a CLI and tests.
  Apply it after the timing patch and build with
  "-Pduckdetector.adbDiagnostics=true" to enable the provider.

The "Build Enhanced DuckDetector" workflow builds Release APKs. Leave its
"adb_diagnostics" input off for the ordinary optimized APK, or enable it for the
version ending in "-adb". The latter also uploads the CLI and its usage guide.
Both choices use the same existing Release signing configuration and code/resource
shrinking. Diagnostic builds do not enable JDWP.

The workflow checks out its own triggering kernel-repository revision for these
patches, rather than fetching a different revision from main.

UID and SELinux regression checks run for both build variants. An ordinary-app
SETCURRENT refusal before context parsing is unavailable coverage, even when
SELinux hiding is enabled; it must not be interpreted as absence of hiding.

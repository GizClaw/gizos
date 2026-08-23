# Crash Before Confirm

`crash-before-confirm` is a portable Example that invokes a launcher-provided
crash callback after Runtime startup. It does not depend on H2Loader package or
image APIs. A H2Loader-managed target can use it to observe rollback and
coredump behavior, while the external acceptance flow owns that verdict.

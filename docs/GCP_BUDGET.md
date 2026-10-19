# Talus cloud spending limit

User-authorized project: `talus-512109`.

The user authorized a **USD $5 cumulative maximum** for GCP resources created
by this agent for Talus. This includes Phase 9 validation and any later runs,
VMs, disks and associated network charges. It is not $5 per run. Do not increase
this amount, reset it for another phase, or start another run without accounting
for prior charges. Unrelated account activity is outside the agent's control.
GCP billing budgets/alerts are not hard spending cutoffs.

## October 19, 2026 attempt

Phase 9 source/dependencies and an unattended Linux validation script were
prepared locally before attempting provisioning. The plan used an enforced
15-minute runtime limit, automatic instance/boot-disk deletion and no service
account. The standard shape's on-demand compute estimate was about $1.78 for
the full 15 minutes, with additional headroom reserved for disk/network costs.

Google rejected every attempt with ZONE_RESOURCE_POOL_EXHAUSTED_WITH_DETAILS:

- c4-standard-144: us-central1-a, -b, -c and -f.
- c4-standard-144-lssd: us-central1-a.
- c4-highcpu-144: us-central1-a.

No instance or disk was created by these attempts. Validation has not run on
Linux. Do not label Phase 9 hardware acceptance complete. Actual billing can
lag; verify billing before a later paid attempt instead of treating an empty
resource inventory as an invoice.

Prepared files and API resource checks are retained under the ignored
`build/phase9-validation/gcp/` directory. No cloud deployment credentials are
stored in this repository.

Pricing reference:
https://cloud.google.com/products/compute/pricing/general-purpose

Runtime-limit reference:
https://docs.cloud.google.com/compute/docs/instances/limit-vm-runtime

## Linux TSan allocation-test check

A separate e2-standard-4 VM (`talus-tsan-check`, us-central1-a) was used to
reproduce the Ubuntu 24.04 / Clang 18 allocation-test linker failure and verify
the fix. It had a 900-second automatic-delete limit and an auto-deleted 20 GB
boot disk. The listed compute price is $0.13402284/hour (about $0.034 at the
maximum lifetime); reserve **$0.10** of the cumulative $5 budget for this run
and associated charges until actual billing is available. This reservation
leaves $4.90 uncommitted; do not reset the budget for later runs.

The TSan allocation target linked and all six tests passed with no TSan
reports. Release and ASan/UBSan allocation suites also passed locally. This
small VM has no required PMU support and establishes no hardware-counter
performance claims. Logs are in `build/tsan-link-validation/`.

Cleanup verified: the VM and boot disk were deleted, project inventory lists
no instances/disks/reserved addresses, and validation SSH metadata was removed.
The final inventory is retained with the run logs.

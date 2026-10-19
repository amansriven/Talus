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

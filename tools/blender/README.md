# `meridian_export` — Blender art pipeline addon

The only sanctioned glTF export path (wraps the stock exporter). Enforces at export time
what CI re-checks: naming, scale/transform, kit pivots, LOD-chain completeness, master-material
parentage, and IF-8 sidecar scaffolding.

**Contract:** [Art SAD §2.1](../../docs/sad/art-sad.md) · shares `budgets.json` with the CI budget validator.

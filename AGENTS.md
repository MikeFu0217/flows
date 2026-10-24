# Development workflow

Use this checkout for the Flows synthesizer. Keep the public repository at
`flows-audio/flows`; never import private historical Git objects or files.

New development uses the `MikeFu0217` GitHub identity and commit identity.
Create a focused branch, push it to `MikeFu0217/flows`, and open a pull request
against `flows-audio/flows`. Run relevant checks locally, inspect the diff and
all review feedback, address actionable findings, and merge the PR when ready.
Do not add CI for now. Never present an absent review as an approval or bypass
repository permissions or branch protection. Codex may be credited as co-author.

Preserve unrelated work. Keep `/Applications/Flows.app` as the only unpacked
standalone Flows synthesizer app on this Mac. After local validation and install,
remove intermediate standalone/test app bundles and refresh Launch Services.

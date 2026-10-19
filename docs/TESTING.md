# Repeatable testing

See [route usage](../testing/USAGE.md) and [counter references](../testing/REFERENCES.md). Fresh-card MVP and cinematic routes need only your disc. Saved-game, forest and Antarctica routes need independently created saves; no save/card seeds are distributed. Establish new references for your own save. The old forest route contains a death-menu tail, so time only its gameplay window or use the survivable route.

The portable Windows runner accepts explicit executable, ELF, ISO, route and external work paths. It creates a fresh card, hides the window, silences audio, launches BelowNormal, records environment/results and enforces a hard timeout. Optional slot and busy-flag paths permit shared-machine admission control. It only stops its own child on timeout. Keep the runtime crash reporter enabled.

Use identical script/save/settings for comparisons. At least three quiet repetitions per variant are needed for new timing claims: record total CPU load, discard unrelated busy runs, report spread and segment boundaries. Pad reads per wall second measure game fps; presented field fps is separate. Keep perfscope CSV, FIELD60 TSV and screenshots private. Counter equality does not prove image/audio correctness. See [controlled hash gate](RESEARCH.md) and the [Windows power trap](PERFORMANCE.md).

Before a source handoff run `python tools/check_no_game_data.py .` and review provenance manually. The scanner rejects generated/game files, private packages, unknown binary content and build products. It does not audit Git history or resolve licensing.

Source-only release-prep checks: `python -B testing/test_release_prep.py`, `python -B tools/check_no_game_data.py .`, `python -B tools/check_staged.py`, and `git diff --check`. The offline suite uses synthetic data and mocked external setup commands; it covers interruption/reuse, stale inventories, altered/extra generated files, staged/worktree disagreement, size limits, renamed private files, symlink modes and missing policy inventories. Real fresh-disc code generation and scripted gameplay routes remain separate qualification gates. See SETUP.md for the two-output comment-change gate.

## Testing a source snapshot without Git

The full release-prep suite includes Git index fixtures and requires Git on PATH. Without Git, run `python -B testing/test_release_prep.py GenerationTests` and `python -B tools/check_no_game_data.py .`; record the remaining Git-dependent suite and staged/diff checks as unavailable. These partial checks do not qualify the complete host suite.

If Windows PowerShell reports duplicate `Path` / `PATH` keys while starting a test,
use Python to normalize the child environment. Replace paths with your own inputs
and append the runner arguments from the route usage examples above:

```powershell
python -c "import os,subprocess,sys; sys.exit(subprocess.call(['powershell.exe','-NoProfile','-File','testing/run_test.ps1']+sys.argv[1:],env=dict(os.environ)))" -Exe <exe> -Elf <elf> -Iso <iso> -Script <route> -Work <fresh-work>
```

Replace each angle-bracket placeholder before running. No machine settings change.

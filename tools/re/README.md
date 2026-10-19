Ghidra helper scripts used for the reverse engineering (paths inside are from the build
container: project /home/claude/gh/proj, programs nwlin / nwwin). See docs/WORKFLOW_HANDOVER.md.
ghidra/*.java: DumpFuncs (jsonl of every function: calls, strings, data refs), Decomp (decompile by
name regex or address), DumpVtables, Xrefs, DemangleAll, FixFuncs.
dl.sh / dw.sh / pair.sh: decompile Linux / Windows / both; q.py, callers.py: query the
Linux->Windows match; match.py, match2.py, srcloc.py: build the match; clean.py: tidy output.

# Reproducible experiments

`tools/lpta_experiment.py` builds the plugin against one explicit LLVM prefix,
compiles the sample with that installation's Clang, and runs its opt. It checks
trace integrity, runs `-verify-each`, compares optimized IR with and without
instrumentation, checks filters against the full trace, and generates HTML.
It requires Linux, Python 3.10+, CMake, a C++ compiler, and an LLVM development
installation containing `llvm-config`, Clang, and opt.

```bash
python3 tools/lpta_experiment.py --llvm-prefix /usr/lib/llvm-18 --output /tmp/lpta-18
python3 -m unittest discover -s test -p 'test_*.py'
```

The output directory must be new. It contains a manifest with tool versions,
input/IR/source hashes, exact commands, check results, and per-pass aggregates;
the trace, optimized IR, report, and command log remain alongside it. Failed
runs retain a manifest with `status=failed`. A reusable build directory can be
selected with `--build-dir`; use a separate build for each LLVM installation.

For the migration, run the same C source with each matching Clang, then run
the newer optimizer on the older IR too:

```bash
python3 tools/lpta_experiment.py --llvm-prefix /path/to/llvm-23.1.1 --output /tmp/lpta-23
python3 tools/lpta_experiment.py --llvm-prefix /path/to/llvm-23.1.1 \
  --input /tmp/lpta-18/input.ll --output /tmp/lpta-23-on-18-ir
python3 tools/lpta_compare.py /tmp/lpta-18/manifest.json /tmp/lpta-23/manifest.json
python3 tools/lpta_compare.py /tmp/lpta-18/manifest.json /tmp/lpta-23-on-18-ir/manifest.json
```

Comparison requires matching pipelines, plugin source, and heuristic labels,
plus matching source-input or IR hashes. It distinguishes frontend-plus-optimizer
changes from changes on the same IR. It aggregates by pass and IR-unit kind,
retaining repeated invocations; it does not guess event alignment from pass
names. Enclosing manager/adaptor events remain inclusive and candidate flags
remain the original coarse heuristic. These counts are not compile-time savings.

Use `--input fixture.ll` for focused IR experiments and `--pipeline` for an
explicit pass sequence. Record new fixtures and their expected behavior as the
analysis lifecycle feature is implemented. These checks do not yet establish
analysis-preservation soundness or validate the historical static survey.

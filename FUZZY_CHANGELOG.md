# fuzzy-cpython downstream changelog

This file records downstream fuzzy-cpython work only. Upstream CPython changes
remain documented by CPython's normal `Misc/NEWS.d` process.

## Unreleased

- Added the opt-in `-X fuzzy[=0|1]` interpreter mode while retaining standard
  CPython behavior by default.
- Added bounded, project-neutral fuzzy values, modules, provenance, events, and
  deterministic single-run fallback operations.
- Added origin-sensitive recovery for direct user imports, including absolute
  and relative `importlib.import_module()` calls, selected missing names, and
  direct bytecode attribute loads.
- Pinned fuzzy bytecode imports to the original builtin importer retained in
  per-interpreter state. Standard mode still honors an overridden
  `builtins.__import__`; fuzzy post-import compatibility remains a separate
  captured hook.
- Captured missing-name and missing-attribute recovery callbacks in
  per-interpreter state before user code, preventing later builtins/module
  attribute replacement from redirecting those recovery sites.
- Moved ordered event records, limits, terminal budget state, and the one-time
  sink reference into per-interpreter C state behind `_fuzzy_runtime`; event
  queries return defensive copies and module reimport does not reset a run.
- Moved missing-name and selected direct missing-attribute event construction
  into their C recovery sites. Trusted values are allocated directly by C from
  the exact type and provenance/depth member descriptors captured before user
  code, bypassing mutable Python `__new__` and `__init__` methods.
- Centralized fuzzy startup, hook installation, state cleanup, direct-import
  construction, and name/attribute recovery in `Modules/_fuzzy_runtime.c`; generic object and
  interpreter files retain only their semantic call sites.
- Added the fixed WASI-only
  `fuzzy_cpython_preview1.checkpoint(ptr, length)` embedding import. Trusted C
  recovery emits a bounded version-1 `FCP1` record through it before the legacy
  Python sink when the build explicitly defines
  `PY_FUZZY_CPYTHON_WASI_CHECKPOINT=1`; default/native builds do not add the
  import, and public/Python-origin event emission remains untrusted.
- Added `fuzzy_runtime` to the fork's frozen stdlib set and made fuzzy startup
  initialize that frozen module directly instead of resolving it through
  `PYTHONPATH`, cwd, or the entry-script path.
- Separated one-time interpreter activation from embedding configuration.
  `configure(user_root=...)` may select the user-code root before the first
  event without reinstalling fuzzy semantics; later root changes are rejected.
- Added unresolved-target handling for `os.remove`, `os.unlink`, `os.rmdir`,
  `os.removedirs`, `Path.unlink`, `Path.rmdir`, and `shutil.rmtree`; exact
  operations continue through the ordinary OS interface.
- Added bounded interpreter-owned identity registries for values created by
  C-owned missing-name and selected missing-attribute recovery. Direct
  `os.remove`, `os.unlink`, and `os.rmdir` path conversion propagates that
  identity only to its exact `str` or `bytes` result; those operations emit the
  trusted checkpoint before skipping the OS call.
- Moved directly missing user import statements to a C recovery site that emits
  the import checkpoint, allocates fresh module chains through native module
  base slots without calling mutable Python constructors or the old module
  factory, and registers exact fuzzy-module identities. Missing attributes loaded
  from those modules can propagate trusted identity to direct primitive deletes.
  Existing public fuzzy prefixes are not upgraded; programmatic imports and
  public fuzzy-module construction remain explicitly untrusted.
- Added identity-based C call dispatch for values created at trusted recovery
  sites. Positional, keyword, and expanded unknown calls emit a trusted
  `unknown-call` record, skip the callable without inferring internal effects,
  and register the bounded `.result` value so a later direct primitive delete
  retains unresolved-selection provenance. Publicly constructed fuzzy values
  and calls remain Python-origin and untrusted.
- Kept marker scanning as an explicitly untrusted conservative fallback.
  Public token allocation, visible marker text, programmatic-import/public-call
  values, ordinary Python-derived paths, pre-converted paths, and higher-level
  deletion helpers cannot mint the trusted deletion identity.
- Added focused standard/fuzzy regression coverage and downstream standalone
  documentation.

No standalone fuzzy-cpython binary or source distribution has been declared
yet. Embedding projects may separately pin source revisions and publish
capability-specific artifacts under their own release policy.

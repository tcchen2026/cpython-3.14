# fuzzy-cpython

`fuzzy-cpython` is an experimental downstream fork of CPython 3.14 for tolerant,
single-run execution of partially unsupported Python programs.

The goal is to let selected failures—such as a directly missing user import,
an unknown value, or an unsupported call—produce bounded fuzzy values and
diagnostics so later code can continue. This is intended for consequence
observation and developer tooling.

“Fuzzy” here does **not** mean fuzz testing. It also does not mean symbolic or
multi-path execution, and a completed run is not proof that a program is safe.

## Project status

This fork is under active development and is not yet a released fuzzy Python
distribution. The source tree contains an opt-in `-X fuzzy` activation path,
project-neutral fuzzy values, bounded events, origin-sensitive import recovery,
selected missing-name and missing-attribute recovery, and conservative handling
of unresolved deletion targets. A temporary WASIp1 source candidate exercises
that work plus a fixed embedding checkpoint import, but no released artifact
contains it yet.

The current source baseline is Python 3.14.7. Downstream commit
`7e557250af09e670192f81c30d67eee995d90615` is the initial migration baseline;
it added an explicitly installed missing-attribute callback at CPython's common
object and optimized method lookup paths. The source changes after that baseline
gate all recovery on `-X fuzzy`. Standard mode does not install the runtime or
consult its recovery callbacks. The current source also narrows fork-owned
attribute recovery to interpreter attribute-load, optimized method-load, and
import-from failure points; general attribute C APIs retain ordinary behavior.
At fuzzy startup, the interpreter captures strong references to the original
builtin importer, its post-import hook, the direct-miss origin decision, the
exact fuzzy-module type, the missing-name and missing-attribute recovery
functions, and the exact fuzzy-value type and its provenance/depth member
descriptors in per-interpreter state before user code runs. Replacing `builtins.__import__`,
`fuzzy_runtime._real_import`, or the corresponding visible hook attributes later
therefore does not redirect the importer or those interpreter recovery sites.
The event list, limits, terminal budget state, and one-time sink reference are
also held in per-interpreter C state behind the private built-in
`_fuzzy_runtime`. Returned events are defensive copies; assigning similarly
named Python globals or deleting and reimporting the private module does not
reset that state.
The corresponding startup, hook installation, state cleanup, direct-import
construction, and missing-name/attribute recovery code is centralized in
`Modules/_fuzzy_runtime.c`; generic object and interpreter files retain only
their semantic call sites, including one startup call from `Modules/main.c`.
An import statement whose original importer raises a directly matching
`ModuleNotFoundError` now reaches a C recovery site. After the captured origin
policy accepts it, C emits the missing-import event, freshly creates the missing
module chain through the native module base slots, and registers the exact
resulting `FuzzyModule` identities. This bypasses mutable Python
`FuzzyModule.__new__`, `__init__`, and the old module factory; an existing public
fuzzy prefix is not upgraded. A missing attribute loaded from one of those
trusted modules inherits trusted import provenance.
Missing-name and selected direct missing-attribute recovery likewise construct
their event message and provenance at the C recovery site, append through a
private trusted C entry point, and allocate the exact fuzzy-value type directly
in C. The allocation sets the captured provenance/depth members without calling
mutable Python `__new__` or `__init__` methods. Replacing similarly named Python
module attributes or constructors therefore does not suppress, redirect, or
alias those trusted event/value steps. A value created at one of those trusted
sites is also recognized by object identity at CPython's ordinary call
dispatch. Its call is skipped in C, its bounded `unknown-call` event is emitted
there, and its derived result inherits trusted identity. This covers positional,
keyword, and expanded calls without executing or guessing the unknown callable's
internal effects. Publicly constructed values retain ordinary Python-origin call
behavior. The Python origin-decision and post-import hooks and their reachable
objects are still Python objects; reflective tampering can still fail execution.
This is a hardened subset, not a claim that all fuzzy semantics are
non-reflective.

Atuin Shell Guard's current release artifact still supplies fuzzy semantics
through its injected runtime prelude. Shell Guard has not switched to this
source candidate, so the source implementation described here and the current
production integration must not be confused.

## Intended mode contract

Fuzzy behavior remains opt-in. The initial source-level interface is:

```console
python3 -X fuzzy script.py
python3 -X fuzzy -
python3 -X fuzzy -c 'source'
python3 -X fuzzy -m package
```

`-X fuzzy=1` is equivalent and `-X fuzzy=0` explicitly disables the mode. The
last repeated fuzzy option wins; any other value is rejected before user code
runs. Executable renaming or packaging is not implemented yet.

When enabled, interpreter startup executes the fork-owned frozen
`fuzzy_runtime` directly and then activates its recovery hooks. It does not
resolve this startup dependency through `sys.path`, so `PYTHONPATH`, the entry
script directory, and the current working directory cannot shadow it. Without
fuzzy mode, the interpreter must preserve standard CPython imports, exceptions,
object behavior, and standard-library fallbacks.

When enabled, the intended runtime will own:

- origin-sensitive recovery for directly missing imports from registered user
  code through import statements or `importlib.import_module()`, without
  swallowing bootstrap or stdlib fallback exceptions;
- fuzzy module and value types with bounded provenance;
- selected missing global, local, module, user-object, builtin, and extension
  attributes;
- skipped unknown calls and deterministic fallback values for later execution;
- bounded, versioned recovery diagnostics; and
- explicit limits for fuzzy events and value derivation.

The current source candidate implements the behaviors above for entry code and
modules beneath one user root while excluding the standard-library root. This
origin policy and the modified interpreter failure paths still require broader
native-platform compatibility testing before a release.
For `-m`, a real module `__file__` takes precedence over its temporary
`__main__` name, so executing a stdlib module does not accidentally opt its
internals into fuzzy recovery. File-less `-c` and stdin main modules remain
registered user code.

Attribute fuzziness currently applies to direct Python attribute syntax and its
optimized method-load path. General APIs such as `getattr()`, `hasattr()`, and
C-extension attribute probes retain their normal `AttributeError` behavior so
library fallback logic is not silently changed.

The input program is executed once. Fixed defaults such as false, empty, or
zero describe that one observed run; they do not cover alternative branches.

Focused fork and Wasm-candidate coverage keeps the opt-in/default-mode split
consistent for stdin, `-c`, script-file, and `-m` entry points. This does not
make every package or standard-library module compatible with fuzzy recovery;
origin rules and unsupported behavior still apply.

## Runtime observation API

In fuzzy mode, `fuzzy_runtime` is imported and activated automatically exactly
once. It exposes `configure()`, `is_enabled()`, `get_state()`, `get_events()`, and
`set_event_sink()`;
importing it explicitly in standard mode is harmless and reports that fuzzy
mode is disabled. Runtime recovery hooks check interpreter-owned configuration
rather than trusting a path-resolved module as the authority for activation.
The module is part of the fork's frozen stdlib set; build metadata for Unix and
Windows must be regenerated when its source changes.
The original builtin importer and the missing-import, missing-name, and
missing-attribute hook references are also retained by the interpreter instead
of looked up through mutable Python module attributes on each operation. Fuzzy
bytecode imports use that retained builtin; standard mode continues to honor an
overridden `builtins.__import__`. Ordered event records, budgets, exhaustion
state, and the sink reference likewise live in interpreter-owned C state rather
than authoritative Python module globals.

Protocol version 1 events are ordered JSON-compatible mappings with these
fields: `protocol_version`, `sequence`, `kind`, `code`, `message`,
`provenance`, `operation`, and `truncated_fields`. Event, text, provenance-depth,
and tracked fuzzy-path limits have hard caps. Text truncation is explicit in
`truncated_fields`. The final event slot is reserved for
`fuzzy-event-budget-exhausted`; the recovery that would cross the limit raises
`FuzzyEventBudgetExceeded` instead of continuing invisibly. `get_events()`
returns defensive copies.

`configure(..., user_root=path)` lets an embedding select the user-code root
after interpreter startup and before the first recovery event. It changes
configuration only; it does not activate or reinstall fuzzy semantics. Changing
to another root after an event is rejected, so one observed run retains one
origin boundary.

`set_event_sink(callable)` installs one synchronous observer before the first
event. The observer receives a defensive copy of each event and cannot be
replaced through the public API or by reimporting the private state module. An
embedding host can use this for bounded checkpoint transport without putting
host policy into the fork.

When a WASI build explicitly defines
`PY_FUZZY_CPYTHON_WASI_CHECKPOINT=1`, the source additionally imports exactly
`fuzzy_cpython_preview1.checkpoint(ptr, length)`. Trusted C recovery sites send
a bounded version-1 `FCP1` binary record through this synchronous call before
notifying the Python sink. The record is at most 32 KiB and contains only the
event sequence and bounded UTF-8 event fields. It carries no descriptor, host
path capability, executable, or policy decision. An embedder that opts into
this build profile must provide the import and validate the pointer, length,
framing, sequence, and event schema. Default and native builds do not use it.

Public `emit_event()` calls and Python-origin events do not use the trusted
checkpoint. Programmatic `importlib.import_module()` and direct `__import__()`
recovery, public `FuzzyModule`/`FuzzyValue` construction and calls, and
higher-level unresolved-delete behavior still reach an embedding only through
the ordinary event sink. In contrast, a directly missing user import statement confirmed at
the interpreter's import opcode emits its import record through the checkpoint
and registers the exact fuzzy module identity. A selected missing attribute from
that identity, or from another C-owned recovery site, creates a trusted value. A
direct call of that exact value is skipped at C call dispatch, emits a trusted
`unknown-call` record, and registers the exact derived result with bounded
`.result` provenance. Arguments are deliberately ignored and no effects inside
the unknown callable are inferred.
When such a value is passed directly to C `os.remove`, `os.unlink`, or
`os.rmdir`, CPython propagates its internal identity to the exact `str` or
`bytes` returned by that deletion call's own path conversion. Only that
converted object can send the unresolved-delete record through the checkpoint
before the OS call is skipped. The custom checkpoint is still an incremental
trust boundary, not a complete authenticated event stream.

`FuzzyValue` records skipped unknown calls and provides fixed false, empty,
zero, string, path, unary, and binary fallbacks while retaining bounded
provenance. Directly missing user imports create `FuzzyModule` objects; selected
missing names and attributes create cached or derived values so later
statements can continue.

Unresolved strings carry deterministic run-local markers allocated in bounded
per-interpreter C state. The direct C entry points for `os.remove`, `os.unlink`,
and `os.rmdir` recognize those markers, emit an `unresolved-operation` event,
and do not call the operating system. Marker recognition alone is deliberately
untrusted: marker text is visible, `_fuzzy_runtime.path_token()` is callable,
and ordinary Python code can copy, join, or pre-convert a marker. Such a call
continues conservatively through the ordinary event sink but cannot select the
WASI checkpoint. A trusted checkpoint additionally requires the internal
C-origin value/path identity chain described above. Exact targets still use
their ordinary implementation. Higher-level `os.removedirs`, `Path.unlink`,
`Path.rmdir`, and `shutil.rmtree` currently use Python-origin
marker/provenance handling with the same conservative outcome. This
interception is consequence metadata, not a filesystem sandbox.

## Embedding and consequence observation

`fuzzy-cpython` defines tolerant Python-language behavior. It does not define
host permissions, filesystem isolation, or risk policy.

An embedding host remains responsible for capabilities and for deciding what an
observed operation means. In Atuin Shell Guard, concrete filesystem operations
must reach the custom WASI host and its copy-on-write virtual filesystem journal
before they are treated as concrete effects. A Python-level unresolved-operation
event may express uncertainty, but it must never invent a concrete path or claim
that a host mutation occurred.

The public event API and Python sink remain in-process observation APIs, not an
authenticated security channel. Interpreter ownership prevents ordinary Python
globals, returned snapshots, and private-module reimport from replacing or
clearing the record list, budgets, or sink reference. Code running in the same
interpreter can still mutate objects reachable by retained Python hooks, call
the public/private untrusted emission API to fabricate an event, or print an
embedding's stderr framing directly.

The WASI checkpoint gives C-confirmed direct import statements, C-owned
missing-name/missing-attribute recovery, identity-confirmed calls of those
values, and the identity-propagated direct primitive-delete subset a
host-recognizable channel that ordinary Python APIs cannot select or replace. A
visible marker, a value from a programmatic import, a call of a public fuzzy
value/module, an ordinary Python-derived or pre-converted path, or a call to the
public token allocator does not acquire that identity. A result derived by the
C-owned trusted-call path does retain identity and can therefore report a later
direct primitive delete as unresolved. The checkpoint does not authenticate
Python-origin programmatic imports, public calls, or higher-level
unresolved deletions, nor does it make the captured Python origin policy or
post-import hook immutable. An embedder must
validate and bound every channel, treat untrusted records only as additive
uncertain evidence, and keep concrete effects authoritative at its capability
boundary. Putting a nonce in Python source, globals, arguments, or environment
variables does not strengthen this boundary.

## Development principles

- Standard CPython behavior is the default and receives regression coverage at
  every modified interpreter failure path.
- Fuzzy recovery is limited to explicitly registered user code.
- Recovery, diagnostics, provenance, recursion, and generated values are
  bounded and deterministic.
- Unsupported or exhausted behavior remains visible; it does not silently
  become evidence of safety.
- Public standalone APIs use project-neutral names rather than embedding
  Atuin-specific policy.
- Python-level fuzzy semantics belong here; embedding capabilities, virtual
  filesystems, effect conversion, and allow/block decisions do not.

## Building and testing

The underlying CPython 3.14.7 build instructions and upstream project information
are preserved in [README.original](README.original).

On a supported Unix development host, use the ordinary CPython source build:

```console
./configure
make -j2
./python -X fuzzy -c 'missing_client(); print("continued")'
```

Choose a parallelism value appropriate for the host. Windows builds continue
to use the upstream `PCbuild` workflow documented by CPython; pass `-X fuzzy`
to the resulting interpreter to enable the downstream mode. The fork does not
currently publish installers or binary releases.

After building the fork, run its focused standard/fuzzy regression tests:

```console
./python -m test test_cmd_line test_fuzzy_runtime
```

A fork-owned GitHub Actions workflow performs a shallow checkout, regenerates
and verifies interpreter cases, builds an out-of-tree debug interpreter on
Linux and macOS, and runs the downstream tests plus affected standard-mode
import, scope, builtin, and descriptor suites. A workflow file is evidence of
the intended gate; compatibility is established only after the workflow passes
at the exact downstream commit.

A release candidate must additionally run the normal CPython test suite for
its supported native platforms. The consuming Shell Guard repository separately
runs the candidate through its stock-Node TypeScript WASIp1 host; that lane is
not a substitute for native compatibility testing.

A consuming project's native, WASI, or embedded test lane does not replace
this fork's standard-mode compatibility tests.

The optional downstream WASI checkpoint build has one project extension beyond
WASIp1: the fixed import documented above. It is intentionally separate from
`wasi_snapshot_preview1`; an embedder must opt into its build flag, exact
module/name, and protocol rather than exposing a generic callback surface.

## Compatibility and release policy

The compatibility target is CPython 3.14.7 when fuzzy mode is absent or
explicitly disabled. Fuzzy mode deliberately changes selected failures only in
registered user code, and its recovery/event API remains experimental.

Until a first downstream release is declared, the source branch is the only
fuzzy-cpython deliverable and no stability promise is made for private names.
A future release must identify both its upstream CPython base and exact
downstream commit, publish supported platform/toolchain results, retain CPython
license notices, document event-protocol changes, and distinguish source
compatibility from any separately built Wasm artifact. Updating an embedding
project's pinned binary remains that project's explicit release decision.
Downstream changes are summarized in
[FUZZY_CHANGELOG.md](FUZZY_CHANGELOG.md).

## Upstream and license

This repository is a downstream research fork of
[CPython](https://github.com/python/cpython). It is not an official Python
release and its project-specific changes are not currently intended for upstream
submission.

The imported source corresponds to Python 3.14.7. CPython copyright and license
terms remain in [LICENSE](LICENSE) and the preserved original README.

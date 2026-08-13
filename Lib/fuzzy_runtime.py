"""Runtime state for opt-in fuzzy-cpython execution.

This module is intentionally small while fuzzy semantics migrate into the
fork. It reports the interpreter option captured when the module is imported;
actual recovery hooks read interpreter-owned configuration directly.
"""

import builtins as _builtins
import _fuzzy_runtime as _event_state
import os as _os
import sys as _sys

__all__ = [
    "PROTOCOL_VERSION",
    "DEFAULT_EVENT_LIMIT",
    "DEFAULT_TEXT_LIMIT",
    "DEFAULT_PROVENANCE_DEPTH_LIMIT",
    "FuzzyEventBudgetExceeded",
    "FuzzyModule",
    "FuzzyString",
    "FuzzyValue",
    "activate",
    "configure",
    "is_enabled",
    "get_state",
    "get_events",
    "set_event_sink",
]

PROTOCOL_VERSION = _event_state.PROTOCOL_VERSION
DEFAULT_EVENT_LIMIT = _event_state.DEFAULT_EVENT_LIMIT
DEFAULT_TEXT_LIMIT = _event_state.DEFAULT_TEXT_LIMIT
DEFAULT_PROVENANCE_DEPTH_LIMIT = (
    _event_state.DEFAULT_PROVENANCE_DEPTH_LIMIT
)
_MAX_TRACKED_PATHS = 4096

_option = _sys._xoptions.get("fuzzy")
_enabled = _option is True or _option == "1"
del _option

_activated = False
_user_root = None
_stdlib_root = None
_module_type = type(_sys)
_real_import = _builtins.__import__
_real_import_module = None
_real_os_removedirs = _os.removedirs
_fuzzy_path_type = None
_fuzzy_paths = {}
_fuzzy_string_tokens = {}
_fuzzy_string_provenances = {}
_patched_pathlib = False
_patched_shutil = False


FuzzyEventBudgetExceeded = _event_state.FuzzyEventBudgetExceeded


def configure(*, event_limit=DEFAULT_EVENT_LIMIT,
              text_limit=DEFAULT_TEXT_LIMIT,
              provenance_depth_limit=DEFAULT_PROVENANCE_DEPTH_LIMIT,
              user_root=None):
    """Configure bounded recovery observations before fuzzy work begins.

    Configuration is rejected after an event has been recorded so one run has
    one deterministic set of limits and one user-code root. Passing
    ``user_root`` lets an embedding set that root after interpreter startup
    without activating the runtime a second time.
    """
    if not _enabled:
        raise RuntimeError("fuzzy mode is disabled")
    requested_root = None
    if user_root is not None:
        requested_root = _resolve_user_root(user_root)
        _check_user_root_change(requested_root)
    _event_state.configure(
        event_limit=event_limit,
        text_limit=text_limit,
        provenance_depth_limit=provenance_depth_limit,
    )
    if requested_root is not None:
        _set_user_root(requested_root)


def _bounded_text(name, value, truncated):
    if not isinstance(value, str):
        raise TypeError(name + " must be a string")
    text_limit = _event_state.get_limits()[1]
    if len(value) <= text_limit:
        return value
    truncated.append(name)
    return value[:text_limit]


def set_event_sink(sink):
    """Install one synchronous observer before any event is recorded."""
    _event_state.set_event_sink(sink)


def _emit_event(kind, code, message, provenance, *, operation=None):
    """Record one interpreter recovery event for fork-owned fuzzy hooks."""
    _event_state.emit_event(
        kind, code, message, provenance, operation=operation
    )


def _require_enabled():
    if not _enabled:
        raise RuntimeError("fuzzy mode is disabled")


def _initial_provenance(provenance):
    _require_enabled()
    truncated = []
    bounded = _bounded_text("provenance", provenance, truncated)
    if truncated:
        _emit_event(
            "runtime",
            "fuzzy-provenance-truncated",
            "initial fuzzy provenance exceeded the configured text limit",
            bounded,
        )
    return bounded


class FuzzyString(str):
    """A deterministic string placeholder retaining bounded provenance."""

    def __new__(cls, provenance):
        bounded = _initial_provenance(provenance)
        token = _event_state.path_token(bounded)
        if token not in _fuzzy_string_tokens:
            _fuzzy_string_tokens[token] = bounded
            _fuzzy_string_provenances[bounded] = token
        value = str.__new__(cls, token)
        value._fuzzy_provenance = bounded
        return value

    @property
    def provenance(self):
        return self._fuzzy_provenance


class FuzzyValue:
    """An unknown value that records deterministic single-run fallbacks."""

    __slots__ = ("_fuzzy_provenance", "_fuzzy_depth")

    def __init__(self, provenance, *, _depth=0, _already_bounded=False):
        self._fuzzy_provenance = (
            provenance if _already_bounded else _initial_provenance(provenance)
        )
        self._fuzzy_depth = _depth

    @property
    def provenance(self):
        return self._fuzzy_provenance

    def _warning(self, code, message):
        _emit_event(
            "warning", code, message, self._fuzzy_provenance
        )

    def _derived(self, operation):
        _, text_limit, provenance_depth_limit = _event_state.get_limits()
        if self._fuzzy_depth >= provenance_depth_limit:
            self._warning(
                "fuzzy-provenance-depth-exhausted",
                "fuzzy value derivation retained its bounded provenance",
            )
            return FuzzyValue(
                self._fuzzy_provenance,
                _depth=self._fuzzy_depth,
                _already_bounded=True,
            )
        suffix = "." + operation
        available = text_limit - len(self._fuzzy_provenance)
        if len(suffix) > available:
            self._warning(
                "fuzzy-provenance-truncated",
                "derived fuzzy provenance exceeded the configured text limit",
            )
            provenance = self._fuzzy_provenance
        else:
            provenance = self._fuzzy_provenance + suffix
        return FuzzyValue(
            provenance,
            _depth=self._fuzzy_depth + 1,
            _already_bounded=True,
        )

    def __getattr__(self, name):
        self._warning(
            "unknown-attribute", "unknown attribute " + name
        )
        if name in ("unlink", "rmdir"):
            operation = "Path." + name

            def unresolved_delete(*args, **kwargs):
                _emit_unresolved_delete(
                    operation, self._fuzzy_provenance
                )

            return unresolved_delete
        return self._derived(name)

    def __getitem__(self, key):
        return self._derived("item")

    def __call__(self, *args, **kwargs):
        self._warning("unknown-call", "unknown call was skipped")
        return self._derived("result")

    def __bool__(self):
        self._warning("fuzzy-bool-default", "fuzzy boolean used false")
        return False

    def __iter__(self):
        self._warning(
            "fuzzy-iterable-default", "fuzzy iterable used empty"
        )
        return iter(())

    def __len__(self):
        self._warning("fuzzy-iterable-default", "fuzzy length used zero")
        return 0

    def __int__(self):
        self._warning("fuzzy-numeric-default", "fuzzy integer used zero")
        return 0

    def __index__(self):
        self._warning("fuzzy-numeric-default", "fuzzy index used zero")
        return 0

    def __float__(self):
        self._warning("fuzzy-numeric-default", "fuzzy float used zero")
        return 0.0

    def __str__(self):
        self._warning(
            "fuzzy-string-unresolved", "fuzzy string stayed unresolved"
        )
        return FuzzyString(self._fuzzy_provenance)

    def __fspath__(self):
        self._warning(
            "fuzzy-string-unresolved", "fuzzy path stayed unresolved"
        )
        return FuzzyString(self._fuzzy_provenance)

    def __repr__(self):
        return "<fuzzy " + self._fuzzy_provenance + ">"

    def __contains__(self, item):
        self._warning(
            "fuzzy-bool-default", "fuzzy membership used false"
        )
        return False

    def __enter__(self):
        return self._derived("enter")

    def __exit__(self, exc_type, exc, traceback):
        return False

    def __await__(self):
        if False:
            yield None
        return self._derived("await")

    def __hash__(self):
        self._warning("fuzzy-numeric-default", "fuzzy hash used zero")
        return 0

    def __neg__(self):
        return self._derived("negate")

    def __pos__(self):
        return self._derived("positive")

    def __invert__(self):
        return self._derived("invert")

    def __abs__(self):
        return self._derived("absolute")


def _binary_operation(value, other):
    return value._derived("operation")


for _binary_name in (
    "__add__", "__radd__", "__sub__", "__rsub__", "__mul__", "__rmul__",
    "__truediv__", "__rtruediv__", "__floordiv__", "__rfloordiv__",
    "__mod__", "__rmod__", "__divmod__", "__rdivmod__",
    "__pow__", "__rpow__", "__matmul__", "__rmatmul__",
    "__lshift__", "__rlshift__", "__rshift__", "__rrshift__",
    "__and__", "__rand__", "__or__", "__ror__", "__xor__", "__rxor__",
    "__lt__", "__le__", "__eq__", "__ne__", "__gt__", "__ge__",
):
    setattr(FuzzyValue, _binary_name, _binary_operation)
del _binary_name


def _canonical_path(path):
    try:
        return _os.path.realpath(path)
    except (OSError, ValueError, TypeError):
        return None


def _resolve_user_root(user_root):
    if user_root is None:
        argv0 = _sys.argv[0] if _sys.argv else ""
        if argv0 and argv0 not in ("-", "-c", "-m"):
            user_root = _os.path.dirname(_os.path.abspath(argv0))
        else:
            user_root = _os.getcwd()
    requested_root = _canonical_path(user_root)
    if requested_root is None:
        raise RuntimeError("unable to resolve fuzzy user root")
    return requested_root


def _check_user_root_change(requested_root):
    if requested_root != _user_root and _event_state.has_events():
        raise RuntimeError(
            "fuzzy runtime already observed recovery for another root"
        )


def _set_user_root(requested_root):
    global _user_root
    _check_user_root_change(requested_root)
    _user_root = requested_root


def _path_is_within(path, root):
    if path is None or root is None:
        return False
    try:
        return _os.path.commonpath((path, root)) == root
    except (OSError, ValueError, TypeError):
        return False


def _is_user_filename(filename):
    if filename in ("<stdin>", "<string>", "<fuzzy-python>"):
        return True
    if not isinstance(filename, str) or filename.startswith("<"):
        return False
    candidate = _canonical_path(filename)
    if _path_is_within(candidate, _stdlib_root):
        return False
    return _path_is_within(candidate, _user_root)


def _is_user_globals(namespace):
    if not isinstance(namespace, dict):
        return False
    filename = namespace.get("__file__")
    if filename is not None:
        return _is_user_filename(filename)
    return namespace.get("__name__") == "__main__"


def _is_user_frame(frame):
    return _is_user_globals(frame.f_globals)


def _resolve_requested_name(name, globals, level):
    if level == 0:
        return name
    if not isinstance(globals, dict):
        return name
    package = globals.get("__package__")
    if not isinstance(package, str) or not package:
        return name
    parts = package.split(".")
    if level > len(parts):
        return name
    base = ".".join(parts[:len(parts) - level + 1])
    if not name:
        return base
    return base + "." + name


def _resolve_import_module_name(name, package):
    if not isinstance(name, str):
        return None
    level = len(name) - len(name.lstrip("."))
    if level == 0:
        return name
    if not isinstance(package, str) or not package:
        return None
    bits = package.rsplit(".", level - 1)
    if len(bits) < level:
        return None
    base = bits[0]
    relative = name[level:]
    return base + "." + relative if relative else base


def _is_direct_missing_import(requested, missing):
    return (
        isinstance(requested, str)
        and bool(requested)
        and isinstance(missing, str)
        and (requested == missing or requested.startswith(missing + "."))
    )


def _direct_missing_target(requested, missing, fromlist):
    if _is_direct_missing_import(requested, missing):
        return requested
    if not isinstance(fromlist, (tuple, list)):
        return None
    for item in fromlist:
        if not isinstance(item, str) or not item or item == "*":
            continue
        candidate = requested + "." + item
        if _is_direct_missing_import(candidate, missing):
            return candidate
    return None


class FuzzyModule(_module_type):
    """A directly unresolved user import with lazy fuzzy attributes."""

    def __init__(self, name, doc=None):
        super().__init__(name, doc)
        _event_state.register_untrusted_module(
            self, "unknown-import:" + name
        )


def _ensure_fuzzy_module(full_name):
    parent = None
    parts = full_name.split(".")
    for index in range(1, len(parts) + 1):
        name = ".".join(parts[:index])
        module = _sys.modules.get(name)
        if module is None:
            module = FuzzyModule(name)
            module.__package__ = name
            module.__path__ = []
            _sys.modules[name] = module
        if parent is not None:
            setattr(parent, parts[index - 1], module)
        parent = module
    return parent


def _record_missing_import(target):
    _ensure_fuzzy_module(target)
    provenance = "unknown-import:" + target
    _emit_event(
        "warning",
        "unknown-import",
        "directly missing user import became a fuzzy module",
        provenance,
    )


def _emit_unresolved_delete(operation, provenance):
    _emit_event(
        "unresolved-operation",
        "fuzzy-delete-target-unresolved",
        "delete target stayed unresolved",
        provenance,
        operation=operation,
    )


def _fuzzy_target_provenance(target):
    if isinstance(target, (FuzzyValue, FuzzyString)):
        return target.provenance
    if isinstance(target, str):
        for token, provenance in _fuzzy_string_tokens.items():
            if token in target:
                return provenance
        return None
    if isinstance(target, bytes):
        for token, provenance in _fuzzy_string_tokens.items():
            if token.encode("ascii") in target:
                return provenance
        return None
    if _fuzzy_path_type is not None and isinstance(target, _fuzzy_path_type):
        record = _fuzzy_paths.get(id(target))
        if record is not None and record[0] is target:
            return record[1]
        try:
            raw_paths = object.__getattribute__(target, "_raw_paths")
        except AttributeError:
            raw_paths = ()
        for raw_path in raw_paths:
            if isinstance(raw_path, FuzzyString):
                return raw_path.provenance
    try:
        converted = _os.fspath(target)
    except TypeError:
        return None
    if converted is not target:
        return _fuzzy_target_provenance(converted)
    return None


def _fuzzy_path_provenance(path):
    record = _fuzzy_paths.get(id(path))
    if record is not None and record[0] is path:
        return record[1], record[2]
    provenance = _fuzzy_target_provenance(path)
    if provenance is None:
        return None
    return provenance, 0


def _derive_path_provenance(provenance, depth, operation):
    _, text_limit, provenance_depth_limit = _event_state.get_limits()
    if depth >= provenance_depth_limit:
        _emit_event(
            "runtime",
            "fuzzy-provenance-depth-exhausted",
            "fuzzy path derivation retained its bounded provenance",
            provenance,
        )
        return provenance, depth
    suffix = "." + operation
    if len(provenance) + len(suffix) > text_limit:
        _emit_event(
            "runtime",
            "fuzzy-provenance-truncated",
            "derived fuzzy path provenance exceeded the configured text limit",
            provenance,
        )
        return provenance, depth + 1
    return provenance + suffix, depth + 1


def _remember_fuzzy_path(path, provenance, depth):
    identity = id(path)
    record = _fuzzy_paths.get(identity)
    if record is not None and record[0] is path:
        return
    if len(_fuzzy_paths) >= _MAX_TRACKED_PATHS:
        _emit_event(
            "runtime",
            "fuzzy-path-budget-exhausted",
            "fuzzy path tracking limit reached",
            provenance,
        )
        raise RuntimeError("fuzzy path tracking limit reached")
    # Retain the object so its identity cannot be reused during this run.
    _fuzzy_paths[identity] = (path, provenance, depth)


def _wrap_delete(operation, function):
    def wrapped(path, *args, **kwargs):
        provenance = _fuzzy_target_provenance(path)
        if provenance is not None:
            _emit_unresolved_delete(operation, provenance)
            return None
        return function(path, *args, **kwargs)

    wrapped.__name__ = function.__name__
    wrapped.__qualname__ = function.__qualname__
    wrapped.__doc__ = function.__doc__
    if hasattr(function, "avoids_symlink_attacks"):
        wrapped.avoids_symlink_attacks = function.avoids_symlink_attacks
    return wrapped


def _patch_os_delete_apis():
    replacements = (
        ("removedirs", "os.removedirs", _real_os_removedirs),
    )
    for name, operation, original in replacements:
        wrapped = _wrap_delete(operation, original)
        setattr(_os, name, wrapped)
        for support_name in (
            "supports_dir_fd", "supports_fd", "supports_follow_symlinks"
        ):
            supported = getattr(_os, support_name, ())
            if original in supported:
                supported.add(wrapped)


def _patch_pathlib(module):
    global _fuzzy_path_type, _patched_pathlib
    if _patched_pathlib:
        return
    namespace = module.__dict__
    pure_path = namespace.get("PurePath")
    path_type = namespace.get("Path")
    if pure_path is None or path_type is None:
        return
    real_init = pure_path.__init__
    real_from_parsed_string = pure_path._from_parsed_string
    real_unlink = path_type.unlink
    real_rmdir = path_type.rmdir
    _fuzzy_path_type = pure_path

    def fuzzy_init(path, *segments):
        provenance = None
        depth = 0
        for segment in segments:
            record = _fuzzy_paths.get(id(segment))
            if record is not None and record[0] is segment:
                provenance, depth = _derive_path_provenance(
                    record[1], record[2], "operation"
                )
                break
            provenance = _fuzzy_target_provenance(segment)
            if provenance is not None:
                provenance, depth = _derive_path_provenance(
                    provenance, depth, "path"
                )
                break
        real_init(path, *segments)
        if provenance is None:
            provenance = _fuzzy_target_provenance(path)
        if provenance is not None:
            _remember_fuzzy_path(path, provenance, depth)

    def fuzzy_from_parsed_string(path, path_string):
        result = real_from_parsed_string(path, path_string)
        record = _fuzzy_path_provenance(path)
        if record is not None:
            provenance, depth = _derive_path_provenance(
                record[0], record[1], "operation"
            )
            _remember_fuzzy_path(result, provenance, depth)
        return result

    def fuzzy_unlink(path, missing_ok=False):
        provenance = _fuzzy_target_provenance(path)
        if provenance is not None:
            _emit_unresolved_delete("Path.unlink", provenance)
            return None
        return real_unlink(path, missing_ok=missing_ok)

    def fuzzy_rmdir(path):
        provenance = _fuzzy_target_provenance(path)
        if provenance is not None:
            _emit_unresolved_delete("Path.rmdir", provenance)
            return None
        return real_rmdir(path)

    pure_path.__init__ = fuzzy_init
    pure_path._from_parsed_string = fuzzy_from_parsed_string
    path_type.unlink = fuzzy_unlink
    path_type.rmdir = fuzzy_rmdir
    _patched_pathlib = True


def _patch_shutil(module):
    global _patched_shutil
    if _patched_shutil:
        return
    function = module.__dict__.get("rmtree")
    if function is None:
        return
    module.rmtree = _wrap_delete("shutil.rmtree", function)
    _patched_shutil = True


def _patch_loaded_delete_apis(module_name=None):
    if module_name is None or module_name == "pathlib":
        module = _sys.modules.get("pathlib")
        if module is not None:
            _patch_pathlib(module)
    if module_name is None or module_name == "shutil":
        module = _sys.modules.get("shutil")
        if module is not None:
            _patch_shutil(module)


def _missing_import(name, globals, fromlist, level, missing):
    user_import = _is_user_globals(globals)
    requested = _resolve_requested_name(name, globals, level)
    target = _direct_missing_target(requested, missing, fromlist)
    if not user_import or target is None or not requested:
        return NotImplemented
    return target, requested


def _fuzzy_import_post(result, name, globals=None, locals=None,
                       fromlist=(), level=0):
    user_import = _is_user_globals(globals)
    requested = _resolve_requested_name(name, globals, level)
    if user_import and level > 0 and not name and fromlist:
        namespace = getattr(result, "__dict__", {})
        for item in fromlist:
            if (
                isinstance(item, str)
                and item
                and item != "*"
                and item not in namespace
            ):
                _record_missing_import(requested + "." + item)
    if level == 0 and isinstance(name, str):
        root_name = name.partition(".")[0]
        if root_name == "importlib":
            _patch_importlib(_sys.modules.get("importlib"))
        _patch_loaded_delete_apis(root_name)
    return result


def _fuzzy_import_opcode(name, globals=None, locals=None, fromlist=(), level=0):
    result = _real_import(name, globals, locals, fromlist, level)
    return _fuzzy_import_post(
        result, name, globals, locals, fromlist, level
    )


def _fuzzy_import(name, globals=None, locals=None, fromlist=(), level=0):
    try:
        return _fuzzy_import_opcode(name, globals, locals, fromlist, level)
    except ModuleNotFoundError as error:
        decision = _missing_import(
            name, globals, fromlist, level, error.name
        )
        if decision is NotImplemented:
            raise
        target, requested = decision
        _record_missing_import(target)
        if fromlist:
            return _sys.modules[requested]
        return _sys.modules[target.split(".", 1)[0]]


def _fuzzy_import_module(name, package=None):
    if _real_import_module is None:
        raise RuntimeError("importlib fuzzy wrapper is not initialized")
    user_import = _is_user_frame(_sys._getframe(1))
    requested = _resolve_import_module_name(name, package)
    try:
        result = _real_import_module(name, package)
    except ModuleNotFoundError as error:
        target = _direct_missing_target(requested, error.name, ())
        if not user_import or target is None:
            raise
        _record_missing_import(target)
        return _sys.modules[target]
    if requested is not None:
        _patch_loaded_delete_apis(requested.partition(".")[0])
    return result


def _patch_importlib(module):
    global _real_import_module
    if _real_import_module is not None or module is None:
        return
    function = module.__dict__.get("import_module")
    if function is None:
        return
    _real_import_module = function
    _fuzzy_import_module.__name__ = function.__name__
    _fuzzy_import_module.__qualname__ = function.__qualname__
    _fuzzy_import_module.__doc__ = function.__doc__
    _fuzzy_import_module.__module__ = function.__module__
    module.import_module = _fuzzy_import_module


def _missing_attribute(owner, name):
    if (
        name.startswith("__")
        and name.endswith("__")
        or not _is_user_frame(_sys._getframe(1))
    ):
        return NotImplemented
    return True


def _missing_name(name, is_local):
    frame = _sys._getframe(1)
    if not _is_user_frame(frame):
        return NotImplemented
    return True


def activate(*, user_root=None):
    """Install fork-owned recovery hooks for one explicitly fuzzy process."""
    global _activated, _stdlib_root
    _require_enabled()
    requested_root = _resolve_user_root(user_root)
    if _activated:
        _set_user_root(requested_root)
        return
    stdlib_dir = getattr(_sys, "_stdlib_dir", None)
    if not isinstance(stdlib_dir, str) or not stdlib_dir:
        version_dir = "python" + str(_sys.version_info.major) + "." + str(
            _sys.version_info.minor
        )
        stdlib_dir = _os.path.join(_sys.base_prefix, "lib", version_dir)
    _stdlib_root = _canonical_path(stdlib_dir)
    if _stdlib_root is None:
        raise RuntimeError("unable to resolve fuzzy stdlib root")
    _set_user_root(requested_root)
    _patch_os_delete_apis()
    _patch_loaded_delete_apis()
    _patch_importlib(_sys.modules.get("importlib"))
    _builtins.__import__ = _fuzzy_import
    _builtins._fuzzy_missing_attribute = _missing_attribute
    _builtins._fuzzy_missing_name = _missing_name
    _activated = True


def is_enabled():
    """Return whether fuzzy runtime state is enabled."""
    return _enabled


def get_state():
    """Return a new, JSON-compatible description of public runtime state."""
    state = {
        "enabled": _enabled,
        "protocol_version": PROTOCOL_VERSION,
        "activated": _activated,
        "user_root": _user_root,
        "stdlib_root": _stdlib_root,
    }
    state.update(_event_state.get_event_state())
    return state


def get_events():
    """Return defensive copies of ordered, JSON-compatible recovery events."""
    return _event_state.get_events()

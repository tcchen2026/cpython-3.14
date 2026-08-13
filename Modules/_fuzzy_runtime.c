/* Core state, startup, and recovery for the opt-in fuzzy-cpython runtime. */

#include "Python.h"
#include "pycore_call.h"          // _PyObject_CallNoArgs()
#include "pycore_descrobject.h"   // PyMemberDescr_Type
#include "pycore_fuzzy.h"         // _PyFuzzy_GetMode()
#include "pycore_interp.h"        // _PyInterpreterState_GetConfig()
#include "pycore_import.h"        // _PyImport_SetModule()
#include "pycore_interpframe.h"   // _PyFrame_GetCode()
#include "pycore_pystate.h"       // _PyInterpreterState_GET()

/* Return 1 when fuzzy mode is enabled, 0 when disabled, and -1 on error. */
static int
fuzzy_option(const PyConfig *config)
{
    int mode = _PyFuzzy_GetMode(config);
    if (mode < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "-X fuzzy must be specified without a value or as "
                        "-X fuzzy=0 or -X fuzzy=1");
    }
    return mode;
}


int
_PyFuzzy_InitializeRuntime(const PyConfig *config)
{
    int mode = fuzzy_option(config);
    if (mode <= 0) {
        return mode;
    }

    /* Execute the fork-owned frozen runtime directly.  Do not let sys.path,
       PYTHONPATH, the entry script, or cwd select this startup dependency. */
    int imported = PyImport_ImportFrozenModule("fuzzy_runtime");
    if (imported <= 0) {
        if (imported == 0) {
            PyErr_SetString(PyExc_RuntimeError,
                            "fuzzy-cpython requires its frozen runtime");
        }
        return -1;
    }
    PyObject *module = PyImport_ImportModule("fuzzy_runtime");
    if (module == NULL) {
        return -1;
    }
    PyObject *activate = PyObject_GetAttrString(module, "activate");
    if (activate == NULL) {
        Py_DECREF(module);
        return -1;
    }
    PyObject *result = _PyObject_CallNoArgs(activate);
    Py_DECREF(activate);
    if (result == NULL) {
        Py_DECREF(module);
        return -1;
    }
    Py_DECREF(result);
    PyObject *missing_attribute = NULL;
    PyObject *missing_name = NULL;
    PyObject *value_factory = NULL;
    PyObject *import_func = NULL;
    PyObject *import_post = NULL;
    PyObject *missing_import = NULL;
    PyObject *module_type = NULL;
    missing_attribute = PyObject_GetAttrString(module, "_missing_attribute");
    if (missing_attribute != NULL) {
        missing_name = PyObject_GetAttrString(module, "_missing_name");
    }
    if (missing_name != NULL) {
        value_factory = PyObject_GetAttrString(module, "FuzzyValue");
    }
    if (value_factory != NULL) {
        import_func = PyObject_GetAttrString(module, "_real_import");
    }
    if (import_func != NULL) {
        import_post = PyObject_GetAttrString(module, "_fuzzy_import_post");
    }
    if (import_post != NULL) {
        missing_import = PyObject_GetAttrString(module, "_missing_import");
    }
    if (missing_import != NULL) {
        module_type = PyObject_GetAttrString(module, "FuzzyModule");
    }
    Py_DECREF(module);
    int installed = -1;
    if (module_type != NULL) {
        installed = _PyFuzzy_InstallHooks(
            _PyThreadState_GET(), missing_attribute, missing_name,
            value_factory, import_func, import_post, missing_import,
            module_type);
    }
    Py_XDECREF(missing_attribute);
    Py_XDECREF(missing_name);
    Py_XDECREF(value_factory);
    Py_XDECREF(import_func);
    Py_XDECREF(import_post);
    Py_XDECREF(missing_import);
    Py_XDECREF(module_type);
    return installed;
}

/*
 * Downstream fuzzy-cpython recovery helper.  Interpreter attribute-load sites
 * call this only after ordinary lookup raises AttributeError.  General C API
 * callers retain ordinary CPython semantics.
 */
int
_PyFuzzy_InstallHooks(PyThreadState *tstate, PyObject *missing_attribute,
                      PyObject *missing_name, PyObject *value_factory,
                      PyObject *import_func, PyObject *import_post,
                      PyObject *missing_import, PyObject *module_type)
{
    PyInterpreterState *interp = tstate->interp;
    const PyConfig *config = _PyInterpreterState_GetConfig(interp);
    if (_PyFuzzy_GetMode(config) != 1) {
        PyErr_SetString(PyExc_RuntimeError,
                        "cannot install fuzzy hooks when fuzzy mode is disabled");
        return -1;
    }
    if (!PyCallable_Check(missing_attribute)
            || !PyCallable_Check(missing_name)
            || !PyType_Check(value_factory)
            || !PyCallable_Check(import_func)
            || !PyCallable_Check(import_post)
            || !PyCallable_Check(missing_import)
            || !PyType_Check(module_type)) {
        PyErr_SetString(
            PyExc_TypeError,
            "fuzzy recovery hooks must be callable and the value and module types must be types");
        return -1;
    }
    if (!PyType_IsSubtype((PyTypeObject *)module_type, &PyModule_Type)) {
        PyErr_SetString(PyExc_TypeError,
                        "fuzzy module type must inherit from module");
        return -1;
    }
    if (interp->fuzzy.missing_attribute != NULL
            || interp->fuzzy.missing_name != NULL
            || interp->fuzzy.value_factory != NULL
            || interp->fuzzy.value_provenance_member != NULL
            || interp->fuzzy.value_depth_member != NULL
            || interp->fuzzy.import_func != NULL
            || interp->fuzzy.import_post != NULL
            || interp->fuzzy.missing_import != NULL
            || interp->fuzzy.module_type != NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy recovery hooks are already installed");
        return -1;
    }
    PyObject *value_provenance_member = PyObject_GetAttrString(
        value_factory, "_fuzzy_provenance");
    PyObject *value_depth_member = PyObject_GetAttrString(
        value_factory, "_fuzzy_depth");
    if (value_provenance_member == NULL || value_depth_member == NULL) {
        Py_XDECREF(value_provenance_member);
        Py_XDECREF(value_depth_member);
        return -1;
    }
    if (!Py_IS_TYPE(value_provenance_member, &PyMemberDescr_Type)
            || !Py_IS_TYPE(value_depth_member, &PyMemberDescr_Type)
            || PyDescr_TYPE(value_provenance_member)
                != (PyTypeObject *)value_factory
            || PyDescr_TYPE(value_depth_member)
                != (PyTypeObject *)value_factory) {
        Py_DECREF(value_provenance_member);
        Py_DECREF(value_depth_member);
        PyErr_SetString(PyExc_TypeError,
                        "fuzzy value state must use exact member descriptors");
        return -1;
    }
    interp->fuzzy.missing_attribute = Py_NewRef(missing_attribute);
    interp->fuzzy.missing_name = Py_NewRef(missing_name);
    interp->fuzzy.value_factory = Py_NewRef(value_factory);
    interp->fuzzy.value_provenance_member = value_provenance_member;
    interp->fuzzy.value_depth_member = value_depth_member;
    interp->fuzzy.import_func = Py_NewRef(import_func);
    interp->fuzzy.import_post = Py_NewRef(import_post);
    interp->fuzzy.missing_import = Py_NewRef(missing_import);
    interp->fuzzy.module_type = Py_NewRef(module_type);
    return 0;
}

void
_PyFuzzy_Clear(PyInterpreterState *interp)
{
    Py_CLEAR(interp->fuzzy.missing_attribute);
    Py_CLEAR(interp->fuzzy.missing_name);
    Py_CLEAR(interp->fuzzy.value_factory);
    Py_CLEAR(interp->fuzzy.value_provenance_member);
    Py_CLEAR(interp->fuzzy.value_depth_member);
    Py_CLEAR(interp->fuzzy.import_func);
    Py_CLEAR(interp->fuzzy.import_post);
    Py_CLEAR(interp->fuzzy.missing_import);
    Py_CLEAR(interp->fuzzy.module_type);
    Py_CLEAR(interp->fuzzy.events);
    Py_CLEAR(interp->fuzzy.event_sink);
    Py_CLEAR(interp->fuzzy.event_budget_error);
    Py_CLEAR(interp->fuzzy.path_provenances);
    Py_CLEAR(interp->fuzzy.provenance_tokens);
    Py_CLEAR(interp->fuzzy.trusted_value_objects);
    Py_CLEAR(interp->fuzzy.trusted_value_provenances);
    Py_CLEAR(interp->fuzzy.trusted_path_objects);
    Py_CLEAR(interp->fuzzy.trusted_path_provenances);
    Py_CLEAR(interp->fuzzy.trusted_module_objects);
    Py_CLEAR(interp->fuzzy.trusted_module_provenances);
    Py_CLEAR(interp->fuzzy.untrusted_module_objects);
    Py_CLEAR(interp->fuzzy.untrusted_module_provenances);
    interp->fuzzy.event_limit = 0;
    interp->fuzzy.text_limit = 0;
    interp->fuzzy.provenance_depth_limit = 0;
    interp->fuzzy.event_budget_exhausted = 0;
    interp->fuzzy.event_state_initialized = 0;
}

static int
fuzzy_recovery_decision(PyObject *decision)
{
    if (decision == Py_NotImplemented) {
        return 0;
    }
    if (decision != Py_True) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy recovery hook returned an invalid decision");
        return -1;
    }
    return 1;
}

static PyObject *
fuzzy_value_after_event(PyThreadState *tstate, const char *code_text,
                        PyObject *message, PyObject *provenance, int trusted)
{
    PyObject *kind = PyUnicode_FromString("warning");
    PyObject *code = PyUnicode_FromString(code_text);
    if (kind == NULL || code == NULL) {
        Py_XDECREF(kind);
        Py_XDECREF(code);
        return NULL;
    }
    int emitted = trusted
        ? _PyFuzzy_EmitTrustedEvent(
            tstate->interp, kind, code, message, provenance, Py_None)
        : _PyFuzzy_EmitEvent(
            tstate->interp, kind, code, message, provenance, Py_None);
    Py_DECREF(kind);
    Py_DECREF(code);
    if (emitted < 0) {
        return NULL;
    }
    PyObject *value = _PyFuzzy_NewValue(tstate->interp, provenance, 0);
    if (trusted && value != NULL
            && _PyFuzzy_RegisterTrustedValue(
                tstate->interp, value, provenance) < 0) {
        Py_CLEAR(value);
    }
    return value;
}

static PyObject *
fuzzy_attribute_value(PyThreadState *tstate, PyObject *owner, PyObject *name)
{
    int trusted = 1;
    PyObject *import_provenance = NULL;
    int module_origin = 0;
    if (PyModule_Check(owner)) {
        module_origin = _PyFuzzy_ModuleProvenance(
            tstate->interp, owner, &import_provenance);
        if (module_origin < 0) {
            return NULL;
        }
    }
    if (module_origin > 0) {
        PyObject *message = PyUnicode_FromFormat(
            "unknown imported attribute %U", name);
        PyObject *provenance = PyUnicode_FromFormat(
            "%U.%U", import_provenance, name);
        if (message == NULL || provenance == NULL) {
            Py_XDECREF(message);
            Py_XDECREF(provenance);
            return NULL;
        }
        trusted = module_origin == 2;
        PyObject *value = fuzzy_value_after_event(
            tstate, "unknown-attribute", message, provenance, trusted);
        Py_DECREF(message);
        Py_DECREF(provenance);
        return value;
    }

    PyObject *owner_name;
    if (PyModule_Check(owner)) {
        PyObject *module_name = PyDict_GetItemString(
            PyModule_GetDict(owner), "__name__");
        owner_name = module_name != NULL
            ? Py_NewRef(module_name)
            : PyUnicode_FromString("module");
    }
    else {
        owner_name = PyType_GetName(Py_TYPE(owner));
    }
    if (owner_name == NULL) {
        return NULL;
    }
    if (!PyUnicode_Check(owner_name)) {
        Py_DECREF(owner_name);
        PyErr_SetString(PyExc_TypeError,
                        "fuzzy recovery owner name must be a string");
        return NULL;
    }
    PyObject *message = PyUnicode_FromFormat(
        "unknown object attribute %U", name);
    PyObject *provenance = PyUnicode_FromFormat(
        "unknown-attribute:%U.%U", owner_name, name);
    Py_DECREF(owner_name);
    if (message == NULL || provenance == NULL) {
        Py_XDECREF(message);
        Py_XDECREF(provenance);
        return NULL;
    }
    PyObject *value = fuzzy_value_after_event(
        tstate, "unknown-attribute", message, provenance, trusted);
    Py_DECREF(message);
    Py_DECREF(provenance);
    return value;
}

static PyObject *
fuzzy_name_value(PyThreadState *tstate, PyObject *name, int is_local)
{
    PyObject *message;
    PyObject *provenance;
    if (is_local) {
        _PyInterpreterFrame *frame = tstate->current_frame;
        if (frame == NULL) {
            PyErr_SetString(PyExc_RuntimeError,
                            "fuzzy local recovery has no current frame");
            return NULL;
        }
        PyObject *frame_name = _PyFrame_GetCode(frame)->co_name;
        message = PyUnicode_FromString(
            "uninitialized local became a fuzzy value");
        provenance = PyUnicode_FromFormat(
            "unknown-local:%U.%U", frame_name, name);
    }
    else {
        message = PyUnicode_FromString("unknown name became a fuzzy value");
        provenance = PyUnicode_FromFormat("unknown-name:%U", name);
    }
    if (message == NULL || provenance == NULL) {
        Py_XDECREF(message);
        Py_XDECREF(provenance);
        return NULL;
    }
    PyObject *value = fuzzy_value_after_event(
        tstate, "unknown-name", message, provenance, 1);
    Py_DECREF(message);
    Py_DECREF(provenance);
    return value;
}

static int
fuzzy_is_direct_import_target(PyObject *target, PyObject *missing)
{
    if (!PyUnicode_Check(target) || PyUnicode_GetLength(target) == 0
            || !PyUnicode_Check(missing)
            || PyUnicode_GetLength(missing) == 0) {
        return 0;
    }
    int equal = PyObject_RichCompareBool(target, missing, Py_EQ);
    if (equal != 0) {
        return equal;
    }
    Py_ssize_t target_length = PyUnicode_GetLength(target);
    Py_ssize_t missing_length = PyUnicode_GetLength(missing);
    if (target_length <= missing_length
            || PyUnicode_ReadChar(target, missing_length) != '.') {
        return 0;
    }
    Py_ssize_t found = PyUnicode_Find(
        target, missing, 0, target_length, 1);
    return found < 0 ? (PyErr_Occurred() ? -1 : 0) : found == 0;
}

static PyObject *
fuzzy_new_import_module(PyThreadState *tstate, PyObject *name)
{
    PyInterpreterState *interp = tstate->interp;
    PyObject *module_type = interp->fuzzy.module_type;
    if (module_type == NULL || !PyType_Check(module_type)
            || !PyType_IsSubtype(
                (PyTypeObject *)module_type, &PyModule_Type)
            || PyModule_Type.tp_new == NULL
            || PyModule_Type.tp_init == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy module type is not installed");
        return NULL;
    }
    PyObject *empty = PyTuple_New(0);
    if (empty == NULL) {
        return NULL;
    }
    PyObject *module = PyModule_Type.tp_new(
        (PyTypeObject *)module_type, empty, NULL);
    Py_DECREF(empty);
    if (module == NULL) {
        return NULL;
    }
    PyObject *args = PyTuple_Pack(1, name);
    if (args == NULL
            || PyModule_Type.tp_init(module, args, NULL) < 0) {
        Py_XDECREF(args);
        Py_DECREF(module);
        return NULL;
    }
    Py_DECREF(args);
    if (!Py_IS_TYPE(module, (PyTypeObject *)module_type)) {
        Py_DECREF(module);
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy module allocation returned an invalid module");
        return NULL;
    }
    PyObject *namespace = PyModule_GetDict(module);
    PyObject *path = PyList_New(0);
    if (namespace == NULL || path == NULL
            || PyDict_SetItemString(namespace, "__package__", name) < 0
            || PyDict_SetItemString(namespace, "__path__", path) < 0) {
        Py_XDECREF(path);
        Py_DECREF(module);
        return NULL;
    }
    Py_DECREF(path);
    return module;
}

static PyObject *
fuzzy_create_import_modules(PyThreadState *tstate, PyObject *target)
{
    PyInterpreterState *interp = tstate->interp;
    Py_ssize_t length = PyUnicode_GetLength(target);
    if (length < 0) {
        return NULL;
    }
    PyObject *parent = NULL;
    PyObject *result = NULL;
    Py_ssize_t component_start = 0;
    for (Py_ssize_t end = 1; end <= length; end++) {
        if (end < length && PyUnicode_ReadChar(target, end) != '.') {
            continue;
        }
        PyObject *name = PyUnicode_Substring(target, 0, end);
        if (name == NULL) {
            goto error;
        }
        PyObject *module = PyImport_GetModule(name);
        if (module == NULL) {
            if (PyErr_Occurred()) {
                Py_DECREF(name);
                goto error;
            }
            module = fuzzy_new_import_module(tstate, name);
            PyObject *provenance = PyUnicode_FromFormat(
                "unknown-import:%U", name);
            if (module == NULL || provenance == NULL
                    || _PyFuzzy_RegisterTrustedModule(
                        interp, module, provenance) < 0
                    || _PyImport_SetModule(name, module) < 0) {
                Py_XDECREF(provenance);
                Py_XDECREF(module);
                Py_DECREF(name);
                goto error;
            }
            Py_DECREF(provenance);
        }
        if (!PyModule_Check(module)) {
            Py_DECREF(module);
            Py_DECREF(name);
            PyErr_SetString(PyExc_RuntimeError,
                            "fuzzy import parent is not a module");
            goto error;
        }
        if (parent != NULL) {
            PyObject *component = PyUnicode_Substring(
                target, component_start, end);
            PyObject *namespace = PyModule_GetDict(parent);
            if (component == NULL || namespace == NULL
                    || PyDict_SetItem(namespace, component, module) < 0) {
                Py_XDECREF(component);
                Py_DECREF(module);
                Py_DECREF(name);
                goto error;
            }
            Py_DECREF(component);
        }
        Py_XSETREF(parent, module);
        if (end == length) {
            PyObject *provenance;
            int origin = _PyFuzzy_ModuleProvenance(
                interp, parent, &provenance);
            if (origin != 2) {
                Py_DECREF(name);
                if (origin >= 0) {
                    PyErr_SetString(
                        PyExc_RuntimeError,
                        "fuzzy import target was not created by trusted recovery");
                }
                goto error;
            }
            result = Py_NewRef(parent);
        }
        Py_DECREF(name);
        component_start = end + 1;
    }
    Py_XDECREF(parent);
    if (result == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy import recovery omitted its target module");
    }
    return result;

error:
    Py_XDECREF(parent);
    Py_XDECREF(result);
    return NULL;
}

PyObject *
_PyObject_FuzzyMissingImport(PyThreadState *tstate, PyObject *name,
                             PyObject *globals, PyObject *fromlist,
                             PyObject *level)
{
    if (!PyErr_ExceptionMatches(PyExc_ModuleNotFoundError)) {
        return NULL;
    }
    PyInterpreterState *interp = tstate->interp;
    const PyConfig *config = _PyInterpreterState_GetConfig(interp);
    if (_PyFuzzy_GetMode(config) != 1
            || interp->fuzzy.missing_import == NULL
            || interp->fuzzy.module_type == NULL) {
        return NULL;
    }

    PyObject *original = PyErr_GetRaisedException();
    if (original == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy import recovery lost its original exception");
        return NULL;
    }
    PyObject *missing = PyObject_GetAttrString(original, "name");
    if (missing == NULL) {
        Py_DECREF(original);
        return NULL;
    }
    PyObject *hook = Py_NewRef(interp->fuzzy.missing_import);
    PyObject *decision = PyObject_CallFunctionObjArgs(
        hook, name, globals != NULL ? globals : Py_None, fromlist, level,
        missing, NULL);
    Py_DECREF(hook);
    if (decision == NULL) {
        Py_DECREF(missing);
        Py_DECREF(original);
        return NULL;
    }
    if (decision == Py_NotImplemented) {
        Py_DECREF(decision);
        Py_DECREF(missing);
        PyErr_SetRaisedException(original);
        return NULL;
    }
    if (!PyTuple_CheckExact(decision) || PyTuple_GET_SIZE(decision) != 2) {
        Py_DECREF(decision);
        Py_DECREF(missing);
        Py_DECREF(original);
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy missing-import hook returned an invalid decision");
        return NULL;
    }
    PyObject *target = Py_NewRef(PyTuple_GET_ITEM(decision, 0));
    PyObject *requested = Py_NewRef(PyTuple_GET_ITEM(decision, 1));
    Py_DECREF(decision);
    int direct = fuzzy_is_direct_import_target(target, missing);
    Py_DECREF(missing);
    if (direct <= 0 || !PyUnicode_Check(requested)
            || PyUnicode_GetLength(requested) == 0) {
        Py_DECREF(target);
        Py_DECREF(requested);
        Py_DECREF(original);
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_RuntimeError,
                            "fuzzy missing-import hook returned invalid names");
        }
        return NULL;
    }
    Py_DECREF(original);

    PyObject *kind = PyUnicode_FromString("warning");
    PyObject *code = PyUnicode_FromString("unknown-import");
    PyObject *message = PyUnicode_FromString(
        "directly missing user import became a fuzzy module");
    PyObject *provenance = PyUnicode_FromFormat(
        "unknown-import:%U", target);
    if (kind == NULL || code == NULL || message == NULL || provenance == NULL) {
        Py_XDECREF(kind);
        Py_XDECREF(code);
        Py_XDECREF(message);
        Py_XDECREF(provenance);
        Py_DECREF(target);
        Py_DECREF(requested);
        return NULL;
    }
    int emitted = _PyFuzzy_EmitTrustedEvent(
        interp, kind, code, message, provenance, Py_None);
    Py_DECREF(kind);
    Py_DECREF(code);
    Py_DECREF(message);
    Py_DECREF(provenance);
    if (emitted < 0) {
        Py_DECREF(target);
        Py_DECREF(requested);
        return NULL;
    }

    PyObject *created = fuzzy_create_import_modules(tstate, target);
    if (created == NULL) {
        Py_DECREF(target);
        Py_DECREF(requested);
        return NULL;
    }
    Py_DECREF(created);

    int has_fromlist = PyObject_IsTrue(fromlist);
    if (has_fromlist < 0) {
        Py_DECREF(target);
        Py_DECREF(requested);
        return NULL;
    }
    PyObject *result_name;
    if (has_fromlist) {
        result_name = Py_NewRef(requested);
    }
    else {
        Py_ssize_t requested_length = PyUnicode_GetLength(requested);
        Py_ssize_t dot = PyUnicode_FindChar(
            requested, '.', 0, requested_length, 1);
        if (dot < 0 && PyErr_Occurred()) {
            Py_DECREF(target);
            Py_DECREF(requested);
            return NULL;
        }
        result_name = dot < 0
            ? Py_NewRef(requested)
            : PyUnicode_Substring(requested, 0, dot);
    }
    Py_DECREF(target);
    Py_DECREF(requested);
    if (result_name == NULL) {
        return NULL;
    }
    PyObject *result = PyImport_GetModule(result_name);
    Py_DECREF(result_name);
    if (result == NULL && !PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy import recovery omitted its result module");
    }
    return result;
}

PyObject *
_PyObject_FuzzyMissingAttribute(PyObject *owner, PyObject *name)
{
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
        return NULL;
    }

    PyThreadState *tstate = _PyThreadState_GET();
    const PyConfig *config = _PyInterpreterState_GetConfig(tstate->interp);
    if (_PyFuzzy_GetMode(config) != 1) {
        return NULL;
    }

    PyObject *hook = tstate->interp->fuzzy.missing_attribute;
    if (hook == NULL) {
        return NULL;
    }

    PyObject *original_exception = PyErr_GetRaisedException();
    Py_INCREF(hook);
    PyObject *result = PyObject_CallFunctionObjArgs(hook, owner, name, NULL);
    Py_DECREF(hook);
    if (result == NULL) {
        Py_DECREF(original_exception);
        return NULL;
    }
    int decision = fuzzy_recovery_decision(result);
    Py_DECREF(result);
    if (decision == 0) {
        PyErr_SetRaisedException(original_exception);
        return NULL;
    }
    Py_DECREF(original_exception);
    if (decision < 0) {
        return NULL;
    }
    return fuzzy_attribute_value(tstate, owner, name);
}

PyObject *
_PyObject_FuzzyMissingName(PyObject *name, int is_local)
{
    PyThreadState *tstate = _PyThreadState_GET();
    const PyConfig *config = _PyInterpreterState_GetConfig(tstate->interp);
    if (_PyFuzzy_GetMode(config) != 1) {
        return NULL;
    }

    PyObject *hook = tstate->interp->fuzzy.missing_name;
    if (hook == NULL) {
        return NULL;
    }
    Py_INCREF(hook);
    PyObject *local = is_local ? Py_True : Py_False;
    PyObject *result = PyObject_CallFunctionObjArgs(hook, name, local, NULL);
    Py_DECREF(hook);
    if (result == NULL) {
        return NULL;
    }
    int decision = fuzzy_recovery_decision(result);
    Py_DECREF(result);
    if (decision == 0) {
        return NULL;
    }
    if (decision < 0) {
        return NULL;
    }
    return fuzzy_name_value(tstate, name, is_local);
}


#define FUZZY_PROTOCOL_VERSION 1
#define FUZZY_DEFAULT_EVENT_LIMIT 256
#define FUZZY_DEFAULT_TEXT_LIMIT 512
#define FUZZY_DEFAULT_PROVENANCE_DEPTH_LIMIT 16
#define FUZZY_MAX_EVENT_LIMIT 4096
#define FUZZY_MAX_TEXT_LIMIT 4096
#define FUZZY_MAX_PROVENANCE_DEPTH_LIMIT 64
#define FUZZY_MAX_TRACKED_PATHS 4096
#define FUZZY_MAX_PATH_TOKEN_CANDIDATES 64
#define FUZZY_CHECKPOINT_HEADER_SIZE 32
#define FUZZY_CHECKPOINT_MAX_BYTES 32768

#if defined(__wasi__) && defined(PY_FUZZY_CPYTHON_WASI_CHECKPOINT)
__attribute__((
    import_module("fuzzy_cpython_preview1"),
    import_name("checkpoint")
))
extern int fuzzy_cpython_checkpoint(const unsigned char *, size_t);
#endif

#if defined(__wasi__) && defined(PY_FUZZY_CPYTHON_WASI_CHECKPOINT)
static void
fuzzy_write_u32_le(unsigned char *target, uint32_t value)
{
    target[0] = (unsigned char)(value & 0xff);
    target[1] = (unsigned char)((value >> 8) & 0xff);
    target[2] = (unsigned char)((value >> 16) & 0xff);
    target[3] = (unsigned char)((value >> 24) & 0xff);
}

static int
fuzzy_checkpoint_field(PyObject *value, const char **text, Py_ssize_t *length)
{
    *text = PyUnicode_AsUTF8AndSize(value, length);
    if (*text == NULL) {
        return -1;
    }
    if (*length < 0 || (size_t)*length > UINT32_MAX) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy checkpoint field is too large");
        return -1;
    }
    return 0;
}
#endif

static int
fuzzy_checkpoint_event(Py_ssize_t sequence, PyObject *kind, PyObject *code,
                       PyObject *message, PyObject *provenance,
                       PyObject *operation)
{
#if !defined(__wasi__) || !defined(PY_FUZZY_CPYTHON_WASI_CHECKPOINT)
    return 0;
#else
    const char *texts[5];
    Py_ssize_t lengths[5];
    if (fuzzy_checkpoint_field(kind, &texts[0], &lengths[0]) < 0
            || fuzzy_checkpoint_field(code, &texts[1], &lengths[1]) < 0
            || fuzzy_checkpoint_field(message, &texts[2], &lengths[2]) < 0
            || fuzzy_checkpoint_field(
                provenance, &texts[3], &lengths[3]) < 0) {
        return -1;
    }
    if (operation == Py_None) {
        texts[4] = NULL;
        lengths[4] = -1;
    }
    else if (fuzzy_checkpoint_field(
                 operation, &texts[4], &lengths[4]) < 0) {
        return -1;
    }
    size_t total = FUZZY_CHECKPOINT_HEADER_SIZE;
    for (int index = 0; index < 5; index++) {
        if (lengths[index] >= 0) {
            total += (size_t)lengths[index];
        }
    }
    if (sequence < 0 || (size_t)sequence > UINT32_MAX
            || total > FUZZY_CHECKPOINT_MAX_BYTES) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy checkpoint exceeds its wire bound");
        return -1;
    }
    unsigned char *wire = PyMem_Malloc(total);
    if (wire == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    memcpy(wire, "FCP1", 4);
    wire[4] = FUZZY_PROTOCOL_VERSION;
    wire[5] = 0;
    wire[6] = 0;
    wire[7] = 0;
    fuzzy_write_u32_le(wire + 8, (uint32_t)sequence);
    for (int index = 0; index < 5; index++) {
        uint32_t length = lengths[index] < 0
            ? UINT32_MAX
            : (uint32_t)lengths[index];
        fuzzy_write_u32_le(wire + 12 + index * 4, length);
    }
    size_t cursor = FUZZY_CHECKPOINT_HEADER_SIZE;
    for (int index = 0; index < 5; index++) {
        if (lengths[index] > 0) {
            memcpy(wire + cursor, texts[index], (size_t)lengths[index]);
            cursor += (size_t)lengths[index];
        }
    }
    int result = fuzzy_cpython_checkpoint(wire, total);
    PyMem_Free(wire);
    if (result != 0) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy checkpoint host rejected an event");
        return -1;
    }
    return 0;
#endif
}

static int
fuzzy_require_enabled(PyInterpreterState *interp)
{
    const PyConfig *config = _PyInterpreterState_GetConfig(interp);
    if (_PyFuzzy_GetMode(config) != 1) {
        PyErr_SetString(PyExc_RuntimeError, "fuzzy mode is disabled");
        return -1;
    }
    return 0;
}

static int
fuzzy_require_state(PyInterpreterState *interp)
{
    if (!interp->fuzzy.event_state_initialized
            || interp->fuzzy.events == NULL
            || interp->fuzzy.event_budget_error == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy runtime state is not initialized");
        return -1;
    }
    return 0;
}

static int
fuzzy_set_dict_item(PyObject *dict, const char *name, PyObject *value)
{
    if (value == NULL) {
        return -1;
    }
    int result = PyDict_SetItemString(dict, name, value);
    Py_DECREF(value);
    return result;
}

static int
fuzzy_parse_limit(PyObject *value, const char *name, Py_ssize_t minimum,
                  Py_ssize_t maximum, Py_ssize_t *result)
{
    if (!PyLong_Check(value) || PyBool_Check(value)) {
        PyErr_Format(PyExc_TypeError, "%s must be an integer", name);
        return -1;
    }
    Py_ssize_t parsed = PyLong_AsSsize_t(value);
    if (parsed == -1 && PyErr_Occurred()) {
        return -1;
    }
    if (parsed < minimum || parsed > maximum) {
        PyErr_Format(PyExc_ValueError,
                     "%s must be between %zd and %zd",
                     name, minimum, maximum);
        return -1;
    }
    *result = parsed;
    return 0;
}

static PyObject *
fuzzy_bounded_text(PyInterpreterState *interp, const char *field,
                   PyObject *value, PyObject *truncated_fields)
{
    if (!PyUnicode_Check(value)) {
        PyErr_Format(PyExc_TypeError, "%s must be a string", field);
        return NULL;
    }
    Py_ssize_t length = PyUnicode_GetLength(value);
    if (length < 0) {
        return NULL;
    }
    if (length <= interp->fuzzy.text_limit) {
        return Py_NewRef(value);
    }
    PyObject *field_name = PyUnicode_FromString(field);
    if (field_name == NULL) {
        return NULL;
    }
    int appended = PyList_Append(truncated_fields, field_name);
    Py_DECREF(field_name);
    if (appended < 0) {
        return NULL;
    }
    return PyUnicode_Substring(value, 0, interp->fuzzy.text_limit);
}

static PyObject *
fuzzy_copy_event(PyObject *event)
{
    PyObject *copy = PyDict_Copy(event);
    if (copy == NULL) {
        return NULL;
    }
    PyObject *fields = PyDict_GetItemString(event, "truncated_fields");
    if (fields == NULL || !PyList_Check(fields)) {
        Py_DECREF(copy);
        PyErr_SetString(PyExc_RuntimeError,
                        "invalid interpreter-owned fuzzy event");
        return NULL;
    }
    PyObject *fields_copy = PyList_GetSlice(fields, 0, PyList_GET_SIZE(fields));
    if (fields_copy == NULL) {
        Py_DECREF(copy);
        return NULL;
    }
    int result = PyDict_SetItemString(copy, "truncated_fields", fields_copy);
    Py_DECREF(fields_copy);
    if (result < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;
}

static int
fuzzy_append_event(PyInterpreterState *interp, PyObject *kind, PyObject *code,
                   PyObject *message, PyObject *provenance,
                   PyObject *operation, int trusted)
{
    PyObject *truncated_fields = PyList_New(0);
    PyObject *event = PyDict_New();
    if (truncated_fields == NULL || event == NULL) {
        Py_XDECREF(truncated_fields);
        Py_XDECREF(event);
        return -1;
    }

    PyObject *bounded_kind = fuzzy_bounded_text(
        interp, "kind", kind, truncated_fields);
    PyObject *bounded_code = fuzzy_bounded_text(
        interp, "code", code, truncated_fields);
    PyObject *bounded_message = fuzzy_bounded_text(
        interp, "message", message, truncated_fields);
    PyObject *bounded_provenance = fuzzy_bounded_text(
        interp, "provenance", provenance, truncated_fields);
    PyObject *bounded_operation = operation == Py_None
        ? Py_NewRef(Py_None)
        : fuzzy_bounded_text(interp, "operation", operation,
                             truncated_fields);
    if (bounded_kind == NULL || bounded_code == NULL
            || bounded_message == NULL || bounded_provenance == NULL
            || bounded_operation == NULL) {
        Py_XDECREF(bounded_kind);
        Py_XDECREF(bounded_code);
        Py_XDECREF(bounded_message);
        Py_XDECREF(bounded_provenance);
        Py_XDECREF(bounded_operation);
        Py_DECREF(truncated_fields);
        Py_DECREF(event);
        return -1;
    }

    Py_ssize_t sequence = PyList_GET_SIZE(interp->fuzzy.events);
    PyObject *protocol = PyLong_FromLong(FUZZY_PROTOCOL_VERSION);
    PyObject *sequence_object = PyLong_FromSsize_t(sequence);
    if (protocol == NULL || sequence_object == NULL) {
        Py_XDECREF(protocol);
        Py_XDECREF(sequence_object);
        Py_DECREF(bounded_kind);
        Py_DECREF(bounded_code);
        Py_DECREF(bounded_message);
        Py_DECREF(bounded_provenance);
        Py_DECREF(bounded_operation);
        Py_DECREF(truncated_fields);
        Py_DECREF(event);
        return -1;
    }
    int failed = PyDict_SetItemString(
        event, "protocol_version", protocol);
    if (!failed) {
        failed = PyDict_SetItemString(event, "sequence", sequence_object);
    }
    if (!failed) {
        failed = PyDict_SetItemString(event, "kind", bounded_kind);
    }
    if (!failed) {
        failed = PyDict_SetItemString(event, "code", bounded_code);
    }
    if (!failed) {
        failed = PyDict_SetItemString(event, "message", bounded_message);
    }
    if (!failed) {
        failed = PyDict_SetItemString(
            event, "provenance", bounded_provenance);
    }
    if (!failed) {
        failed = PyDict_SetItemString(event, "operation", bounded_operation);
    }
    if (!failed) {
        failed = PyDict_SetItemString(
            event, "truncated_fields", truncated_fields);
    }
    Py_DECREF(protocol);
    Py_DECREF(sequence_object);
    if (failed) {
        Py_DECREF(bounded_kind);
        Py_DECREF(bounded_code);
        Py_DECREF(bounded_message);
        Py_DECREF(bounded_provenance);
        Py_DECREF(bounded_operation);
        Py_DECREF(truncated_fields);
        Py_DECREF(event);
        return -1;
    }
    if (PyList_Append(interp->fuzzy.events, event) < 0) {
        Py_DECREF(bounded_kind);
        Py_DECREF(bounded_code);
        Py_DECREF(bounded_message);
        Py_DECREF(bounded_provenance);
        Py_DECREF(bounded_operation);
        Py_DECREF(truncated_fields);
        Py_DECREF(event);
        return -1;
    }
    if (trusted && fuzzy_checkpoint_event(
            sequence, bounded_kind, bounded_code, bounded_message,
            bounded_provenance, bounded_operation) < 0) {
        Py_DECREF(bounded_kind);
        Py_DECREF(bounded_code);
        Py_DECREF(bounded_message);
        Py_DECREF(bounded_provenance);
        Py_DECREF(bounded_operation);
        Py_DECREF(truncated_fields);
        Py_DECREF(event);
        return -1;
    }
    Py_DECREF(bounded_kind);
    Py_DECREF(bounded_code);
    Py_DECREF(bounded_message);
    Py_DECREF(bounded_provenance);
    Py_DECREF(bounded_operation);
    Py_DECREF(truncated_fields);
    if (interp->fuzzy.event_sink != NULL) {
        PyObject *copy = fuzzy_copy_event(event);
        if (copy == NULL) {
            Py_DECREF(event);
            return -1;
        }
        PyObject *result = PyObject_CallOneArg(interp->fuzzy.event_sink, copy);
        Py_DECREF(copy);
        if (result == NULL) {
            Py_DECREF(event);
            return -1;
        }
        Py_DECREF(result);
    }
    Py_DECREF(event);
    return 0;
}

static int
fuzzy_append_budget_exhausted(PyInterpreterState *interp, int trusted)
{
    PyObject *kind = PyUnicode_FromString("runtime");
    PyObject *code = PyUnicode_FromString("fuzzy-event-budget-exhausted");
    PyObject *message = PyUnicode_FromString(
        "fuzzy event limit reached before unrecorded recovery");
    PyObject *provenance = PyUnicode_FromString("fuzzy-runtime");
    if (kind == NULL || code == NULL || message == NULL || provenance == NULL) {
        Py_XDECREF(kind);
        Py_XDECREF(code);
        Py_XDECREF(message);
        Py_XDECREF(provenance);
        return -1;
    }
    int result = fuzzy_append_event(
        interp, kind, code, message, provenance, Py_None, trusted);
    Py_DECREF(kind);
    Py_DECREF(code);
    Py_DECREF(message);
    Py_DECREF(provenance);
    return result;
}

static PyObject *
fuzzy_configure(PyObject *module, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {
        "event_limit", "text_limit", "provenance_depth_limit", NULL,
    };
    PyObject *event_limit_obj;
    PyObject *text_limit_obj;
    PyObject *depth_limit_obj;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "OOO:configure", keywords,
            &event_limit_obj, &text_limit_obj, &depth_limit_obj)) {
        return NULL;
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (fuzzy_require_enabled(interp) < 0 || fuzzy_require_state(interp) < 0) {
        return NULL;
    }
    if (PyList_GET_SIZE(interp->fuzzy.events) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "fuzzy runtime is already active");
        return NULL;
    }
    Py_ssize_t event_limit;
    Py_ssize_t text_limit;
    Py_ssize_t depth_limit;
    if (fuzzy_parse_limit(event_limit_obj, "event_limit", 2,
                          FUZZY_MAX_EVENT_LIMIT, &event_limit) < 0
            || fuzzy_parse_limit(text_limit_obj, "text_limit", 32,
                                 FUZZY_MAX_TEXT_LIMIT, &text_limit) < 0
            || fuzzy_parse_limit(depth_limit_obj, "provenance_depth_limit", 1,
                                 FUZZY_MAX_PROVENANCE_DEPTH_LIMIT,
                                 &depth_limit) < 0) {
        return NULL;
    }
    interp->fuzzy.event_limit = event_limit;
    interp->fuzzy.text_limit = text_limit;
    interp->fuzzy.provenance_depth_limit = depth_limit;
    Py_RETURN_NONE;
}

static int
fuzzy_emit_event_impl(PyInterpreterState *interp, PyObject *kind,
                      PyObject *code, PyObject *message,
                      PyObject *provenance, PyObject *operation, int trusted)
{
    if (fuzzy_require_enabled(interp) < 0 || fuzzy_require_state(interp) < 0) {
        return -1;
    }
    if (PyList_GET_SIZE(interp->fuzzy.events)
            >= interp->fuzzy.event_limit - 1) {
        if (!interp->fuzzy.event_budget_exhausted) {
            interp->fuzzy.event_budget_exhausted = 1;
            if (fuzzy_append_budget_exhausted(interp, trusted) < 0) {
                return -1;
            }
        }
        PyErr_SetString(interp->fuzzy.event_budget_error,
                        "fuzzy event limit reached");
        return -1;
    }
    return fuzzy_append_event(
        interp, kind, code, message, provenance, operation, trusted);
}

int
_PyFuzzy_EmitEvent(PyInterpreterState *interp, PyObject *kind,
                   PyObject *code, PyObject *message, PyObject *provenance,
                   PyObject *operation)
{
    return fuzzy_emit_event_impl(
        interp, kind, code, message, provenance, operation, 0);
}

int
_PyFuzzy_EmitTrustedEvent(PyInterpreterState *interp, PyObject *kind,
                          PyObject *code, PyObject *message,
                          PyObject *provenance, PyObject *operation)
{
    return fuzzy_emit_event_impl(
        interp, kind, code, message, provenance, operation, 1);
}

static int
fuzzy_identity_provenance(PyObject *provenances, PyObject *object,
                          PyObject **result)
{
    if (provenances == NULL || !PyDict_Check(provenances)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy identity state is not initialized");
        return -1;
    }
    PyObject *key = PyLong_FromVoidPtr(object);
    if (key == NULL) {
        return -1;
    }
    PyObject *provenance = PyDict_GetItemWithError(provenances, key);
    Py_DECREF(key);
    if (provenance == NULL) {
        if (PyErr_Occurred()) {
            return -1;
        }
        *result = NULL;
        return 0;
    }
    *result = provenance;
    return 1;
}

static int
fuzzy_register_identity(PyObject *objects, PyObject *provenances,
                        PyObject *object, PyObject *provenance)
{
    if (objects == NULL || !PyList_Check(objects)
            || provenances == NULL || !PyDict_Check(provenances)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy identity state is not initialized");
        return -1;
    }
    PyObject *key = PyLong_FromVoidPtr(object);
    if (key == NULL) {
        return -1;
    }
    PyObject *existing = PyDict_GetItemWithError(provenances, key);
    if (existing != NULL) {
        Py_DECREF(key);
        if (existing != provenance) {
            PyErr_SetString(PyExc_RuntimeError,
                            "fuzzy identity has conflicting provenance");
            return -1;
        }
        return 0;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(key);
        return -1;
    }
    Py_ssize_t index = PyList_GET_SIZE(objects);
    if (index >= FUZZY_MAX_TRACKED_PATHS) {
        Py_DECREF(key);
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy identity tracking limit reached");
        return -1;
    }
    if (PyList_Append(objects, object) < 0
            || PyDict_SetItem(provenances, key, provenance) < 0) {
        PyObject *raised = PyErr_GetRaisedException();
        if (PyList_GET_SIZE(objects) == index + 1) {
            if (PyList_SetSlice(objects, index, index + 1, NULL) < 0) {
                PyErr_Clear();
            }
        }
        Py_DECREF(key);
        PyErr_SetRaisedException(raised);
        return -1;
    }
    Py_DECREF(key);
    return 0;
}

static int
fuzzy_trusted_value_state(PyInterpreterState *interp, PyObject *value,
                          PyObject **provenance, Py_ssize_t *depth)
{
    PyObject *state;
    int found = fuzzy_identity_provenance(
        interp->fuzzy.trusted_value_provenances, value, &state);
    if (found <= 0) {
        return found;
    }
    if (!PyTuple_CheckExact(state) || PyTuple_GET_SIZE(state) != 2
            || !PyUnicode_Check(PyTuple_GET_ITEM(state, 0))
            || !PyLong_CheckExact(PyTuple_GET_ITEM(state, 1))) {
        PyErr_SetString(PyExc_RuntimeError,
                        "invalid trusted fuzzy value state");
        return -1;
    }
    Py_ssize_t parsed = PyLong_AsSsize_t(PyTuple_GET_ITEM(state, 1));
    if ((parsed == -1 && PyErr_Occurred()) || parsed < 0
            || parsed > interp->fuzzy.provenance_depth_limit) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_RuntimeError,
                            "invalid trusted fuzzy value depth");
        }
        return -1;
    }
    *provenance = PyTuple_GET_ITEM(state, 0);
    *depth = parsed;
    return 1;
}

static int
fuzzy_register_trusted_value(PyInterpreterState *interp, PyObject *value,
                             PyObject *provenance, Py_ssize_t depth)
{
    PyObject *depth_object = PyLong_FromSsize_t(depth);
    if (depth_object == NULL) {
        return -1;
    }
    PyObject *state = PyTuple_Pack(2, provenance, depth_object);
    Py_DECREF(depth_object);
    if (state == NULL) {
        return -1;
    }
    int result = fuzzy_register_identity(
        interp->fuzzy.trusted_value_objects,
        interp->fuzzy.trusted_value_provenances,
        value, state);
    Py_DECREF(state);
    return result;
}

int
_PyFuzzy_RegisterTrustedValue(PyInterpreterState *interp, PyObject *value,
                              PyObject *provenance)
{
    const PyConfig *config = _PyInterpreterState_GetConfig(interp);
    if (_PyFuzzy_GetMode(config) != 1) {
        PyErr_SetString(PyExc_RuntimeError,
                        "cannot register a fuzzy value outside fuzzy mode");
        return -1;
    }
    if (fuzzy_require_state(interp) < 0) {
        return -1;
    }
    if (!PyUnicode_Check(provenance)) {
        PyErr_SetString(PyExc_TypeError,
                        "trusted fuzzy provenance must be a string");
        return -1;
    }
    return fuzzy_register_trusted_value(interp, value, provenance, 0);
}

static int
fuzzy_emit_trusted_warning(PyInterpreterState *interp, const char *code_text,
                           const char *message_text, PyObject *provenance)
{
    PyObject *kind = PyUnicode_FromString("warning");
    PyObject *code = PyUnicode_FromString(code_text);
    PyObject *message = PyUnicode_FromString(message_text);
    if (kind == NULL || code == NULL || message == NULL) {
        Py_XDECREF(kind);
        Py_XDECREF(code);
        Py_XDECREF(message);
        return -1;
    }
    int result = _PyFuzzy_EmitTrustedEvent(
        interp, kind, code, message, provenance, Py_None);
    Py_DECREF(kind);
    Py_DECREF(code);
    Py_DECREF(message);
    return result;
}

PyObject *
_PyFuzzy_NewValue(PyInterpreterState *interp, PyObject *provenance,
                  Py_ssize_t depth)
{
    PyObject *factory = interp->fuzzy.value_factory;
    PyObject *provenance_member = interp->fuzzy.value_provenance_member;
    PyObject *depth_member = interp->fuzzy.value_depth_member;
    if (factory == NULL || !PyType_Check(factory)
            || provenance_member == NULL
            || !Py_IS_TYPE(provenance_member, &PyMemberDescr_Type)
            || depth_member == NULL
            || !Py_IS_TYPE(depth_member, &PyMemberDescr_Type)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy value type is not installed");
        return NULL;
    }
    if (!PyUnicode_Check(provenance) || depth < 0
            || depth > interp->fuzzy.provenance_depth_limit) {
        PyErr_SetString(PyExc_TypeError,
                        "invalid fuzzy value provenance state");
        return NULL;
    }
    PyObject *bounded = Py_NewRef(provenance);
    Py_ssize_t length = PyUnicode_GetLength(bounded);
    if (length < 0) {
        Py_DECREF(bounded);
        return NULL;
    }
    if (length > interp->fuzzy.text_limit) {
        Py_SETREF(bounded, PyUnicode_Substring(
            bounded, 0, interp->fuzzy.text_limit));
        if (bounded == NULL) {
            return NULL;
        }
        if (fuzzy_emit_trusted_warning(
                interp,
                "fuzzy-provenance-truncated",
                "initial fuzzy provenance exceeded the configured text limit",
                bounded) < 0) {
            Py_DECREF(bounded);
            return NULL;
        }
    }
    PyObject *value = PyType_GenericAlloc((PyTypeObject *)factory, 0);
    PyObject *depth_object = PyLong_FromSsize_t(depth);
    if (value == NULL || depth_object == NULL) {
        Py_XDECREF(value);
        Py_XDECREF(depth_object);
        Py_DECREF(bounded);
        return NULL;
    }
    PyMemberDef *provenance_definition =
        ((PyMemberDescrObject *)provenance_member)->d_member;
    PyMemberDef *depth_definition =
        ((PyMemberDescrObject *)depth_member)->d_member;
    if (PyMember_SetOne(
            (char *)value, provenance_definition, bounded) < 0
            || PyMember_SetOne(
                (char *)value, depth_definition, depth_object) < 0) {
        Py_DECREF(value);
        Py_DECREF(depth_object);
        Py_DECREF(bounded);
        return NULL;
    }
    Py_DECREF(depth_object);
    Py_DECREF(bounded);
    return value;
}

static PyObject *
fuzzy_derive_call_provenance(PyInterpreterState *interp,
                             PyObject *provenance, Py_ssize_t depth,
                             Py_ssize_t *result_depth)
{
    if (depth >= interp->fuzzy.provenance_depth_limit) {
        if (fuzzy_emit_trusted_warning(
                interp,
                "fuzzy-provenance-depth-exhausted",
                "fuzzy value derivation retained its bounded provenance",
                provenance) < 0) {
            return NULL;
        }
        *result_depth = depth;
        return Py_NewRef(provenance);
    }
    *result_depth = depth + 1;
    Py_ssize_t length = PyUnicode_GetLength(provenance);
    if (length < 0) {
        return NULL;
    }
    static const Py_ssize_t suffix_length = 7;
    if (length > interp->fuzzy.text_limit - suffix_length) {
        if (fuzzy_emit_trusted_warning(
                interp,
                "fuzzy-provenance-truncated",
                "derived fuzzy provenance exceeded the configured text limit",
                provenance) < 0) {
            return NULL;
        }
        return Py_NewRef(provenance);
    }
    return PyUnicode_FromFormat("%U.result", provenance);
}

int
_PyFuzzy_TryCall(PyThreadState *tstate, PyObject *callable,
                 PyObject **result)
{
    *result = NULL;
    PyInterpreterState *interp = tstate->interp;
    const PyConfig *config = _PyInterpreterState_GetConfig(interp);
    if (_PyFuzzy_GetMode(config) != 1) {
        return 0;
    }
    if (fuzzy_require_state(interp) < 0) {
        return -1;
    }
    PyObject *provenance;
    Py_ssize_t depth;
    int trusted = fuzzy_trusted_value_state(
        interp, callable, &provenance, &depth);
    if (trusted <= 0) {
        return trusted;
    }
    if (fuzzy_emit_trusted_warning(
            interp, "unknown-call", "unknown call was skipped",
            provenance) < 0) {
        return -1;
    }
    Py_ssize_t result_depth;
    PyObject *derived = fuzzy_derive_call_provenance(
        interp, provenance, depth, &result_depth);
    if (derived == NULL) {
        return -1;
    }
    PyObject *value = _PyFuzzy_NewValue(interp, derived, result_depth);
    if (value == NULL) {
        Py_DECREF(derived);
        return -1;
    }
    if (fuzzy_register_trusted_value(
            interp, value, derived, result_depth) < 0) {
        Py_DECREF(value);
        Py_DECREF(derived);
        return -1;
    }
    Py_DECREF(derived);
    *result = value;
    return 1;
}

int
_PyFuzzy_PropagateTrustedPath(PyInterpreterState *interp, PyObject *source,
                              PyObject *path)
{
    const PyConfig *config = _PyInterpreterState_GetConfig(interp);
    if (_PyFuzzy_GetMode(config) != 1) {
        return 0;
    }
    if (fuzzy_require_state(interp) < 0) {
        return -1;
    }
    PyObject *provenance;
    Py_ssize_t depth;
    int found = fuzzy_trusted_value_state(
        interp, source, &provenance, &depth);
    if (found <= 0) {
        return found;
    }
    if (!PyUnicode_Check(path) && !PyBytes_Check(path)) {
        PyErr_SetString(PyExc_TypeError,
                        "trusted fuzzy path must be str or bytes");
        return -1;
    }
    if (fuzzy_register_identity(
            interp->fuzzy.trusted_path_objects,
            interp->fuzzy.trusted_path_provenances,
            path, provenance) < 0) {
        return -1;
    }
    return 1;
}

int
_PyFuzzy_RegisterTrustedModule(PyInterpreterState *interp, PyObject *module,
                               PyObject *provenance)
{
    const PyConfig *config = _PyInterpreterState_GetConfig(interp);
    if (_PyFuzzy_GetMode(config) != 1) {
        PyErr_SetString(PyExc_RuntimeError,
                        "cannot register a fuzzy module outside fuzzy mode");
        return -1;
    }
    if (fuzzy_require_state(interp) < 0) {
        return -1;
    }
    if (!PyModule_Check(module) || !PyUnicode_Check(provenance)) {
        PyErr_SetString(PyExc_TypeError,
                        "trusted fuzzy module registration is invalid");
        return -1;
    }
    return fuzzy_register_identity(
        interp->fuzzy.trusted_module_objects,
        interp->fuzzy.trusted_module_provenances,
        module, provenance);
}

int
_PyFuzzy_ModuleProvenance(PyInterpreterState *interp, PyObject *module,
                          PyObject **provenance)
{
    if (fuzzy_require_state(interp) < 0) {
        return -1;
    }
    int found = fuzzy_identity_provenance(
        interp->fuzzy.trusted_module_provenances, module, provenance);
    if (found != 0) {
        return found < 0 ? -1 : 2;
    }
    found = fuzzy_identity_provenance(
        interp->fuzzy.untrusted_module_provenances, module, provenance);
    return found < 0 ? -1 : found;
}

static int
fuzzy_path_token_index(const char *path, Py_ssize_t length,
                       Py_ssize_t provenance_count, Py_ssize_t *result)
{
    static const char prefix[] = "<fuzzy:";
    const Py_ssize_t prefix_length = (Py_ssize_t)(sizeof(prefix) - 1);
    Py_ssize_t candidates = 0;
    for (Py_ssize_t index = 0; index + prefix_length + 2 <= length; index++) {
        if (memcmp(path + index, prefix, (size_t)prefix_length) != 0) {
            continue;
        }
        candidates++;
        if (candidates > FUZZY_MAX_PATH_TOKEN_CANDIDATES) {
            PyErr_SetString(PyExc_RuntimeError,
                            "fuzzy path token scan limit reached");
            return -1;
        }
        Py_ssize_t cursor = index + prefix_length;
        Py_ssize_t token_index = 0;
        int digits = 0;
        while (cursor < length && path[cursor] >= '0' && path[cursor] <= '9') {
            int digit = path[cursor] - '0';
            if (token_index > (PY_SSIZE_T_MAX - digit) / 10) {
                token_index = provenance_count;
                break;
            }
            token_index = token_index * 10 + digit;
            digits++;
            cursor++;
        }
        if (digits > 0 && cursor < length && path[cursor] == '>'
                && token_index < provenance_count) {
            *result = token_index;
            return 1;
        }
    }
    return 0;
}

int
_PyFuzzy_EmitUnresolvedPath(PyInterpreterState *interp, PyObject *path,
                            const char *operation_text)
{
    const PyConfig *config = _PyInterpreterState_GetConfig(interp);
    if (_PyFuzzy_GetMode(config) != 1) {
        return 0;
    }
    if (fuzzy_require_state(interp) < 0) {
        return -1;
    }
    if (interp->fuzzy.path_provenances == NULL
            || !PyList_Check(interp->fuzzy.path_provenances)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy path state is not initialized");
        return -1;
    }

    PyObject *provenance;
    int trusted = fuzzy_identity_provenance(
        interp->fuzzy.trusted_path_provenances, path, &provenance);
    if (trusted < 0) {
        return -1;
    }
    if (!trusted) {
        PyObject *bytes;
        if (PyBytes_Check(path)) {
            bytes = Py_NewRef(path);
        }
        else if (PyUnicode_Check(path)) {
            bytes = PyUnicode_EncodeFSDefault(path);
        }
        else {
            return 0;
        }
        if (bytes == NULL) {
            return -1;
        }
        Py_ssize_t token_index;
        int found = fuzzy_path_token_index(
            PyBytes_AS_STRING(bytes), PyBytes_GET_SIZE(bytes),
            PyList_GET_SIZE(interp->fuzzy.path_provenances), &token_index);
        Py_DECREF(bytes);
        if (found <= 0) {
            return found;
        }
        provenance = PyList_GET_ITEM(
            interp->fuzzy.path_provenances, token_index);
    }
    PyObject *kind = PyUnicode_FromString("unresolved-operation");
    PyObject *code = PyUnicode_FromString("fuzzy-delete-target-unresolved");
    PyObject *message = PyUnicode_FromString(
        "delete target stayed unresolved");
    PyObject *operation = PyUnicode_FromString(operation_text);
    if (kind == NULL || code == NULL || message == NULL || operation == NULL) {
        Py_XDECREF(kind);
        Py_XDECREF(code);
        Py_XDECREF(message);
        Py_XDECREF(operation);
        return -1;
    }
    int emitted = trusted
        ? _PyFuzzy_EmitTrustedEvent(
            interp, kind, code, message, provenance, operation)
        : _PyFuzzy_EmitEvent(
            interp, kind, code, message, provenance, operation);
    Py_DECREF(kind);
    Py_DECREF(code);
    Py_DECREF(message);
    Py_DECREF(operation);
    return emitted < 0 ? -1 : 1;
}

static PyObject *
fuzzy_path_token(PyObject *module, PyObject *provenance)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (fuzzy_require_enabled(interp) < 0 || fuzzy_require_state(interp) < 0) {
        return NULL;
    }
    if (!PyUnicode_Check(provenance)) {
        PyErr_SetString(PyExc_TypeError, "provenance must be a string");
        return NULL;
    }
    if (interp->fuzzy.path_provenances == NULL
            || !PyList_Check(interp->fuzzy.path_provenances)
            || interp->fuzzy.provenance_tokens == NULL
            || !PyDict_Check(interp->fuzzy.provenance_tokens)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy path state is not initialized");
        return NULL;
    }
    Py_ssize_t provenance_length = PyUnicode_GetLength(provenance);
    if (provenance_length < 0) {
        return NULL;
    }
    if (provenance_length > interp->fuzzy.text_limit) {
        PyErr_SetString(PyExc_ValueError,
                        "provenance exceeds the configured text limit");
        return NULL;
    }
    PyObject *token = PyDict_GetItemWithError(
        interp->fuzzy.provenance_tokens, provenance);
    if (token != NULL) {
        return Py_NewRef(token);
    }
    if (PyErr_Occurred()) {
        return NULL;
    }
    Py_ssize_t index = PyList_GET_SIZE(interp->fuzzy.path_provenances);
    if (index >= FUZZY_MAX_TRACKED_PATHS) {
        PyObject *kind = PyUnicode_FromString("runtime");
        PyObject *code = PyUnicode_FromString("fuzzy-string-budget-exhausted");
        PyObject *message = PyUnicode_FromString(
            "fuzzy string tracking limit reached");
        if (kind == NULL || code == NULL || message == NULL) {
            Py_XDECREF(kind);
            Py_XDECREF(code);
            Py_XDECREF(message);
            return NULL;
        }
        int emitted = _PyFuzzy_EmitEvent(
            interp, kind, code, message, provenance, Py_None);
        Py_DECREF(kind);
        Py_DECREF(code);
        Py_DECREF(message);
        if (emitted < 0) {
            return NULL;
        }
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy string tracking limit reached");
        return NULL;
    }
    token = PyUnicode_FromFormat("<fuzzy:%zd>", index);
    if (token == NULL) {
        return NULL;
    }
    if (PyList_Append(interp->fuzzy.path_provenances, provenance) < 0
            || PyDict_SetItem(
                interp->fuzzy.provenance_tokens, provenance, token) < 0) {
        PyObject *raised = PyErr_GetRaisedException();
        if (PyList_GET_SIZE(interp->fuzzy.path_provenances) == index + 1) {
            if (PyList_SetSlice(
                    interp->fuzzy.path_provenances, index, index + 1, NULL) < 0) {
                PyErr_Clear();
            }
        }
        PyErr_SetRaisedException(raised);
        Py_DECREF(token);
        return NULL;
    }
    return token;
}

static PyObject *
fuzzy_emit_event(PyObject *module, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {
        "kind", "code", "message", "provenance", "operation", NULL,
    };
    PyObject *kind;
    PyObject *code;
    PyObject *message;
    PyObject *provenance;
    PyObject *operation = Py_None;
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "UUUU|O:emit_event", keywords,
            &kind, &code, &message, &provenance, &operation)) {
        return NULL;
    }
    if (operation != Py_None && !PyUnicode_Check(operation)) {
        PyErr_SetString(PyExc_TypeError, "operation must be a string or None");
        return NULL;
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (_PyFuzzy_EmitEvent(
            interp, kind, code, message, provenance, operation) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
fuzzy_register_untrusted_module(PyObject *module, PyObject *args)
{
    PyObject *target;
    PyObject *provenance;
    if (!PyArg_ParseTuple(
            args, "OU:register_untrusted_module", &target, &provenance)) {
        return NULL;
    }
    if (!PyModule_Check(target)) {
        PyErr_SetString(PyExc_TypeError,
                        "untrusted fuzzy module must be a module");
        return NULL;
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (fuzzy_require_enabled(interp) < 0 || fuzzy_require_state(interp) < 0) {
        return NULL;
    }
    if (fuzzy_register_identity(
            interp->fuzzy.untrusted_module_objects,
            interp->fuzzy.untrusted_module_provenances,
            target, provenance) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
fuzzy_set_event_sink(PyObject *module, PyObject *sink)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (fuzzy_require_enabled(interp) < 0 || fuzzy_require_state(interp) < 0) {
        return NULL;
    }
    if (!PyCallable_Check(sink)) {
        PyErr_SetString(PyExc_TypeError, "event sink must be callable");
        return NULL;
    }
    if (interp->fuzzy.event_sink != NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy event sink is already installed");
        return NULL;
    }
    if (PyList_GET_SIZE(interp->fuzzy.events) != 0) {
        PyErr_SetString(PyExc_RuntimeError,
                        "fuzzy events have already been recorded");
        return NULL;
    }
    interp->fuzzy.event_sink = Py_NewRef(sink);
    Py_RETURN_NONE;
}

static PyObject *
fuzzy_get_events(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (fuzzy_require_state(interp) < 0) {
        return NULL;
    }
    Py_ssize_t length = PyList_GET_SIZE(interp->fuzzy.events);
    PyObject *events = PyList_New(length);
    if (events == NULL) {
        return NULL;
    }
    for (Py_ssize_t index = 0; index < length; index++) {
        PyObject *event = PyList_GET_ITEM(interp->fuzzy.events, index);
        PyObject *copy = fuzzy_copy_event(event);
        if (copy == NULL) {
            Py_DECREF(events);
            return NULL;
        }
        PyList_SET_ITEM(events, index, copy);
    }
    return events;
}

static PyObject *
fuzzy_get_limits(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (fuzzy_require_state(interp) < 0) {
        return NULL;
    }
    return Py_BuildValue(
        "(nnn)", interp->fuzzy.event_limit, interp->fuzzy.text_limit,
        interp->fuzzy.provenance_depth_limit);
}

static PyObject *
fuzzy_has_events(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (fuzzy_require_state(interp) < 0) {
        return NULL;
    }
    return PyBool_FromLong(PyList_GET_SIZE(interp->fuzzy.events) != 0);
}

static PyObject *
fuzzy_get_event_state(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (fuzzy_require_state(interp) < 0) {
        return NULL;
    }
    PyObject *state = PyDict_New();
    if (state == NULL) {
        return NULL;
    }
    if (fuzzy_set_dict_item(
            state, "event_limit",
            PyLong_FromSsize_t(interp->fuzzy.event_limit)) < 0
            || fuzzy_set_dict_item(
                state, "text_limit",
                PyLong_FromSsize_t(interp->fuzzy.text_limit)) < 0
            || fuzzy_set_dict_item(
                state, "provenance_depth_limit",
                PyLong_FromSsize_t(
                    interp->fuzzy.provenance_depth_limit)) < 0
            || fuzzy_set_dict_item(
                state, "event_count",
                PyLong_FromSsize_t(
                    PyList_GET_SIZE(interp->fuzzy.events))) < 0
            || fuzzy_set_dict_item(
                state, "event_budget_exhausted",
                PyBool_FromLong(
                    interp->fuzzy.event_budget_exhausted)) < 0
            || fuzzy_set_dict_item(
                state, "event_sink_installed",
                PyBool_FromLong(interp->fuzzy.event_sink != NULL)) < 0) {
        Py_DECREF(state);
        return NULL;
    }
    return state;
}

static PyMethodDef fuzzy_runtime_methods[] = {
    {"configure", _PyCFunction_CAST(fuzzy_configure),
     METH_VARARGS | METH_KEYWORDS, NULL},
    {"emit_event", _PyCFunction_CAST(fuzzy_emit_event),
     METH_VARARGS | METH_KEYWORDS, NULL},
    {"register_untrusted_module", fuzzy_register_untrusted_module,
     METH_VARARGS, NULL},
    {"set_event_sink", fuzzy_set_event_sink, METH_O, NULL},
    {"get_events", fuzzy_get_events, METH_NOARGS, NULL},
    {"get_limits", fuzzy_get_limits, METH_NOARGS, NULL},
    {"has_events", fuzzy_has_events, METH_NOARGS, NULL},
    {"get_event_state", fuzzy_get_event_state, METH_NOARGS, NULL},
    {"path_token", fuzzy_path_token, METH_O, NULL},
    {NULL, NULL, 0, NULL},
};

static int
fuzzy_runtime_exec(PyObject *module)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (!interp->fuzzy.event_state_initialized) {
        interp->fuzzy.events = PyList_New(0);
        if (interp->fuzzy.events == NULL) {
            return -1;
        }
        interp->fuzzy.event_budget_error = PyErr_NewException(
            "fuzzy_runtime.FuzzyEventBudgetExceeded", PyExc_RuntimeError, NULL);
        if (interp->fuzzy.event_budget_error == NULL) {
            Py_CLEAR(interp->fuzzy.events);
            return -1;
        }
        interp->fuzzy.path_provenances = PyList_New(0);
        interp->fuzzy.provenance_tokens = PyDict_New();
        interp->fuzzy.trusted_value_objects = PyList_New(0);
        interp->fuzzy.trusted_value_provenances = PyDict_New();
        interp->fuzzy.trusted_path_objects = PyList_New(0);
        interp->fuzzy.trusted_path_provenances = PyDict_New();
        interp->fuzzy.trusted_module_objects = PyList_New(0);
        interp->fuzzy.trusted_module_provenances = PyDict_New();
        interp->fuzzy.untrusted_module_objects = PyList_New(0);
        interp->fuzzy.untrusted_module_provenances = PyDict_New();
        if (interp->fuzzy.path_provenances == NULL
                || interp->fuzzy.provenance_tokens == NULL
                || interp->fuzzy.trusted_value_objects == NULL
                || interp->fuzzy.trusted_value_provenances == NULL
                || interp->fuzzy.trusted_path_objects == NULL
                || interp->fuzzy.trusted_path_provenances == NULL
                || interp->fuzzy.trusted_module_objects == NULL
                || interp->fuzzy.trusted_module_provenances == NULL
                || interp->fuzzy.untrusted_module_objects == NULL
                || interp->fuzzy.untrusted_module_provenances == NULL) {
            Py_CLEAR(interp->fuzzy.events);
            Py_CLEAR(interp->fuzzy.event_budget_error);
            Py_CLEAR(interp->fuzzy.path_provenances);
            Py_CLEAR(interp->fuzzy.provenance_tokens);
            Py_CLEAR(interp->fuzzy.trusted_value_objects);
            Py_CLEAR(interp->fuzzy.trusted_value_provenances);
            Py_CLEAR(interp->fuzzy.trusted_path_objects);
            Py_CLEAR(interp->fuzzy.trusted_path_provenances);
            Py_CLEAR(interp->fuzzy.trusted_module_objects);
            Py_CLEAR(interp->fuzzy.trusted_module_provenances);
            Py_CLEAR(interp->fuzzy.untrusted_module_objects);
            Py_CLEAR(interp->fuzzy.untrusted_module_provenances);
            return -1;
        }
        interp->fuzzy.event_limit = FUZZY_DEFAULT_EVENT_LIMIT;
        interp->fuzzy.text_limit = FUZZY_DEFAULT_TEXT_LIMIT;
        interp->fuzzy.provenance_depth_limit =
            FUZZY_DEFAULT_PROVENANCE_DEPTH_LIMIT;
        interp->fuzzy.event_budget_exhausted = 0;
        interp->fuzzy.event_state_initialized = 1;
    }
    if (PyModule_AddIntConstant(
            module, "PROTOCOL_VERSION", FUZZY_PROTOCOL_VERSION) < 0
            || PyModule_AddIntConstant(
                module, "DEFAULT_EVENT_LIMIT",
                FUZZY_DEFAULT_EVENT_LIMIT) < 0
            || PyModule_AddIntConstant(
                module, "DEFAULT_TEXT_LIMIT",
                FUZZY_DEFAULT_TEXT_LIMIT) < 0
            || PyModule_AddIntConstant(
                module, "DEFAULT_PROVENANCE_DEPTH_LIMIT",
                FUZZY_DEFAULT_PROVENANCE_DEPTH_LIMIT) < 0
            || PyModule_AddObjectRef(
                module, "FuzzyEventBudgetExceeded",
                interp->fuzzy.event_budget_error) < 0) {
        return -1;
    }
    return 0;
}

static PyModuleDef_Slot fuzzy_runtime_slots[] = {
    {Py_mod_exec, fuzzy_runtime_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {0, NULL},
};

static struct PyModuleDef fuzzy_runtime_module = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "_fuzzy_runtime",
    .m_doc = "Interpreter-owned state for fuzzy-cpython.",
    .m_size = 0,
    .m_methods = fuzzy_runtime_methods,
    .m_slots = fuzzy_runtime_slots,
};

PyMODINIT_FUNC
PyInit__fuzzy_runtime(void)
{
    return PyModuleDef_Init(&fuzzy_runtime_module);
}

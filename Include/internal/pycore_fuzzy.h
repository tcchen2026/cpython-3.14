#ifndef Py_INTERNAL_FUZZY_H
#define Py_INTERNAL_FUZZY_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

/* Return 1 when the last fuzzy option enables the mode, 0 when it disables
   the mode or is absent, and -1 when its value is invalid. */
static inline int
_PyFuzzy_GetMode(const PyConfig *config)
{
    for (Py_ssize_t i = config->xoptions.length; i > 0; i--) {
        const wchar_t *option = config->xoptions.items[i - 1];
        if (wcscmp(option, L"fuzzy") == 0
                || wcscmp(option, L"fuzzy=1") == 0) {
            return 1;
        }
        if (wcscmp(option, L"fuzzy=0") == 0) {
            return 0;
        }
        if (wcsncmp(option, L"fuzzy=", 6) == 0) {
            return -1;
        }
    }
    return 0;
}

/* The attribute caller must have an active AttributeError.  Both functions
   return a new reference when recovery succeeds, NULL without an exception
   when fuzzy recovery declines, or NULL with the hook's exception. */
extern PyObject *_PyObject_FuzzyMissingAttribute(
    PyObject *owner,
    PyObject *name);
extern PyObject *_PyObject_FuzzyMissingName(
    PyObject *name,
    int is_local);
extern PyObject *_PyObject_FuzzyMissingImport(
    PyThreadState *tstate,
    PyObject *name,
    PyObject *globals,
    PyObject *fromlist,
    PyObject *level);
/* Initialize and install the frozen fork-owned runtime before user code. */
extern int _PyFuzzy_InitializeRuntime(const PyConfig *config);
extern int _PyFuzzy_InstallHooks(
    PyThreadState *tstate,
    PyObject *missing_attribute,
    PyObject *missing_name,
    PyObject *value_factory,
    PyObject *import_func,
    PyObject *import_post,
    PyObject *missing_import,
    PyObject *module_type);
extern int _PyFuzzy_EmitEvent(
    PyInterpreterState *interp,
    PyObject *kind,
    PyObject *code,
    PyObject *message,
    PyObject *provenance,
    PyObject *operation);
extern int _PyFuzzy_EmitTrustedEvent(
    PyInterpreterState *interp,
    PyObject *kind,
    PyObject *code,
    PyObject *message,
    PyObject *provenance,
    PyObject *operation);
/* Register a value created at a trusted C recovery site, then propagate that
   identity only across a filesystem conversion performed inside CPython. */
extern int _PyFuzzy_RegisterTrustedValue(
    PyInterpreterState *interp,
    PyObject *value,
    PyObject *provenance);
/* Allocate the exact captured fuzzy value type without invoking mutable
   Python constructors, and initialize its captured slot descriptors. */
extern PyObject *_PyFuzzy_NewValue(
    PyInterpreterState *interp,
    PyObject *provenance,
    Py_ssize_t depth);
/* If callable is an identity-trusted fuzzy value, record the skipped call,
   create and register its derived result, store that new reference in result,
   and return 1.  Return 0 for every ordinary callable and -1 on error. */
extern int _PyFuzzy_TryCall(
    PyThreadState *tstate,
    PyObject *callable,
    PyObject **result);
extern int _PyFuzzy_PropagateTrustedPath(
    PyInterpreterState *interp,
    PyObject *source,
    PyObject *path);
/* Module provenance returns 2 for a C-confirmed direct import miss, 1 for an
   untrusted Python-origin fuzzy module, 0 for an ordinary module, and -1 on
   error.  The provenance reference is borrowed. */
extern int _PyFuzzy_RegisterTrustedModule(
    PyInterpreterState *interp,
    PyObject *module,
    PyObject *provenance);
extern int _PyFuzzy_ModuleProvenance(
    PyInterpreterState *interp,
    PyObject *module,
    PyObject **provenance);
/* Return 1 after recording either an identity-trusted or marker-recognized
   unresolved deletion target, 0 when the path is concrete, and -1 on error.
   Only the identity-trusted case uses the embedding checkpoint. */
extern int _PyFuzzy_EmitUnresolvedPath(
    PyInterpreterState *interp,
    PyObject *path,
    const char *operation);
extern void _PyFuzzy_Clear(PyInterpreterState *interp);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_FUZZY_H */

import os
import textwrap
import unittest

from test import support
from test.support import os_helper, script_helper
from test.support.script_helper import assert_python_ok


@support.cpython_only
class FuzzyRuntimeTests(unittest.TestCase):
    ENTRY_PROGRAM = textwrap.dedent("""
        import fuzzy_runtime as fuzzy
        try:
            result = entry_mode_missing()
        except NameError:
            print(LABEL + ':standard')
        else:
            print(LABEL + ':fuzzy', isinstance(result, fuzzy.FuzzyValue))
    """)

    def test_standard_mode_is_disabled(self):
        rc, out, err = assert_python_ok(
            '-S', '-c',
            'import fuzzy_runtime; print(fuzzy_runtime.is_enabled())',
        )
        self.assertEqual(out, b'False\n')
        self.assertEqual(err, b'')

    def test_fuzzy_startup_ignores_pythonpath_runtime_shadow(self):
        with os_helper.temp_dir() as directory:
            script_helper.make_script(
                directory,
                'fuzzy_runtime',
                'raise AssertionError("PYTHONPATH shadow loaded")',
            )
            code = (
                'import fuzzy_runtime as fuzzy; '
                'print(fuzzy.is_enabled(), fuzzy.PROTOCOL_VERSION)'
            )
            rc, out, err = assert_python_ok(
                '-S', '-X', 'fuzzy', '-c', code,
                PYTHONPATH=directory,
            )
        self.assertEqual(out, b'True 1\n')
        self.assertEqual(err, b'')

    def test_bounded_event_protocol(self):
        code = textwrap.dedent("""
            import fuzzy_runtime as fuzzy
            fuzzy.configure(event_limit=3, text_limit=32)
            fuzzy._emit_event('warning', 'first', 'x' * 40, 'origin:first')
            snapshot = fuzzy.get_events()
            snapshot[0]['message'] = 'changed'
            fuzzy._emit_event('warning', 'second', 'ok', 'origin:second')
            try:
                fuzzy._emit_event(
                    'warning', 'third', 'unrecorded', 'origin:third'
                )
            except fuzzy.FuzzyEventBudgetExceeded:
                pass
            events = fuzzy.get_events()
            print(len(events), fuzzy.get_state()['event_budget_exhausted'])
            print(events[0]['message'] == 'x' * 32,
                  events[0]['truncated_fields'])
            print(events[-1]['code'], events[-1]['sequence'])
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b"3 True\nTrue ['message']\n"
            b"fuzzy-event-budget-exhausted 2\n",
        )
        self.assertEqual(err, b'')

    def test_configuration_requires_fuzzy_mode(self):
        code = textwrap.dedent("""
            import fuzzy_runtime as fuzzy
            try:
                fuzzy.configure()
            except RuntimeError as error:
                print(error)
        """)
        rc, out, err = assert_python_ok('-S', '-c', code)
        self.assertEqual(out, b'fuzzy mode is disabled\n')
        self.assertEqual(err, b'')

    def test_configuration_sets_user_root_without_reactivation(self):
        with os_helper.temp_dir() as first_root:
            with os_helper.temp_dir() as second_root:
                code = textwrap.dedent(f"""
                    import os
                    import fuzzy_runtime as fuzzy
                    fuzzy.configure(user_root={first_root!r})
                    print(
                        fuzzy.get_state()['user_root']
                        == os.path.realpath({first_root!r})
                    )
                    configured_root_missing
                    try:
                        fuzzy.configure(user_root={second_root!r})
                    except RuntimeError as error:
                        print(error)
                """)
                rc, out, err = assert_python_ok(
                    '-S', '-X', 'fuzzy', '-c', code
                )
        self.assertEqual(
            out,
            b'True\n'
            b'fuzzy runtime already observed recovery for another root\n',
        )
        self.assertEqual(err, b'')

    def test_command_script_and_module_entry_modes_are_opt_in(self):
        def source(label):
            return 'LABEL = ' + repr(label) + '\n' + self.ENTRY_PROGRAM

        with os_helper.temp_dir() as directory:
            script = script_helper.make_script(
                directory, 'fuzzy_entry', source('script')
            )
            package = os.path.join(directory, 'fuzzy_entry_package')
            os.mkdir(package)
            script_helper.make_script(package, '__init__', '')
            script_helper.make_script(
                package, '__main__', source('module')
            )
            modes = (
                ('command', ('-c', source('command'))),
                ('script', (script,)),
                ('module', ('-m', 'fuzzy_entry_package')),
            )
            for label, arguments in modes:
                with self.subTest(mode=label, fuzzy=False):
                    rc, out, err = assert_python_ok(
                        '-S', *arguments,
                        __isolated=False, __cwd=directory,
                    )
                    self.assertEqual(out, (label + ':standard\n').encode())
                    self.assertEqual(err, b'')
                with self.subTest(mode=label, fuzzy=True):
                    rc, out, err = assert_python_ok(
                        '-S', '-X', 'fuzzy', *arguments,
                        __isolated=False, __cwd=directory,
                    )
                    self.assertEqual(out, (label + ':fuzzy True\n').encode())
                    self.assertEqual(err, b'')

    def test_event_sink_is_synchronous_defensive_and_one_time(self):
        code = textwrap.dedent("""
            import fuzzy_runtime as fuzzy
            observed = []
            fuzzy.set_event_sink(observed.append)
            fuzzy._emit_event(
                'warning', 'observed', 'message', 'origin:sink'
            )
            observed[0]['message'] = 'changed'
            print(fuzzy.get_state()['event_sink_installed'])
            print(fuzzy.get_events()[0]['message'])
            try:
                fuzzy.set_event_sink(lambda event: None)
            except RuntimeError as error:
                print(error)
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b'True\nmessage\nfuzzy event sink is already installed\n',
        )
        self.assertEqual(err, b'')

    def test_event_state_survives_python_globals_and_module_reimport(self):
        code = textwrap.dedent("""
            import importlib
            import sys
            import _fuzzy_runtime as state
            import fuzzy_runtime as fuzzy
            observed = []
            fuzzy.set_event_sink(observed.append)
            fuzzy._events = []
            fuzzy._event_sink = lambda event: None
            fuzzy._event_limit = 4096
            fuzzy._emit_event(
                'warning', 'owned', 'message', 'origin:owned'
            )
            snapshot = state.get_events()
            snapshot[0]['message'] = 'changed'
            snapshot[0]['truncated_fields'].append('message')
            del sys.modules['_fuzzy_runtime']
            reloaded = importlib.import_module('_fuzzy_runtime')
            event = reloaded.get_events()[0]
            event_state = reloaded.get_event_state()
            print(event_state['event_count'],
                  event_state['event_sink_installed'])
            print(len(observed), observed[0]['message'])
            print(event['message'], event['truncated_fields'])
            try:
                reloaded.set_event_sink(lambda event: None)
            except RuntimeError as error:
                print(error)
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b'1 True\n1 message\nmessage []\n'
            b'fuzzy event sink is already installed\n',
        )
        self.assertEqual(err, b'')

    def test_fuzzy_values_use_bounded_single_run_defaults(self):
        code = textwrap.dedent("""
            import os
            import fuzzy_runtime as fuzzy
            fuzzy.configure(event_limit=32, provenance_depth_limit=2)
            value = fuzzy.FuzzyValue('unknown-name:client')
            result = value.service('request').more
            print(repr(result))
            print(bool(result), len(result), int(result), float(result))
            print(hash(result), 'item' in result, tuple(iter(result)))
            path = os.fspath(result)
            print(isinstance(path, fuzzy.FuzzyString), path.provenance)
            print([event['code'] for event in fuzzy.get_events()])
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        lines = out.decode().splitlines()
        self.assertEqual(lines[0], '<fuzzy unknown-name:client.service.result>')
        self.assertEqual(lines[1], 'False 0 0 0.0')
        self.assertEqual(lines[2], '0 False ()')
        self.assertEqual(
            lines[3], 'True unknown-name:client.service.result'
        )
        self.assertIn('unknown-call', lines[4])
        self.assertIn('fuzzy-bool-default', lines[4])
        self.assertIn('fuzzy-provenance-depth-exhausted', lines[4])
        self.assertIn('fuzzy-string-unresolved', lines[4])
        self.assertEqual(err, b'')

    def test_fuzzy_value_creation_requires_fuzzy_mode(self):
        code = textwrap.dedent("""
            import fuzzy_runtime as fuzzy
            try:
                fuzzy.FuzzyValue('unknown-name:value')
            except RuntimeError as error:
                print(error)
        """)
        rc, out, err = assert_python_ok('-S', '-c', code)
        self.assertEqual(out, b'fuzzy mode is disabled\n')
        self.assertEqual(err, b'')

    def test_c_recovery_ignores_hook_event_and_factory_replacement(self):
        code = textwrap.dedent("""
            import builtins
            import _fuzzy_runtime as state
            import fuzzy_runtime as fuzzy
            real_type = fuzzy.FuzzyValue
            builtins._fuzzy_missing_name = lambda *args: NotImplemented
            builtins._fuzzy_missing_attribute = lambda *args: NotImplemented
            fuzzy._missing_name = lambda *args: NotImplemented
            fuzzy._missing_attribute = lambda *args: NotImplemented
            fuzzy._emit_event = lambda *args, **kwargs: None
            fuzzy.FuzzyValue = lambda provenance: 'replaced:' + provenance
            name_value = replaced_missing_name
            attribute_value = object().replaced_missing_attribute
            print(isinstance(name_value, real_type))
            print(name_value.provenance)
            print(isinstance(attribute_value, real_type))
            print(attribute_value.provenance)
            print([event['code'] for event in state.get_events()])
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b'True\nunknown-name:replaced_missing_name\n'
            b'True\nunknown-attribute:object.replaced_missing_attribute\n'
            b"['unknown-name', 'unknown-attribute']\n",
        )
        self.assertEqual(err, b'')

    def test_trusted_unknown_calls_bypass_python_call_replacement(self):
        code = textwrap.dedent("""
            import os
            import _fuzzy_runtime as state
            import fuzzy_runtime as fuzzy
            real_type = fuzzy.FuzzyValue
            fuzzy._emit_event = lambda *args, **kwargs: None
            real_type.__call__ = lambda self, *args, **kwargs: 'replaced'
            plain = trusted_plain_call()
            keyword = trusted_keyword_call(flag=True)
            expanded = trusted_expanded_call(*(), **{})
            public = real_type('public-value')
            print(public())
            print(plain.provenance)
            print(keyword.provenance)
            print(expanded.provenance)
            os.remove(keyword)
            events = state.get_events()
            print([event['code'] for event in events])
            print(events[-1]['operation'], events[-1]['provenance'])
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b'replaced\nunknown-name:trusted_plain_call.result\n'
            b'unknown-name:trusted_keyword_call.result\n'
            b'unknown-name:trusted_expanded_call.result\n'
            b"['unknown-name', 'unknown-call', 'unknown-name', "
            b"'unknown-call', 'unknown-name', 'unknown-call', "
            b"'fuzzy-delete-target-unresolved']\n"
            b'os.remove unknown-name:trusted_keyword_call.result\n',
        )
        self.assertEqual(err, b'')

    def test_c_recovery_bypasses_a_replaced_factory_constructor(self):
        code = textwrap.dedent("""
            import fuzzy_runtime as fuzzy
            real_type = fuzzy.FuzzyValue
            real_type.__new__ = staticmethod(
                lambda cls, provenance: 'forged:' + provenance
            )
            value = invalid_factory_missing
            print(isinstance(value, real_type), value.provenance)
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b'True unknown-name:invalid_factory_missing\n',
        )
        self.assertEqual(err, b'')

    def test_direct_import_bypasses_replaced_module_constructors(self):
        code = textwrap.dedent("""
            import os
            import fuzzy_runtime as fuzzy
            real_type = fuzzy.FuzzyModule
            public = real_type('public-module')
            real_type.__new__ = staticmethod(
                lambda cls, name, doc=None: public
            )
            real_type.__init__ = lambda self, name, doc=None: None
            fuzzy._ensure_fuzzy_module = lambda name: public
            import trusted_module_constructor as created
            print(created is public)
            print(type(created) is real_type, created.__name__)
            os.remove(created.TARGET)
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b'False\nTrue trusted_module_constructor\n',
        )
        self.assertEqual(err, b'')

    def test_direct_delete_checkpoint_ignores_python_event_replacement(self):
        code = textwrap.dedent("""
            import os
            import _fuzzy_runtime as state
            import fuzzy_runtime as fuzzy
            fuzzy._emit_event = lambda *args, **kwargs: None
            target = trusted_delete_missing
            os.remove(os.path.join('/tmp', target))
            events = state.get_events()
            print([event['code'] for event in events])
            print(events[-1]['operation'], events[-1]['provenance'])
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b"['unknown-name', 'fuzzy-delete-target-unresolved']\n"
            b'os.remove unknown-name:trusted_delete_missing\n',
        )
        self.assertEqual(err, b'')

    def test_direct_missing_user_import_becomes_fuzzy(self):
        code = textwrap.dedent("""
            import importlib
            import fuzzy_runtime as fuzzy
            import fuzzy_missing_package.child
            from fuzzy_missing_package import client
            dynamic = importlib.import_module(
                'fuzzy_dynamic_package.child'
            )
            print(isinstance(fuzzy_missing_package, fuzzy.FuzzyModule))
            print(isinstance(client, fuzzy.FuzzyValue))
            print(client.provenance)
            print(isinstance(dynamic, fuzzy.FuzzyModule))
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b'True\nTrue\n'
            b'unknown-import:fuzzy_missing_package.client\nTrue\n',
        )
        self.assertEqual(err, b'')

    def test_fuzzy_import_opcode_uses_captured_original_importer(self):
        code = textwrap.dedent("""
            import builtins
            import fuzzy_runtime as fuzzy
            class Sentinel:
                pass
            builtins.__import__ = lambda *args, **kwargs: Sentinel()
            fuzzy._real_import = lambda *args, **kwargs: Sentinel()
            fuzzy._fuzzy_import_post.__code__ = (
                lambda *args: 17
            ).__code__
            import math
            import fuzzy_captured_import_target
            print(math.sqrt(9))
            print(isinstance(
                fuzzy_captured_import_target, fuzzy.FuzzyModule
            ))
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(out, b'3.0\nTrue\n')
        self.assertEqual(err, b'')

    def test_standard_import_opcode_honors_builtin_override(self):
        code = textwrap.dedent("""
            import builtins
            class Sentinel:
                value = 'custom-import'
            real_import = builtins.__import__
            def custom_import(name, *args, **kwargs):
                if name == 'custom_import_target':
                    return Sentinel()
                return real_import(name, *args, **kwargs)
            builtins.__import__ = custom_import
            import custom_import_target
            print(custom_import_target.value)
        """)
        rc, out, err = assert_python_ok('-S', '-c', code)
        self.assertEqual(out, b'custom-import\n')
        self.assertEqual(err, b'')

    def test_import_module_relative_direct_miss_becomes_fuzzy(self):
        with os_helper.temp_dir() as directory:
            package = os.path.join(directory, 'fuzzy_dynamic_relative')
            os.mkdir(package)
            script_helper.make_script(package, '__init__', '')
            script_helper.make_script(
                package,
                'child',
                textwrap.dedent("""
                    import importlib
                    value = importlib.import_module(
                        '.missing', __package__
                    ).client
                """),
            )
            code = textwrap.dedent("""
                import fuzzy_dynamic_relative.child as child
                print(child.value.provenance)
            """)
            rc, out, err = assert_python_ok(
                '-S', '-X', 'fuzzy', '-c', code,
                __isolated=False, __cwd=directory,
            )
            self.assertEqual(
                out,
                b'unknown-import:fuzzy_dynamic_relative.missing.client\n',
            )
            self.assertEqual(err, b'')

    def test_stdlib_transitive_missing_import_stays_native(self):
        code = textwrap.dedent("""
            try:
                import test.fuzzy_import_helper
            except ModuleNotFoundError as error:
                print(error.name)
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(out, b'fuzzy_missing_stdlib_dependency\n')
        self.assertEqual(err, b'')

        code = textwrap.dedent("""
            try:
                import test.fuzzy_import_module_helper
            except ModuleNotFoundError as error:
                print(error.name)
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b'fuzzy_missing_dynamic_stdlib_dependency\n',
        )
        self.assertEqual(err, b'')

    def test_import_module_stays_native_in_standard_mode(self):
        code = textwrap.dedent("""
            import importlib
            try:
                importlib.import_module('fuzzy_missing_dynamic_module')
            except ModuleNotFoundError as error:
                print(error.name)
        """)
        rc, out, err = assert_python_ok('-S', '-c', code)
        self.assertEqual(out, b'fuzzy_missing_dynamic_module\n')
        self.assertEqual(err, b'')

    def test_missing_global_and_local_become_fuzzy(self):
        code = textwrap.dedent("""
            import fuzzy_runtime as fuzzy
            first_global = fuzzy_missing_global
            second_global = fuzzy_missing_global
            def read_local():
                first = fuzzy_missing_local
                second = fuzzy_missing_local
                fuzzy_missing_local = 'later'
                return first, second
            first_local, second_local = read_local()
            print(first_global is second_global, first_global.provenance)
            print(first_local is second_local, first_local.provenance)
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b'True unknown-name:fuzzy_missing_global\n'
            b'True unknown-local:read_local.fuzzy_missing_local\n',
        )
        self.assertEqual(err, b'')

    def test_missing_names_stay_native_in_standard_and_stdlib_code(self):
        standard = textwrap.dedent("""
            try:
                fuzzy_missing_global
            except NameError:
                print('name-error')
            def read_local():
                try:
                    fuzzy_missing_local
                except UnboundLocalError:
                    print('local-error')
                fuzzy_missing_local = 'later'
            read_local()
        """)
        rc, out, err = assert_python_ok('-S', '-c', standard)
        self.assertEqual(out, b'name-error\nlocal-error\n')
        self.assertEqual(err, b'')

        stdlib = textwrap.dedent("""
            try:
                import test.fuzzy_name_helper
            except NameError as error:
                print(error.name)
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', stdlib)
        self.assertEqual(out, b'fuzzy_missing_stdlib_name\n')
        self.assertEqual(err, b'')

        rc, out, err = assert_python_ok(
            '-S', '-X', 'fuzzy', '-m', 'test.fuzzy_stdlib_main_helper'
        )
        self.assertEqual(out, b'stdlib-main-native\n')
        self.assertEqual(err, b'')

    def test_missing_attributes_cover_user_builtin_extension_and_module(self):
        code = textwrap.dedent("""
            import _io
            import fuzzy_runtime as fuzzy
            import test.fuzzy_name_helper

            class UserValue:
                pass

            values = [
                UserValue().missing,
                object().missing,
                (1).missing,
                _io.BytesIO().missing,
                test.fuzzy_name_helper.missing,
                UserValue().missing_method(),
            ]
            class Base:
                pass
            class Child(Base):
                def missing_super(self):
                    return super().missing
            values.append(Child().missing_super())
            print([value.provenance for value in values])
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b"['unknown-attribute:UserValue.missing', "
            b"'unknown-attribute:object.missing', "
            b"'unknown-attribute:int.missing', "
            b"'unknown-attribute:BytesIO.missing', "
            b"'unknown-attribute:test.fuzzy_name_helper.missing', "
            b"'unknown-attribute:UserValue.missing_method.result', "
            b"'unknown-attribute:super.missing']\n",
        )
        self.assertEqual(err, b'')

    def test_attribute_recovery_does_not_swallow_descriptor_errors(self):
        code = textwrap.dedent("""
            class BrokenDescriptor:
                @property
                def value(self):
                    raise RuntimeError('descriptor failed')

            try:
                BrokenDescriptor().value
            except RuntimeError as error:
                print(error)
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(out, b'descriptor failed\n')
        self.assertEqual(err, b'')

    def test_general_attribute_c_apis_retain_native_errors(self):
        code = textwrap.dedent("""
            import operator
            import fuzzy_runtime as fuzzy

            value = object()
            print(isinstance(value.direct_missing, fuzzy.FuzzyValue))
            print(isinstance(value.direct_method(), fuzzy.FuzzyValue))
            for label, operation in (
                ('getattr', lambda: getattr(value, 'missing')),
                ('attrgetter', lambda: operator.attrgetter('missing')(value)),
            ):
                try:
                    operation()
                except AttributeError:
                    print(label, 'attribute-error')
            print('hasattr', hasattr(value, 'missing'))
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b'True\nTrue\ngetattr attribute-error\n'
            b'attrgetter attribute-error\nhasattr False\n',
        )
        self.assertEqual(err, b'')

    def test_standard_attribute_error_context_is_preserved(self):
        code = textwrap.dedent("""
            class UserValue:
                pass

            value = UserValue()
            try:
                value.missing
            except AttributeError as error:
                print(error.name, error.obj is value)
        """)
        rc, out, err = assert_python_ok('-S', '-c', code)
        self.assertEqual(out, b'missing True\n')
        self.assertEqual(err, b'')

    def test_dynamic_user_code_retains_fuzzy_recovery(self):
        code = textwrap.dedent("""
            source = '''
            dynamic_missing_global()
            def invoke():
                dynamic_missing_local()
                dynamic_missing_local = None
            invoke()
            '''
            exec(compile(source, '<generated-user-code>', 'exec'))
            print('reached')
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(out, b'reached\n')
        self.assertEqual(err, b'')

    def test_uninitialized_local_recovery_preserves_tracing_and_locals(self):
        code = textwrap.dedent("""
            import fuzzy_runtime as fuzzy
            import sys

            traced_presence = []
            def trace(frame, event, argument):
                if frame.f_code.co_name == 'invoke' and event == 'line':
                    traced_presence.append(
                        'missing_local' in frame.f_locals
                    )
                return trace

            def invoke():
                value = missing_local
                print(locals()['missing_local'] is value)
                missing_local = None
                return value

            sys.settrace(trace)
            value = invoke()
            sys.settrace(None)
            print(isinstance(value, fuzzy.FuzzyValue))
            print(any(traced_presence))
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(out, b'True\nTrue\nTrue\n')
        self.assertEqual(err, b'')

    def test_fuzzy_delete_targets_emit_without_calling_os(self):
        code = textwrap.dedent("""
            import os
            from pathlib import Path
            import shutil
            import fuzzy_runtime as fuzzy
            target = fuzzy.FuzzyValue('unknown-name:target')
            class PathProxy:
                def __fspath__(self):
                    return os.fspath(target)
            os.remove(path=target)
            os.unlink(os.path.join('/tmp', target))
            os.rmdir(os.fsencode(target))
            os.removedirs(target)
            os.remove(PathProxy())
            target.unlink()
            (Path(target) / 'child').unlink()
            Path(target).parent.rmdir()
            shutil.rmtree(path=target)
            events = [
                event for event in fuzzy.get_events()
                if event['kind'] == 'unresolved-operation'
            ]
            print([event['operation'] for event in events])
            print([event['provenance'] for event in events])
        """)
        rc, out, err = assert_python_ok('-S', '-X', 'fuzzy', '-c', code)
        self.assertEqual(
            out,
            b"['os.remove', 'os.unlink', 'os.rmdir', 'os.removedirs', "
            b"'os.remove', 'Path.unlink', 'Path.unlink', 'Path.rmdir', "
            b"'shutil.rmtree']\n"
            b"['unknown-name:target', 'unknown-name:target', "
            b"'unknown-name:target', 'unknown-name:target', "
            b"'unknown-name:target', "
            b"'unknown-name:target', "
            b"'unknown-name:target.path.operation', "
            b"'unknown-name:target.path.operation', "
            b"'unknown-name:target']\n",
        )
        self.assertEqual(err, b'')

    def test_fuzzy_mode_preserves_shutil_symlink_safety_capability(self):
        code = (
            'import shutil; '
            'print(shutil.rmtree.avoids_symlink_attacks)'
        )
        rc, standard_out, err = assert_python_ok('-S', '-c', code)
        self.assertEqual(err, b'')
        rc, fuzzy_out, err = assert_python_ok(
            '-S', '-X', 'fuzzy', '-c', code
        )
        self.assertEqual(err, b'')
        self.assertEqual(fuzzy_out, standard_out)


if __name__ == '__main__':
    unittest.main()

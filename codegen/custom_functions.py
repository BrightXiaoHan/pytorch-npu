import re
from collections import namedtuple, defaultdict
from typing import List, Dict, Sequence
import yaml

from torchgen.code_template import CodeTemplate
from torchgen.gen import (parse_tags_yaml, LineLoader, FileManager, cpp_string, error_check_native_functions)
from torchgen.model import (BackendIndex, DispatchKey, Location, Variant,
                            NativeFunction, OperatorName, BackendMetadata)
from torchgen.utils import concatMap, context
from torchgen.context import with_native_function, native_function_manager
from torchgen.api.types import DispatcherSignature
from torchgen.api import cpp
from codegen.utils import (enable_opplugin, is_op_valid, filed_tag, get_opplugin_wrap_name, PathManager)


# Parse native_functions.yaml into a sequence of NativeFunctions and Backend Indices.
ParsedYaml = namedtuple('ParsedYaml', ['native_functions', 'backend_indices'])


CUSTOM_FUNCTIONS_DECLARATION = CodeTemplate("""\
${return_type} ${func_name}(${args_str});
""")

EXPORT_CUSTOM_FUNCTIONS_DECLARATION = CodeTemplate("""\
__attribute__((__visibility__("default"))) \
${return_type} ${func_name}(${args_str});
""")

CUSTOM_FUNCTIONS_DEFINITION = CodeTemplate("""\
${return_type} ${func_name}(${args_str}) {
    static auto op = c10::Dispatcher::singleton().findSchemaOrThrow("npu::${base_name}", "${overload}").typed<${schema}>();
    return op.${func_type}(${args_exprs_str});
}
""")


def parse_custom_yaml(custom_path: str, tag_path: str) -> ParsedYaml:
    valid_tags = parse_tags_yaml(tag_path)
    rs: List[NativeFunction] = []
    bs: Dict[DispatchKey, Dict[OperatorName, BackendMetadata]] = defaultdict(dict)
    # Filter the custom native yaml file, and extract the functions we defined.
    from io import StringIO
    f_str = StringIO()
    PathManager.check_directory_path_readable(custom_path)
    with open(custom_path, 'r') as f:
        for line in f:
            if line.split(':')[0] in ['backend', 'cpp_namespace', 'extra_headers',
                                      'supported', 'autograd']:
                flag = False
                continue
            if line.split(':')[0] in ['custom', 'custom_autograd']:
                flag = True
                continue
            if ':' not in line or not flag:
                continue
            f_str.write(line)

    f_str.seek(0)
    custom_es = yaml.safe_load(f_str)
    custom_es = filed_tag(custom_es)
    for e_with_vars in custom_es:
        func, m = NativeFunction.from_yaml(e_with_vars, "Location", valid_tags)
        func.variants.discard(Variant.method)
        rs.append(func)
        BackendIndex.grow_index(bs, m)

    error_check_native_functions(rs)
    # Default dict is to prevent the codegen from barfing when we have a dispatch key that has no kernels yet.
    indices: Dict[DispatchKey, BackendIndex] = defaultdict(lambda: BackendIndex(
        dispatch_key=DispatchKey.Undefined, use_out_as_primary=True, external=False, index={}))
    for k, v in bs.items():
        # All structured in-tree operators are implemented in terms of their out operator.
        indices[k] = BackendIndex(dispatch_key=k,
                                  use_out_as_primary=True,
                                  external=False,
                                  device_guard=False,
                                  index=v)
    return ParsedYaml(rs, indices)


METHOD_DEFINITION = CodeTemplate("""\
${return_type} ${name}(${args_str}) {
  ${unpack_out}
  ${type_definition_body}
}

""")

TRACE_DISPATCH = CodeTemplate("""\
return ${impl_name}(${args_exprs_str});""")


@with_native_function
def compute_op_definition(f: NativeFunction):
    out_num = len(f.func.arguments.out)
    sig = DispatcherSignature.from_schema(f.func, prefix=f'wrapper_{f.func.name.overload_name}_')
    name = sig.name()
    args = sig.arguments()
    args_str = ', '.join(a.defn() for a in args)

    args_exprs_str = ', '.join(a.name for a in args)

    impl_name = f"at_npu::native::NPUNativeFunctions::{cpp.name(f.func)}"

    if enable_opplugin() and is_op_valid(str(f.func.name)):
        impl_name = f"op_plugin::{get_opplugin_wrap_name(f)}"

    check_out = [f'TORCH_CHECK(out.size() == {out_num}, "expected tuple of {out_num} elements but got ", out.size());']
    unpack_out = check_out + [f'at::Tensor {args[-out_num + i].name} = out[{i}];' for i in range(out_num)] \
        if out_num > 1 else ''
    out_return_type = '::std::tuple<{}>'.format(', '.join(['at::Tensor'] * out_num))

    return [METHOD_DEFINITION.substitute(
        return_type=out_return_type if out_num > 1 else cpp.returns_type(f.func.returns).cpp_type(),
        name=name,
        args_str=','.join(a.defn() for a in args[:-out_num]) + ', at::TensorList out' if out_num > 1 else args_str,
        unpack_out=unpack_out,
        type_definition_body=[TRACE_DISPATCH.substitute(impl_name=impl_name, args_exprs_str=args_exprs_str)]
    )]


@with_native_function
def compute_register_symbol(f: NativeFunction):
    out_num = len(f.func.arguments.out)
    if out_num > 1:
        decl = re.compile(r"(?P<name>[^\(]+)\((?P<args>.*)\) -> (?P<returns>.*)").findall(str(f.func))[0]
        func_schema = decl[0] + '(' + ','.join(decl[1].split(',')[:-out_num]) + ', Tensor[] out) -> (' + ', '.join(
            ['Tensor'] * out_num) + ')'
    else:
        func_schema = str(f.func)
    if f.has_composite_explicit_autograd_kernel:
        name = DispatcherSignature.from_schema(f.func, prefix=f'wrapper_{f.func.name.overload_name}_').name()
        return [f'm.def({cpp_string(func_schema)}, TORCH_FN(at_npu::native::{name}));\n']
    else:
        return [f'm.def({cpp_string(func_schema)});\n']


@with_native_function
def compute_register_impl(f: NativeFunction):
    if f.has_composite_explicit_autograd_kernel:
        return []
    else:
        name = DispatcherSignature.from_schema(f.func, prefix=f'wrapper_{f.func.name.overload_name}_').name()
        return [f'm.impl("{f.func.name}", TORCH_FN(at_npu::native::{name}));\n']


def gen_custom_trace(fm: FileManager, custom_trace_functions: Sequence[NativeFunction]):

    fm.write_with_template(f'CustomRegisterSchema.cpp', 'CustomRegisterSchema.cpp', lambda: {
        'custom_op_definitions': list(concatMap(
            lambda f: compute_op_definition(f),
            custom_trace_functions
        )),
        'custom_schema_registrations': list(concatMap(
            lambda f: compute_register_symbol(f),
            custom_trace_functions
        )),
        'custom_impl_registrations': list(concatMap(
            lambda f: compute_register_impl(f),
            custom_trace_functions
        )),
    })


def gen_custom_ops_patch(fm: FileManager, custom_trace_functions: Sequence[NativeFunction]):
    fm.write_with_template(f'custom_ops.py', 'custom_ops.py', lambda: {
        'custom_ops': [f'torch_npu.{ops} = torch.ops.npu.{ops}'
                       for ops in set([f.func.name.name for f in custom_trace_functions])],
    })


def compute_custom_functions_declaration(f: NativeFunction, func_type: str):
    with native_function_manager(f):
        sig = DispatcherSignature.from_schema(f.func)
        name = sig.name()
        args = sig.arguments()
        if func_type == 'call':
            args_str = ', '.join(a.defn() for a in args)
        if func_type == 'redispatch':
            args_str = 'c10::DispatchKeySet dispatchKeySet, ' + ', '.join(a.defn() for a in args)

        if (func_type == 'call') and (name == 'npu_slice_out'):
            return [EXPORT_CUSTOM_FUNCTIONS_DECLARATION.substitute(
                    return_type=cpp.returns_type(f.func.returns).cpp_type(),
                    func_name=name,
                    args_str=args_str,)]

        return [CUSTOM_FUNCTIONS_DECLARATION.substitute(
                return_type=cpp.returns_type(f.func.returns).cpp_type(),
                func_name=name,
                args_str=args_str,)]


def compute_custom_functions_definition(f: NativeFunction, func_type: str):
    with native_function_manager(f):
        sig = DispatcherSignature.from_schema(f.func)
        name = sig.name()
        args = sig.arguments()
        if func_type == 'call':
            args_str = ', '.join(a.defn() for a in args)
            args_exprs_str = ', '.join(a.name for a in args)
        if func_type == 'redispatch':
            args_str = 'c10::DispatchKeySet dispatchKeySet, ' + ', '.join(a.defn() for a in args)
            args_exprs_str = 'dispatchKeySet, ' + ', '.join(a.name for a in args)

        return [CUSTOM_FUNCTIONS_DEFINITION.substitute(
                return_type=cpp.returns_type(f.func.returns).cpp_type(),
                base_name=f.func.name.name,
                func_name=name,
                overload=f.func.name.overload_name,
                args_str=args_str,
                func_type=func_type,
                schema=sig.type(),
                args_exprs_str=args_exprs_str,)]


def gen_custom_functions_dispatch(
    fm: FileManager,
    custom_functions: Sequence[NativeFunction]
) -> None:
    func_type_list = ['call', 'redispatch']
    file_name_list = ['CustomFunctions', 'CustomRedispatch']

    for func_type, file_name in zip(func_type_list, file_name_list):
        fm.write_with_template(
        f'{file_name}.h', f'{file_name}.h', lambda:{
        'custom_function_declarations':list(concatMap(
            lambda f: compute_custom_functions_declaration(f, func_type),
            custom_functions
            ))}
        )

        fm.write_with_template(
        f'{file_name}.cpp', f'{file_name}.cpp', lambda:{
        'custom_function_definitions':list(concatMap(
            lambda f: compute_custom_functions_definition(f, func_type),
            custom_functions
            ))}
        )

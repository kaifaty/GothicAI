import importlib
import pkgutil

import pytest

import gothic_ai
from gothic_ai.llm import Tool


def _tools():
    tools = {}
    modules = [gothic_ai.__name__]
    modules += [module.name for module in pkgutil.walk_packages(gothic_ai.__path__, "gothic_ai.")]
    for module_name in modules:
        module = importlib.import_module(module_name)
        for value in vars(module).values():
            candidates = value if isinstance(value, (list, tuple)) else (value,)
            for candidate in candidates:
                if isinstance(candidate, Tool):
                    tools[id(candidate)] = candidate
    assert tools, "No server tools found"
    return list(tools.values())


def _assert_strict_schema(schema, path):
    if isinstance(schema, dict):
        kind = schema.get("type")
        if kind == "object" or isinstance(kind, list) and "object" in kind:
            assert schema.get("additionalProperties") is False, path
            required = schema.get("required")
            assert isinstance(required, list), path
            assert sorted(required) == sorted(schema.get("properties", {})), path
        for key, value in schema.items():
            _assert_strict_schema(value, f"{path}.{key}")
    elif isinstance(schema, list):
        for index, value in enumerate(schema):
            _assert_strict_schema(value, f"{path}[{index}]")


@pytest.mark.parametrize("tool", _tools(), ids=lambda tool: tool.name)
def test_tools_are_strict_json_schemas(tool):
    _assert_strict_schema(tool.parameters, f"{tool.name}.parameters")

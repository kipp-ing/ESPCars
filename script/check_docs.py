#!/usr/bin/env python3
"""Check MDX key coverage against loaded ESPHome schemas, including automations.

Run in a fresh Python process: marker provenance is recorded while importing
components. Upstream entity/pin/automation options are linked by inheritance;
only keys declared by this repository require local descriptions. Inherited
keys may also be described, but arbitrary keys are rejected. No YAML validation,
code generation, network access or hardware is involved.
"""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass, field
from importlib import import_module
from pathlib import Path
import re
from types import FunctionType, ModuleType
from unittest.mock import patch

import voluptuous as vol

from esphome import automation
import esphome.components

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs" / "esphome.io" / "components"
KEY = re.compile(r"^\s*- \*\*([a-z][a-z0-9_]*)\*\*", re.MULTILINE)


@dataclass
class Keys:
    """Locally declared keys and all accepted keys at the same schema boundary."""

    required: set[str] = field(default_factory=set)
    accepted: set[str] = field(default_factory=set)
    sections: dict[str, Keys] = field(default_factory=dict)


def component_modules(root: Path = ROOT) -> list[str]:
    """Discover hubs and platforms, including directory-shaped LIN platforms."""
    modules = []
    for hub in sorted((root / "components").glob("*/__init__.py")):
        modules.append(f"esphome.components.{hub.parent.name}")
        for path in sorted(hub.parent.iterdir()):
            if path.name == "__init__.py":
                continue
            source = path / "__init__.py" if path.is_dir() else path
            if source.suffix == ".py" and source.is_file():
                tree = ast.parse(source.read_text(encoding="utf-8"))
                if any(
                    isinstance(node, (ast.FunctionDef, ast.Name))
                    and (node.name if isinstance(node, ast.FunctionDef) else node.id)
                    == "CONFIG_SCHEMA"
                    for node in ast.walk(tree)
                ):
                    modules.append(f"esphome.components.{hub.parent.name}.{path.stem}")
    return modules


def schema_keys(schema: object, owners: dict[int, str], module: ModuleType) -> Keys:
    """Walk schemas, wrappers, dispatch functions and locally hidden mappings."""
    result = Keys()
    seen: dict[int, object] = {}

    def visit(value):
        if id(value) in seen:
            return
        seen[id(value)] = value
        if isinstance(value, vol.Schema):
            visit(value.schema)
            for extra in getattr(value, "_extra_schemas", []):
                visit(extra)
        elif isinstance(value, dict):
            for marker, validator in value.items():
                key = marker.schema if isinstance(marker, vol.Marker) else marker
                if not isinstance(key, str):
                    continue  # Dynamic dictionaries (e.g. integer-to-text maps).
                result.accepted.add(key)
                if owners.get(id(marker), "").startswith("esphome.components."):
                    owner = owners[id(marker)]
                    if owner in local_modules:
                        result.required.add(key)
                        if (
                            isinstance(validator, FunctionType)
                            and validator.__module__ == "esphome.automation"
                            and validator.__qualname__.startswith(
                                "validate_automation."
                            )
                        ):
                            result.sections[f"{key} Trigger"] = schema_keys(
                                validator, owners, module
                            )
                        visit(validator)
        elif isinstance(value, (list, tuple)):
            for child in value:
                visit(child)
        elif isinstance(value, (vol.All, vol.Any)):
            visit(value.validators)
        elif isinstance(value, FunctionType):
            for cell in value.__closure__ or ():
                visit(cell.cell_contents)
            if value.__module__ not in local_modules:
                return
            # Dispatchers and recursive validators reference schemas by name.
            for name in value.__code__.co_names:
                target = value.__globals__.get(name)
                if name.endswith("SCHEMA") or (
                    isinstance(target, FunctionType)
                    and target.__module__ in local_modules
                ):
                    visit(target)
            # Validators such as portal._limit_bound construct a schema at call
            # time. Evaluate only schema expressions with module-global inputs.
            tree = ast.parse(
                Path(value.__code__.co_filename).read_text(encoding="utf-8")
            )
            definition = next(
                (
                    n
                    for n in tree.body
                    if isinstance(n, ast.FunctionDef) and n.name == value.__name__
                ),
                None,
            )
            if definition is None:
                return
            for node in ast.walk(definition):
                if (
                    isinstance(node, ast.Call)
                    and isinstance(node.func, ast.Attribute)
                    and isinstance(node.func.value, ast.Name)
                    and node.func.value.id == "cv"
                    and node.func.attr == "Schema"
                ):
                    expression = ast.Expression(node)
                    visit(
                        eval(
                            compile(expression, value.__code__.co_filename, "eval"),
                            value.__globals__,
                        )
                    )
                # Hand-validated mapping keys, e.g. absent_when.provider/source.
                if (
                    isinstance(node, ast.Compare)
                    and isinstance(node.left, ast.Name)
                    and node.left.id.startswith("CONF_")
                    and any(isinstance(op, (ast.In, ast.NotIn)) for op in node.ops)
                    and any(
                        isinstance(n, ast.Name) and n.id == "value"
                        for n in node.comparators
                    )
                ):
                    key = value.__globals__[node.left.id]
                    result.required.add(key)
                    result.accepted.add(key)

    # The collector initializes this set before importing any component.
    local_modules = set(component_modules())
    visit(schema)
    # rename_key accepts the old spelling before applying the visible schema.
    tree = ast.parse(Path(module.__file__).read_text(encoding="utf-8"))
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "rename_key"
        ):
            old = ast.literal_eval(node.args[0])
            result.required.add(old)
            result.accepted.add(old)
    return result


def load_schemas() -> dict[str, Keys]:
    """Import actual schemas and registered actions/conditions with provenance."""
    import sys

    modules = component_modules()
    if any(name in sys.modules for name in modules):
        raise RuntimeError(
            "Run check_docs.py in a fresh process before importing components"
        )
    esphome.components.__path__.insert(0, str(ROOT / "components"))
    owners: dict[int, str] = {}
    markers = []  # Keep identities alive throughout collection.
    original = vol.Marker.__init__

    def record(marker, *args, **kwargs):
        original(marker, *args, **kwargs)
        frame = sys._getframe(1)
        while frame:
            name = frame.f_globals.get("__name__", "")
            if name.startswith("esphome.components."):
                owners[id(marker)] = name
                markers.append(marker)
                break
            frame = frame.f_back

    result = {}
    with patch.object(vol.Marker, "__init__", record):
        loaded = [import_module(name) for name in modules]
        for module in loaded:
            result[module.__name__] = schema_keys(module.CONFIG_SCHEMA, owners, module)
            for registry in (automation.ACTION_REGISTRY, automation.CONDITION_REGISTRY):
                for name, entry in registry.items():
                    if entry.fun.__module__ == module.__name__:
                        keys = schema_keys(entry.raw_schema, owners, module)
                        kind = (
                            "Action"
                            if registry is automation.ACTION_REGISTRY
                            else "Condition"
                        )
                        result[module.__name__].sections[f"{name} {kind}"] = keys
                        result[module.__name__].required.update(keys.required)
                        result[module.__name__].accepted.update(keys.accepted)
    return result


def page_paths(schemas: dict[str, Keys]) -> dict[str, Path]:
    paths = {}
    for module in schemas:
        parts = module.split(".")[2:]
        component = parts[0]
        platforms = any(name.startswith(module + ".") for name in schemas)
        if len(parts) > 1:
            paths[module] = Path(component) / f"{parts[1]}.mdx"
        else:
            paths[module] = (
                Path(component) / "index.mdx" if platforms else Path(f"{component}.mdx")
            )
    return paths


def check_pages(schemas: dict[str, Keys], docs: Path) -> list[str]:
    errors = []
    paths = page_paths(schemas)
    for module, path in paths.items():
        page = docs / path
        if not page.is_file():
            errors.append(
                f"{path}: missing page ({len(schemas[module].required)} keys)"
            )
            continue
        text = page.read_text(encoding="utf-8")
        # Examples, inline mentions and comments do not satisfy documentation.
        prose = re.sub(r"```.*?```|<!--.*?-->", "", text, flags=re.DOTALL)
        documented = set(KEY.findall(prose))
        errors.extend(
            f"{path}: missing **{key}**"
            for key in sorted(schemas[module].required - documented)
        )
        errors.extend(
            f"{path}: unknown **{key}**"
            for key in sorted(documented - schemas[module].accepted)
        )
        for name, keys in schemas[module].sections.items():
            item, kind = name.rsplit(" ", 1)
            heading = f"### `{item}` {kind}"
            match = re.search(
                re.escape(heading) + r"\n(.*?)(?=^#{2,3} |\Z)",
                prose,
                re.DOTALL | re.MULTILINE,
            )
            if match is None:
                errors.append(f"{path}: missing {heading}")
                continue
            section = match.group(1)
            if "**Configuration variables:**" not in section:
                errors.append(f"{path}: {name}: missing Configuration variables list")
            if "```yaml" not in text.split(heading, 1)[1].split("\n### ", 1)[0]:
                errors.append(f"{path}: {name}: missing YAML example")
            section_keys = set(KEY.findall(section))
            errors.extend(
                f"{path}: {name}: missing **{key}**"
                for key in sorted(keys.required - section_keys)
            )
            errors.extend(
                f"{path}: {name}: unknown **{key}**"
                for key in sorted(section_keys - keys.accepted)
            )
    errors.extend(
        f"{page.relative_to(docs)}: no component/platform schema"
        for page in sorted(docs.rglob("*.mdx"))
        if page.relative_to(docs) not in paths.values()
    )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--docs-root", type=Path, default=DOCS)
    args = parser.parse_args()
    schemas = load_schemas()
    errors = check_pages(schemas, args.docs_root)
    for error in errors:
        print(error)
    print(
        f"Docs: {len(schemas)} pages, {sum(len(s.required) for s in schemas.values())} keys, {len(errors)} findings"
    )
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())

"""Blocking docs/owner checks exercise failures as well as the current tree."""

from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess
import sys
from types import ModuleType

import pytest

from esphome import config_validation as cv
from script import build_codeowners, check_docs

ROOT = Path(__file__).resolve().parents[2]
DOCS = ROOT / "docs" / "esphome.io" / "components"


def run(script, *args):
    return subprocess.run(
        [sys.executable, str(ROOT / "script" / script), *map(str, args)],
        capture_output=True,
        text=True,
        check=False,
        cwd=ROOT,
    )


@pytest.fixture
def pages(tmp_path):
    destination = tmp_path / "components"
    shutil.copytree(DOCS, destination)
    return destination


def test_current_docs_pass():
    result = run("check_docs.py")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "21 pages" in result.stdout
    assert "0 findings" in result.stdout


@pytest.mark.parametrize(
    ("page", "key"),
    [
        ("can_gateway/index.mdx", "crc_byte"),
        ("linbus/sensor.mdx", "master_requests"),
        ("portal.mdx", "below"),
        ("portal.mdx", "provider"),
        ("uds/text_sensor.mdx", "element"),
        ("isotp/sensor.mdx", "messages_received"),
    ],
)
def test_missing_nested_platform_and_alias_keys_fail(pages, page, key):
    path = pages / page
    path.write_text(
        path.read_text(encoding="utf-8").replace(f"**{key}**", f"`{key}`"),
        encoding="utf-8",
    )
    result = run("check_docs.py", "--docs-root", pages)
    assert result.returncode == 1
    assert f"{page}: missing **{key}**" in result.stdout


def test_action_needs_its_own_keys_even_if_documented_elsewhere(pages):
    path = pages / "ccp.mdx"
    text = path.read_text(encoding="utf-8")
    start = text.index("### `ccp.short_upload` Action")
    end = text.index("\n### ", start + 1)
    text = (
        text[:start] + text[start:end].replace("**address**", "`address`") + text[end:]
    )
    path.write_text(text, encoding="utf-8")
    result = run("check_docs.py", "--docs-root", pages)
    assert result.returncode == 1
    assert "ccp.short_upload Action: missing **address**" in result.stdout


def test_missing_trigger_heading_fails(pages):
    path = pages / "isotp" / "index.mdx"
    path.write_text(
        path.read_text(encoding="utf-8").replace(
            "### `on_error` Trigger", "### Error Handling"
        ),
        encoding="utf-8",
    )
    result = run("check_docs.py", "--docs-root", pages)
    assert result.returncode == 1
    assert "missing ### `on_error` Trigger" in result.stdout


def test_unknown_key_fails_and_examples_do_not_supply_descriptions(pages):
    path = pages / "isotp" / "sensor.mdx"
    text = path.read_text(encoding="utf-8").replace(
        "**frames_ignored**", "`frames_ignored`"
    )
    text += "\n- **invented_key** (*Optional*, int): Invalid option.\n"
    text += "\n```yaml\n# - **frames_ignored**\n```\n<!-- - **frames_ignored** -->\n"
    path.write_text(text, encoding="utf-8")
    result = run("check_docs.py", "--docs-root", pages)
    assert result.returncode == 1
    assert "unknown **invented_key**" in result.stdout
    assert "missing **frames_ignored**" in result.stdout


def test_missing_platform_page_fails(pages):
    (pages / "linbus" / "binary_sensor.mdx").unlink()
    result = run("check_docs.py", "--docs-root", pages)
    assert result.returncode == 1
    assert "linbus/binary_sensor.mdx: missing page" in result.stdout


def test_loaded_schema_traverses_comprehensions_wrappers_and_inheritance(
    tmp_path, monkeypatch
):
    module = ModuleType("esphome.components.fictional")
    source = tmp_path / "__init__.py"
    source.write_text("", encoding="utf-8")
    module.__file__ = str(source)
    own = cv.Required("new_nested_key")
    parent = cv.Optional("new_container")
    inherited = cv.Optional("inherited_option")
    nested = cv.Schema({own: cv.boolean})
    schema = cv.All(cv.Schema({parent: cv.ensure_list(nested), inherited: cv.string}))
    monkeypatch.setattr(check_docs, "component_modules", lambda: [module.__name__])
    owners = {id(own): module.__name__, id(parent): module.__name__}
    keys = check_docs.schema_keys(schema, owners, module)
    assert keys.required == {"new_container", "new_nested_key"}
    assert keys.accepted == {"new_container", "new_nested_key", "inherited_option"}
    keys.sections["fictional.check Condition"] = check_docs.Keys(
        {"new_nested_key"}, {"new_nested_key"}
    )
    page = tmp_path / "fictional.mdx"
    page.write_text(
        "- **new_container**\n- **new_nested_key**\n### `fictional.check` Condition\n\n"
        "```yaml\nif:\n  condition: fictional.check\n```\n\n**Configuration variables:**\n",
        encoding="utf-8",
    )
    errors = check_docs.check_pages({module.__name__: keys}, tmp_path)
    assert any("Condition: missing **new_nested_key**" in error for error in errors)


@pytest.fixture
def owner_repo(tmp_path):
    hub = tmp_path / "components" / "fictional"
    hub.mkdir(parents=True)
    (hub / "__init__.py").write_text(
        'CODEOWNERS = ["@zeta", "@alpha", "@alpha"]\n', encoding="utf-8"
    )
    (hub / "sensor.py").write_text('CODEOWNERS = ["@beta"]\n', encoding="utf-8")
    folder = hub / "binary_sensor"
    folder.mkdir()
    (folder / "__init__.py").write_text('CODEOWNERS = ["@gamma"]\n', encoding="utf-8")
    return tmp_path


def test_codeowners_generation_and_staleness_are_blocking(owner_repo):
    result = run("build_codeowners.py", "--root", owner_repo)
    assert result.returncode == 0, result.stdout + result.stderr
    content = (owner_repo / "CODEOWNERS").read_text(encoding="utf-8")
    assert "/components/fictional/* @alpha @beta @zeta\n" in content
    assert "/components/fictional/binary_sensor/* @gamma\n" in content
    assert run("build_codeowners.py", "--root", owner_repo, "--check").returncode == 0
    (owner_repo / "components" / "fictional" / "sensor.py").write_text(
        'CODEOWNERS = ["@changed"]\n', encoding="utf-8"
    )
    stale = run("build_codeowners.py", "--root", owner_repo, "--check")
    assert stale.returncode == 1
    assert "stale" in stale.stdout
    assert (owner_repo / "CODEOWNERS").read_text(encoding="utf-8") == content


@pytest.mark.parametrize(
    "declaration", ["", "CODEOWNERS = []\n", 'CODEOWNERS = ["missing-at"]\n']
)
def test_missing_or_invalid_hub_owners_fail(owner_repo, declaration):
    (owner_repo / "components" / "fictional" / "__init__.py").write_text(
        declaration, encoding="utf-8"
    )
    with pytest.raises(ValueError, match="CODEOWNERS"):
        build_codeowners.generate(owner_repo)


def test_missing_and_shadowed_codeowners_fail(owner_repo):
    assert run("build_codeowners.py", "--root", owner_repo, "--check").returncode == 1
    assert run("build_codeowners.py", "--root", owner_repo).returncode == 0
    shadow = owner_repo / ".github" / "CODEOWNERS"
    shadow.parent.mkdir()
    shadow.write_text("* @someone_else\n", encoding="utf-8")
    result = run("build_codeowners.py", "--root", owner_repo, "--check")
    assert result.returncode == 1
    assert "shadows" in result.stdout


def test_current_codeowners_pass():
    result = run("build_codeowners.py", "--check")
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("path", sorted(DOCS.rglob("*.mdx")))
def test_mdx_shape(path):
    text = path.read_text(encoding="utf-8")
    assert re.match(r'---\ntitle: "[^"\n]+"\ndescription: "[^"\n]+"\n---\n', text)
    assert "import APIRef from '@components/APIRef.astro';" in text
    assert not re.search(r"^# ", text, re.MULTILINE)
    assert "<span id=" not in text
    assert "## Configuration variables\n" in text
    assert "## See Also\n" in text
    assert text.rstrip().endswith("/>")
    assert max(map(len, text.splitlines())) <= 120
    assert not re.search(r"GPIO\d+", text)
    for link in re.findall(r"\]\((/[^)#]+)\)", text):
        assert link.endswith("/"), link
    assert "ESP32" in text and "ESP-IDF" in text and "C5" in text


def test_private_data_guard_includes_untracked_mdx(tmp_path, monkeypatch, capsys):
    from script import check_no_private_data

    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    page = tmp_path / "fictional.mdx"
    page.write_text("fictional_private_marker\n", encoding="utf-8")
    monkeypatch.setattr(check_no_private_data, "REPO_ROOT", tmp_path)
    monkeypatch.setattr(
        check_no_private_data,
        "PATTERNS",
        [(re.compile("fictional_private_marker"), "fictional test marker")],
    )
    assert page in check_no_private_data.tracked_files()
    assert check_no_private_data.main() == 1
    assert "fictional.mdx:1" in capsys.readouterr().out
    page.write_text("Fictional public example\n", encoding="utf-8")
    assert check_no_private_data.main() == 0


def test_docs_and_owner_checks_block_local_and_required_ci_lint():
    import yaml

    workflow = yaml.safe_load(
        (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
    )
    lint = workflow["jobs"]["upstream-lint"]
    assert not lint.get("continue-on-error", False)
    assert "upstream-lint" in workflow["jobs"]["required-checks"]["needs"]
    local = (ROOT / "script" / "check.sh").read_text(encoding="utf-8")
    for command in ("script/check_docs.py", "script/build_codeowners.py --check"):
        assert command in local
        steps = [step for step in lint["steps"] if command in step.get("run", "")]
        assert len(steps) == 1
        assert not steps[0].get("continue-on-error", False)
        assert "||" not in steps[0]["run"]


@pytest.mark.parametrize("path", sorted(DOCS.rglob("*.mdx")))
def test_yaml_examples_parse_and_open_with_passive_configuration(path):
    import json

    import yaml

    examples = [
        yaml.safe_load(block)
        for block in re.findall(
            r"```yaml\n(.*?)```", path.read_text(encoding="utf-8"), re.DOTALL
        )
    ]
    assert examples and all(isinstance(example, dict) for example in examples)
    first = examples[0]
    if "can_gateway" in first:
        assert all(port["listen_only"] for port in first["can_gateway"]["ports"])
        assert not first["can_gateway"].get("routes")
        assert not first["can_gateway"].get("cyclic_sends")
    if "linbus" in first:
        assert first["linbus"]["mode"] == "listener"
    if "sensor" in first:
        assert all(
            "field" not in sensor
            for sensor in first["sensor"]
            if sensor["platform"] == "uds"
        )
    if "text_sensor" in first:
        assert all(
            sensor["update_interval"] == "never"
            for sensor in first["text_sensor"]
            if sensor["platform"] == "uds"
        )
    serialized = json.dumps(first)
    for component in ("can_gateway", "ccp", "isotp", "uds", "linbus"):
        assert f'"{component}.send"' not in serialized
        assert f'"{component}.read"' not in serialized
    manifest = json.loads(
        (ROOT / "tests" / "uds" / "fixtures" / "mini.manifest.json").read_text(
            encoding="utf-8"
        )
    )
    fields = {
        field["name"] for group in manifest["groups"] for field in group["fields"]
    }
    services = {group["name"] for group in manifest["groups"]} | {
        group["did"] for group in manifest["groups"]
    }

    def check_references(value):
        if isinstance(value, dict):
            if "field" in value:
                assert value["field"] in fields
            if "service" in value:
                assert value["service"] in services
            for child in value.values():
                check_references(child)
        elif isinstance(value, list):
            for child in value:
                check_references(child)

    for example in examples:
        check_references(example)

"""Pytest fixtures for ESPCars component tests.

The component suites under ``tests/<component>/`` were written as ESPHome
in-tree tests and import ``esphome.components.<component>`` directly. The
path hook below maps that namespace onto this repo's ``components/``
directory, so the suites run unmodified against the installed esphome.

Fixtures are adapted from esphome's ``tests/component_tests/conftest.py``
(© ESPHome authors, GPLv3/MIT).
"""

from __future__ import annotations

from collections.abc import Callable, Generator
from pathlib import Path
from typing import Any, Protocol

import pytest

from esphome import config, final_validate
import esphome.components
from esphome.config import Config
from esphome.const import (
    KEY_CORE,
    KEY_TARGET_FRAMEWORK,
    KEY_TARGET_PLATFORM,
    PlatformFramework,
)
from esphome.core import CORE
from esphome.types import ConfigType

# Make `import esphome.components.<name>` resolve to this repo's components/.
_COMPONENTS_DIR = Path(__file__).parent.parent / "components"
if _COMPONENTS_DIR.as_posix() not in esphome.components.__path__:
    esphome.components.__path__.append(_COMPONENTS_DIR.as_posix())


class SetCoreConfigCallable(Protocol):
    """Protocol for the set_core_config fixture setter function."""

    def __call__(  # noqa: E704
        self,
        platform_framework: PlatformFramework,
        /,
        *,
        core_data: ConfigType | None = None,
        platform_data: ConfigType | None = None,
        full_config: dict[str, ConfigType] | None = None,
    ) -> None: ...


@pytest.fixture(autouse=True)
def config_path(request: pytest.FixtureRequest) -> Generator[None]:
    """Set CORE.config_path to the test's directory and reset it after the test."""
    original_path = CORE.config_path
    config_dir = Path(request.fspath).parent / "config"

    if config_dir.exists():
        CORE.config_path = config_dir / "dummy.yaml"
    else:
        CORE.config_path = Path(request.fspath).parent / "dummy.yaml"

    yield
    CORE.config_path = original_path


@pytest.fixture(autouse=True)
def reset_core() -> Generator[None]:
    """Reset CORE after each test."""
    yield
    CORE.reset()


@pytest.fixture
def set_core_config() -> Generator[SetCoreConfigCallable]:
    """Fixture to set up the core configuration for tests."""

    def setter(
        platform_framework: PlatformFramework,
        /,
        *,
        core_data: ConfigType | None = None,
        platform_data: ConfigType | None = None,
        full_config: dict[str, ConfigType] | None = None,
    ) -> None:
        platform, framework = platform_framework.value

        CORE.data[KEY_CORE] = {
            KEY_TARGET_PLATFORM: platform.value,
            KEY_TARGET_FRAMEWORK: framework.value,
        }

        if core_data:
            CORE.data[KEY_CORE].update(core_data)

        if platform_data:
            CORE.data[platform.value] = platform_data

        config.path_context.set([])
        final_validate.full_config.set(full_config or Config())

    yield setter


@pytest.fixture
def set_component_config() -> Callable[[str, Any], None]:
    """Set a component configuration in the mock full config.

    Must be used after the core configuration has been set up.
    """

    def setter(name: str, value: Any) -> None:
        final_validate.full_config.get()[name] = value

    return setter

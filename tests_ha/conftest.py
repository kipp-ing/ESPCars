"""Fixtures for the Home Assistant glue tests of sdlog_relay."""

from __future__ import annotations

from pathlib import Path
import sys

import pytest

REPO = Path(__file__).parent.parent
# HA imports custom integrations as the top-level package ``custom_components``.
sys.path.insert(0, str(REPO))

pytest_plugins = "pytest_homeassistant_custom_component"


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations):
    yield

#!/usr/bin/env python3
"""The Balboa Spa Easyconf integration under a real Home Assistant, against the bridge simulator.

Shows that the integration works with the bridge unchanged: the config flow
finds the spa on the bridge's port, the entry is keyed by the MAC the bridge
reports (set here to a stock module's, as when swapping one for the other),
and the light and thermostat set from Home Assistant reach the simulated main
board through the bridge's own bus code.

Usage:
  make sim
  <venv with homeassistant and pybalboa>/bin/python tools/ha_smoke_against_sim.py [path to HA-Balboa-Spa-Easyconf]

Ends with os._exit: Python 3.14 crashes on shutdown after Home Assistant's C
extensions are loaded, once the work is done.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
INTEGRATION = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT.parent / "HA-Balboa-Spa-Easyconf").resolve()
SIM = ROOT / "build" / "host" / "bridge_sim"
DOMAIN = "balboa_easyconf"
MAC = "00:15:27:12:34:56"

failures: list[str] = []


def check(condition: bool, what: str) -> None:
    print(("ok    " if condition else "FAIL  ") + what, flush=True)
    if not condition:
        failures.append(what)


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Sim:
    def __init__(self) -> None:
        self.proc = subprocess.Popen(
            [str(SIM), "--mode", "assigned", "--mac", MAC],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1,
        )
        ready = self.proc.stdout.readline().split()
        self.port = int(ready[1])

    def stats(self) -> dict:
        self.proc.stdin.write("stats\n")
        self.proc.stdin.flush()
        return json.loads(self.proc.stdout.readline())

    def close(self) -> None:
        try:
            self.proc.stdin.write("quit\n")
            self.proc.stdin.flush()
            self.proc.wait(5)
        except Exception:  # noqa: BLE001
            self.proc.kill()


async def until(condition, timeout: float = 8.0) -> bool:
    loop = asyncio.get_running_loop()
    end = loop.time() + timeout
    while loop.time() < end:
        if condition():
            return True
        await asyncio.sleep(0.1)
    return condition()


async def main() -> int:
    sys.path.insert(0, str(INTEGRATION))
    from homeassistant import auth as ha_auth, bootstrap, config_entries, loader
    from homeassistant.core import HomeAssistant
    from homeassistant.helpers import entity_registry as er
    from homeassistant.setup import async_setup_component

    logging.basicConfig(level=logging.WARNING)
    sim = Sim()
    await asyncio.sleep(1.0)
    check(sim.stats()["bus"]["state"] == "online", "the bridge has a channel on the simulated bus")

    with tempfile.TemporaryDirectory() as config_dir:
        os.makedirs(f"{config_dir}/custom_components")
        os.symlink(INTEGRATION / "custom_components" / DOMAIN, f"{config_dir}/custom_components/{DOMAIN}")
        hass = HomeAssistant(config_dir)
        await hass.config.async_set_time_zone("Europe/Stockholm")
        loader.async_setup(hass)
        hass.config_entries = config_entries.ConfigEntries(hass, {})
        await hass.config_entries.async_initialize()
        assert await bootstrap.async_load_base_functionality(hass)
        hass.auth = await ha_auth.auth_manager_from_config(hass, [{"type": "homeassistant"}], [])
        assert await async_setup_component(hass, "homeassistant", {})
        assert await async_setup_component(hass, "http", {"http": {"server_port": free_port()}})

        async def no_discovery():
            return []

        flow = hass.config_entries.flow
        with patch(f"custom_components.{DOMAIN}.config_flow.discover", no_discovery):
            result = await flow.async_init(DOMAIN, context={"source": "user"})
            result = await flow.async_configure(result["flow_id"], {"host": f"127.0.0.1:{sim.port}"})
        check(result.get("step_id") == "powers", f"the bridge answers the config flow ({result.get('step_id')})")
        values = {
            str(k): (k.default() if callable(getattr(k, "default", None)) else None)
            for k in result["data_schema"].schema
        }
        answers = {k: v for k, v in values.items() if v is not None}
        answers.update({"use_learned": False, "sync_time": True})
        answers.pop("reference_sensor", None)
        result = await flow.async_configure(result["flow_id"], answers)
        check(result["type"] == "create_entry", f"entry created ({result.get('title')})")
        entry = result["result"]
        await hass.async_block_till_done()
        check(entry.title == "BP2100G1", f"titled by the model the board reports ({entry.title})")
        check(entry.unique_id == MAC, f"keyed by the MAC the bridge reports ({entry.unique_id})")
        check(entry.state is config_entries.ConfigEntryState.LOADED, f"entry loaded ({entry.state})")

        registry = er.async_get(hass)
        ours = [e for e in registry.entities.values() if e.platform == DOMAIN and not e.disabled]
        light = next(e.entity_id for e in ours if e.domain == "light")
        climate = next(e.entity_id for e in ours if e.domain == "climate")
        await until(lambda: hass.states.get(climate).attributes.get("current_temperature") == 12.0)
        check(
            hass.states.get(climate).attributes.get("current_temperature") == 12.0,
            "water temperature from the board's status, through the bridge",
        )

        await hass.services.async_call("light", "turn_on", {"entity_id": light}, blocking=True)
        check(await until(lambda: hass.states.get(light).state == "on"), "light on in Home Assistant")
        check(sim.stats()["board"]["light"] is True, "and on at the main board")

        await hass.services.async_call(
            "climate", "set_temperature", {"entity_id": climate, "temperature": 21}, blocking=True
        )
        check(
            await until(lambda: hass.states.get(climate).attributes.get("temperature") == 21),
            "target 21 °C in Home Assistant",
        )
        check(sim.stats()["board"]["target"] == 21.0, "and at the main board")

        board = sim.stats()["board"]
        check(board["unsolicited"] == 0 and board["wrong_channel"] == 0, "the bridge only wrote when it was its turn")

        assert await hass.config_entries.async_unload(entry.entry_id)
        await hass.async_block_till_done()
        await hass.async_stop()
    sim.close()

    print(f"\n{'FAILED: ' + str(len(failures)) if failures else 'all ok'}")
    return 1 if failures else 0


if __name__ == "__main__":
    rc = asyncio.run(main())
    logging.shutdown()
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(rc)

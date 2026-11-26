"""The bridge simulator against pybalboa, the library the Balboa integrations use.

`make pytest PYTHON=<python with pybalboa and pytest>`. Each test starts
build/host/bridge_sim (the firmware's own bus, hub and network code, against a
simulated BP2100G1 main board) and talks to it the way Home Assistant does.

Every test also checks the board's policing counters: the bridge must never
write outside the window right after a CTS (or New Client CTS, or Existing
Client Request) meant for it. On a real bus such a write is a collision.
"""

from __future__ import annotations

import asyncio
import json
import os
import socket
import subprocess
import time
from pathlib import Path

import pytest
from pybalboa import SpaClient

SIM = Path(__file__).resolve().parent.parent / "build" / "host" / "bridge_sim"
MAC = "00:15:27:aa:bb:cc"


class Sim:
    def __init__(self, *args: str) -> None:
        if not SIM.exists():
            pytest.skip("build the simulator first: make sim")
        self.proc = subprocess.Popen(
            [str(SIM), "--mac", MAC, *args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        line = self.proc.stdout.readline().split()
        assert line and line[0] == "READY", line
        self.tcp, self.udp = int(line[1]), int(line[2])

    def command(self, text: str) -> str:
        self.proc.stdin.write(text + "\n")
        self.proc.stdin.flush()
        return self.proc.stdout.readline().strip()

    def stats(self) -> dict:
        return json.loads(self.command("stats"))

    def wait_for(self, predicate, timeout: float = 8.0) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            stats = self.stats()
            if predicate(stats):
                return stats
            if time.monotonic() > deadline:
                raise AssertionError(f"timed out; last stats: {stats}")
            time.sleep(0.1)

    def close(self) -> None:
        if self.proc.poll() is None:
            try:
                self.command("quit")
            except (BrokenPipeError, OSError):
                pass
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def assert_no_collisions(self) -> None:
        board = self.stats()["board"]
        assert board["unsolicited"] == 0, board
        assert board["wrong_channel"] == 0, board


@pytest.fixture
def make_sim():
    sims: list[Sim] = []

    def make(*args: str) -> Sim:
        sim = Sim(*args)
        sims.append(sim)
        return sim

    yield make
    for sim in sims:
        sim.close()


async def until(predicate, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() > deadline:
            raise AssertionError("condition not met in time")
        await asyncio.sleep(0.05)


async def loaded(port: int) -> SpaClient:
    client = SpaClient("127.0.0.1", port)
    assert await client.connect()
    assert await client.async_configuration_loaded(10), "configuration never loaded"
    return client


def run(coro):
    return asyncio.run(coro)


def test_assigned_channel_full_control(make_sim):
    sim = make_sim("--mode", "assigned")
    sim.wait_for(lambda s: s["bus"]["state"] == "online")

    async def scenario():
        client = await loaded(sim.tcp)
        try:
            assert client.mac_address == MAC
            assert client.model == "BP2100G1"
            assert client.temperature == 12.0
            assert len(client.pumps) == 2

            await client.pumps[0].set_state(2)  # off -> high: two toggles
            await until(lambda: client.pumps[0].state == 2)
            await client.lights[0].set_state(1)
            await until(lambda: client.lights[0].state == 1)
            await client.set_temperature(20)
            await until(lambda: client.target_temperature == 20)
        finally:
            await client.disconnect()

    run(scenario())
    stats = sim.stats()
    assert stats["board"]["pumps"][0] == 2
    assert stats["board"]["light"] is True
    assert stats["board"]["target"] == 20.0
    assert stats["board"]["toggles"] == 3
    assert stats["bus"]["channel"] == 0x10
    assert stats["hub"]["module_answers"] >= 1
    sim.assert_no_collisions()


def test_several_clients_at_once(make_sim):
    sim = make_sim("--mode", "assigned")
    sim.wait_for(lambda s: s["bus"]["state"] == "online")

    async def scenario():
        a = await loaded(sim.tcp)
        b = await loaded(sim.tcp)
        try:
            await a.lights[0].set_state(1)
            await until(lambda: b.lights[0].state == 1)
            await b.pumps[1].set_state(1)
            await until(lambda: a.pumps[1].state == 1)
        finally:
            await a.disconnect()
            await b.disconnect()

    run(scenario())
    assert sim.stats()["net"]["refused"] == 0
    sim.assert_no_collisions()


def test_beside_the_stock_module(make_sim):
    sim = make_sim("--mode", "assigned", "--stock-module")
    stats = sim.wait_for(lambda s: s["bus"]["state"] == "online")
    assert 0x0A in stats["board"]["polled"]  # the stock module keeps its channel
    assert stats["bus"]["channel"] == 0x10

    async def scenario():
        client = await loaded(sim.tcp)
        try:
            await client.pumps[1].set_state(1)
            await until(lambda: client.pumps[1].state == 1)
        finally:
            await client.disconnect()

    run(scenario())
    sim.assert_no_collisions()


def test_listen_only_reads_but_never_writes(make_sim):
    # The stock module asks for the spa's settings now and then; the bridge
    # hears the answers and serves them to its own clients without a word on
    # the bus.
    sim = make_sim("--mode", "listen_only", "--stock-module")
    sim.wait_for(lambda s: s["hub"]["cached"] & 0b1111 == 0b1111)

    async def scenario():
        client = await loaded(sim.tcp)
        try:
            assert client.model == "BP2100G1"
            assert client.temperature == 12.0
            await client.lights[0].set_state(1)
            await asyncio.sleep(1.0)
            assert client.lights[0].state == 0
        finally:
            await client.disconnect()

    run(scenario())
    stats = sim.stats()
    assert stats["board"]["frames_from_bridge"] == 0
    assert stats["board"]["light"] is False
    assert stats["hub"]["cache_answers"] >= 4
    assert stats["hub"]["dropped_listen"] >= 1
    assert stats["bus"]["state"] == "listening"


def test_fixed_wifi_takes_over_an_unanswered_0x0a(make_sim):
    sim = make_sim("--mode", "fixed_wifi", "--wifi-registered")
    stats = sim.wait_for(lambda s: s["bus"]["state"] == "online")
    assert stats["bus"]["channel"] == 0x0A

    async def scenario():
        client = await loaded(sim.tcp)
        try:
            await client.set_temperature(18)
            await until(lambda: client.target_temperature == 18)
        finally:
            await client.disconnect()

    run(scenario())
    sim.assert_no_collisions()


def test_fixed_wifi_backs_off_when_the_stock_module_answers(make_sim):
    sim = make_sim("--mode", "fixed_wifi", "--stock-module")
    stats = sim.wait_for(lambda s: s["bus"]["state"] == "gave_up")
    time.sleep(0.5)
    stats = sim.stats()
    assert stats["board"]["frames_from_bridge"] == 0


def test_board_power_cycle_and_bridge_reboot_keep_the_channel(make_sim):
    sim = make_sim("--mode", "assigned")
    sim.wait_for(lambda s: s["bus"]["state"] == "online")

    # The board restarts and asks every channel whether anyone is there.
    assert sim.command("powercycle") == "OK"
    stats = sim.wait_for(lambda s: s["board"]["existing_answers"] >= 1 and 0x10 in s["board"]["polled"])
    time.sleep(1.0)
    stats = sim.stats()
    assert stats["bus"]["state"] == "online" and stats["bus"]["channel"] == 0x10

    # The bridge restarts (software reset) and takes its old channel back.
    assert sim.command("reboot") == "OK"
    stats = sim.wait_for(lambda s: s["bus"]["state"] == "online")
    assert stats["bus"]["channel"] == 0x10
    assert stats["board"]["channel_requests"] == 1  # never asked for a second one

    async def scenario():
        client = await loaded(sim.tcp)
        try:
            await client.lights[0].set_state(1)
            await until(lambda: client.lights[0].state == 1)
        finally:
            await client.disconnect()

    run(scenario())
    sim.assert_no_collisions()


def test_transceiver_echo_is_ignored(make_sim):
    sim = make_sim("--mode", "assigned", "--echo")
    sim.wait_for(lambda s: s["bus"]["state"] == "online")

    async def scenario():
        client = await loaded(sim.tcp)
        try:
            await client.pumps[0].set_state(1)
            await until(lambda: client.pumps[0].state == 1)
        finally:
            await client.disconnect()

    run(scenario())
    stats = sim.stats()
    assert stats["bus"]["echoes"] > 10
    assert stats["board"]["toggles"] == 1
    sim.assert_no_collisions()


def test_udp_discovery_answer(make_sim):
    sim = make_sim("--mode", "listen_only")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(2)
        s.sendto(b"Discovery", ("127.0.0.1", sim.udp))
        data, _ = s.recvfrom(128)
    # The way pybalboa's discovery reads it.
    assert b"BWGS" in data.upper()
    hostname, mac = map(str.strip, data.decode().splitlines()[:2])
    assert hostname == "BWGSPA"
    assert mac == "00-15-27-AA-BB-CC"


def test_slow_client_does_not_stall_others(make_sim):
    sim = make_sim("--mode", "assigned", "--status-interval", "0")
    sim.wait_for(lambda s: s["bus"]["state"] == "online")
    # A client that connects and never reads.
    lazy = socket.create_connection(("127.0.0.1", sim.tcp))
    lazy.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
    try:

        async def scenario():
            client = await loaded(sim.tcp)
            try:
                await client.lights[0].set_state(1)
                await until(lambda: client.lights[0].state == 1)
            finally:
                await client.disconnect()

        run(scenario())
    finally:
        lazy.close()
    sim.assert_no_collisions()

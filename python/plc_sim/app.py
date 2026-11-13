"""The plc_sim process: Modbus-TCP server + simulation task + heartbeat.

Register map (addresses and scaling come from the ``registers`` config block):

  input register   ir_speed_actual    actual belt speed, m/s * speed_scale
  input register   ir_fault_code      0 = healthy, 1 = setpoint above max_speed
  discrete input   di_item_present    photo-eye
  holding register hr_speed_setpoint  commanded speed, m/s * speed_scale
  holding register hr_sim_outage_ms   fault injection, see below

Fault injection: writing N != 0 to hr_sim_outage_ms makes the PLC "drop off
the network". The write is acknowledged first; on the next simulation tick the
Modbus server is shut down - the listening socket is closed and every client
connection is dropped - for N ms (65535 = until the process is restarted).
Then it listens again and the register reads 0. The belt simulation and the
heartbeat keep running throughout; the heartbeat reports DEGRADED.
"""
from __future__ import annotations

import asyncio
import logging
import os
import time
from typing import Any

import zmq
from pymodbus.datastore import (
    ModbusSequentialDataBlock,
    ModbusServerContext,
    ModbusSlaveContext,
)
from pymodbus.server import ModbusTcpServer

from cell_proto import cell_pb2

from . import config as cfg
from .model import ConveyorModel, ConveyorParams

log = logging.getLogger("plc_sim")

SOURCE = "plc_sim"
HEARTBEAT_TOPIC = b"hb"
OUTAGE_UNTIL_RESTART = 65535
OUTAGE_DETAIL = "simulated network outage (fault injection)"

# pymodbus function codes used to address the datastore tables.
_FC_DISCRETE_INPUTS = 2
_FC_HOLDING_REGISTERS = 3
_FC_INPUT_REGISTERS = 4


def _address(config: dict[str, Any], name: str) -> int:
    value = cfg.get(config, f"registers.{name}", int)
    if not 0 <= value <= 65535:
        raise cfg.ConfigError(f"registers.{name} must be a Modbus address (0..65535)")
    return value


def _number(config: dict[str, Any], key: str, default: Any = cfg._MISSING) -> float:
    return float(cfg.get(config, key, (int, float), default))


class PlcSim:
    def __init__(self, config: dict[str, Any]) -> None:
        self.host = cfg.get(config, "host", str)
        self.port = cfg.get(config, "port", int)
        self.unit_id = cfg.get(config, "unit_id", int)
        self.heartbeat_endpoint = cfg.get(config, "heartbeat_endpoint", str)
        self.heartbeat_interval_s = _number(config, "heartbeat_interval_ms") / 1000.0
        self.tick_s = 1.0 / _number(config, "tick_hz")
        if self.heartbeat_interval_s <= 0 or self.tick_s <= 0:
            raise cfg.ConfigError("heartbeat_interval_ms and tick_hz must be positive")

        initial_speed = _number(config, "initial_speed")
        params = ConveyorParams(
            initial_speed=initial_speed,
            max_speed=_number(config, "max_speed"),
            ramp_per_s=_number(config, "ramp_per_s"),
            item_period_s=_number(config, "item_period_s"),
            item_duty=_number(config, "item_duty"),
            nominal_speed=_number(config, "nominal_speed", initial_speed),
        )
        try:
            self.model = ConveyorModel(params)
        except ValueError as exc:
            raise cfg.ConfigError(str(exc)) from exc

        self.speed_scale = _number(config, "registers.speed_scale")
        if self.speed_scale <= 0 or params.max_speed * self.speed_scale > 65535:
            raise cfg.ConfigError(
                "registers.speed_scale must be positive and max_speed must fit a 16-bit register")
        self.ir_speed_actual = _address(config, "ir_speed_actual")
        self.ir_fault_code = _address(config, "ir_fault_code")
        self.di_item_present = _address(config, "di_item_present")
        self.hr_speed_setpoint = _address(config, "hr_speed_setpoint")
        self.hr_sim_outage_ms = _address(config, "hr_sim_outage_ms")
        if self.hr_speed_setpoint == self.hr_sim_outage_ms:
            raise cfg.ConfigError("hr_speed_setpoint and hr_sim_outage_ms must differ")

        # ModbusSlaveContext maps protocol address A to block address A + 1,
        # so each block starts at 0 and is one longer than the highest address.
        def block(*addresses: int) -> ModbusSequentialDataBlock:
            return ModbusSequentialDataBlock(0, [0] * (max(addresses) + 2))

        self.slave = ModbusSlaveContext(
            di=block(self.di_item_present),
            co=block(0),
            hr=block(self.hr_speed_setpoint, self.hr_sim_outage_ms),
            ir=block(self.ir_speed_actual, self.ir_fault_code),
        )
        # single=False: only the configured unit id answers, like a real PLC.
        self.context = ModbusServerContext(slaves={self.unit_id: self.slave}, single=False)
        self._set_holding(self.hr_speed_setpoint, round(initial_speed * self.speed_scale))
        self._publish_inputs()

        self.server: ModbusTcpServer | None = None
        self.outage_until: float | None = None  # loop time; inf = until restart
        self._zmq_ctx: zmq.Context | None = None
        self._pub: zmq.Socket | None = None
        self._hb_seq = 0
        self._start_ns = time.monotonic_ns()

    # ---- datastore access ---------------------------------------------------

    def _holding(self, address: int) -> int:
        return int(self.slave.getValues(_FC_HOLDING_REGISTERS, address, 1)[0])

    def _set_holding(self, address: int, value: int) -> None:
        self.slave.setValues(_FC_HOLDING_REGISTERS, address, [value])

    def _publish_inputs(self) -> None:
        speed = min(65535, max(0, round(self.model.speed * self.speed_scale)))
        self.slave.setValues(_FC_INPUT_REGISTERS, self.ir_speed_actual, [speed])
        self.slave.setValues(_FC_INPUT_REGISTERS, self.ir_fault_code, [self.model.fault_code])
        self.slave.setValues(_FC_DISCRETE_INPUTS, self.di_item_present,
                             [self.model.item_present])

    # ---- Modbus server ------------------------------------------------------

    @property
    def in_outage(self) -> bool:
        return self.outage_until is not None

    async def _start_server(self) -> None:
        server = ModbusTcpServer(self.context, address=(self.host, self.port))
        await server.serve_forever(background=True)
        if server.transport is None:  # pymodbus logs the OSError and carries on
            raise OSError(f"cannot listen on {self.host}:{self.port}")
        self.server = server

    async def _stop_server(self) -> None:
        """Close the listening socket and drop every client connection."""
        if self.server is not None:
            await self.server.shutdown()
            self.server = None

    async def start(self) -> None:
        self._open_heartbeat_socket()
        await self._start_server()
        log.info("plc_sim running: Modbus-TCP on %s:%d (unit %d), heartbeat on %s", self.host,
                 self.port, self.unit_id, self.heartbeat_endpoint)

    async def close(self) -> None:
        await self._stop_server()
        if self._pub is not None:
            self._send_heartbeat(cell_pb2.PROCESS_STOPPING, "")
            self._pub.close(linger=0)
            self._pub = None
        if self._zmq_ctx is not None:
            self._zmq_ctx.term()
            self._zmq_ctx = None

    # ---- simulation ---------------------------------------------------------

    async def _begin_outage(self, duration_ms: int) -> None:
        loop = asyncio.get_running_loop()
        if duration_ms >= OUTAGE_UNTIL_RESTART:
            self.outage_until = float("inf")
            log.warning("fault injection: dropping off the network until restart")
        else:
            self.outage_until = loop.time() + duration_ms / 1000.0
            log.warning("fault injection: dropping off the network for %d ms", duration_ms)
        await self._stop_server()

    async def _end_outage(self) -> None:
        self._set_holding(self.hr_sim_outage_ms, 0)
        await self._start_server()
        self.outage_until = None
        log.info("fault injection over: listening on %s:%d again", self.host, self.port)

    async def tick(self, dt: float) -> None:
        """One simulation step: read commands, advance the belt, publish inputs."""
        self.model.setpoint = self._holding(self.hr_speed_setpoint) / self.speed_scale
        self.model.step(dt)
        self._publish_inputs()

        if not self.in_outage:
            outage_ms = self._holding(self.hr_sim_outage_ms)
            if outage_ms != 0:
                await self._begin_outage(outage_ms)
        elif asyncio.get_running_loop().time() >= self.outage_until:
            await self._end_outage()

    async def sim_loop(self) -> None:
        loop = asyncio.get_running_loop()
        deadline = loop.time() + self.tick_s
        while True:
            await asyncio.sleep(max(0.0, deadline - loop.time()))
            await self.tick(self.tick_s)
            deadline += self.tick_s
            if loop.time() > deadline + self.tick_s:  # fell behind: resynchronise
                deadline = loop.time() + self.tick_s

    # ---- heartbeat ----------------------------------------------------------

    def _open_heartbeat_socket(self) -> None:
        prefix = "ipc://"
        if self.heartbeat_endpoint.startswith(prefix):
            directory = os.path.dirname(self.heartbeat_endpoint[len(prefix):])
            if directory:
                os.makedirs(directory, exist_ok=True)
        self._zmq_ctx = zmq.Context()
        self._pub = self._zmq_ctx.socket(zmq.PUB)
        self._pub.setsockopt(zmq.LINGER, 0)
        self._pub.bind(self.heartbeat_endpoint)

    def _send_heartbeat(self, status: int, detail: str) -> None:
        now_ns = time.monotonic_ns()  # CLOCK_MONOTONIC, the clock the C++ side uses
        hb = cell_pb2.Heartbeat()
        hb.h.seq = self._hb_seq
        hb.h.send_mono_ns = now_ns
        hb.h.source = SOURCE
        hb.status = status
        hb.uptime_ms = (now_ns - self._start_ns) // 1_000_000
        hb.detail = detail
        try:
            self._pub.send_multipart([HEARTBEAT_TOPIC, hb.SerializeToString()], zmq.NOBLOCK)
        except zmq.Again:
            return  # not sent, so the sequence number is not consumed
        self._hb_seq += 1

    async def heartbeat_loop(self) -> None:
        loop = asyncio.get_running_loop()
        deadline = loop.time()
        while True:
            if self.in_outage:
                self._send_heartbeat(cell_pb2.PROCESS_DEGRADED, OUTAGE_DETAIL)
            else:
                self._send_heartbeat(cell_pb2.PROCESS_OK, "")
            deadline = max(deadline + self.heartbeat_interval_s, loop.time())
            await asyncio.sleep(max(0.0, deadline - loop.time()))


async def run(config: dict[str, Any], stop: asyncio.Event) -> int:
    """Run until `stop` is set. Returns the process exit code."""
    sim = PlcSim(config)
    try:
        await sim.start()
    except (OSError, zmq.ZMQError) as exc:
        log.critical("startup failed: %s", exc)
        await sim.close()
        return 1

    tasks = [
        asyncio.create_task(sim.sim_loop(), name="sim"),
        asyncio.create_task(sim.heartbeat_loop(), name="heartbeat"),
    ]
    stop_task = asyncio.create_task(stop.wait(), name="stop")
    done, _ = await asyncio.wait([stop_task, *tasks], return_when=asyncio.FIRST_COMPLETED)

    exit_code = 0
    for task in done:
        if task is not stop_task and task.exception() is not None:
            log.critical("task '%s' failed: %r", task.get_name(), task.exception())
            exit_code = 1
    for task in (stop_task, *tasks):
        task.cancel()
    await asyncio.gather(stop_task, *tasks, return_exceptions=True)
    await sim.close()
    return exit_code

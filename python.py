#!/usr/bin/env python3
"""
================================================================================
  traffic_generator.py
  Data-Driven Digital Twin — LoRa-Enabled Metering & Predictive Usage
  Notifications for Rural Microgrids
================================================================================

  Project : Rural IoT Smart Metering (LoRa / ESP32 Edge Gateway)
  Role    : Runs on a laptop. Ingests a Kaggle CSV load dataset, computes
            real-time energy consumption for 10 virtual households (IDs 101-110),
            and streams telemetry packets over USB Serial to an Arduino.
            The Arduino re-broadcasts each packet over LoRa to the remote
            ESP32 edge gateway, which handles all cloud/MQTT communication.

  Author  : Senior Data Engineer / Embedded Systems Architect
  Target  : Python 3.8+  |  pyserial

  ──────────────────────────────────────────────────────────────────────────────
  ARCHITECTURE OVERVIEW
  ──────────────────────────────────────────────────────────────────────────────

  ┌─────────────────────┐   USB Serial    ┌───────────┐   LoRa RF   ┌──────────┐
  │  traffic_generator  │ ─────────────► │  Arduino  │ ──────────► │  ESP32   │
  │  (This script)      │  9600 baud      │  (relay)  │  915 MHz    │ Gateway  │
  └─────────────────────┘                └───────────┘             └──────────┘
        │
        ├─ Ingests load_data.csv  (Day, Hour, Base_Load_W)
        ├─ Maintains a virtual 30-day billing clock
        ├─ Simulates 10 households with per-node noise multipliers
        ├─ Euler-integrates cumulative kWh
        └─ Emits TDMA-staggered, checksummed packets

  ──────────────────────────────────────────────────────────────────────────────
  STRICT CONSTRAINTS (intentionally absent from this file)
  ──────────────────────────────────────────────────────────────────────────────
    ✗  No solar/battery/SoC/thermal math
    ✗  No hardware fault injection or keyboard listeners
    ✗  No Firebase, HTTP, or any cloud/web code
    ✗  No central-plant telemetry (all bandwidth reserved for households)
================================================================================
"""

import csv
import time
import random
import serial          # pip install pyserial
import serial.tools.list_ports
import logging
import sys
from pathlib import Path

# ──────────────────────────────────────────────────────────────────────────────
#  LOGGING SETUP
#  Uses Python's built-in logging so every print is timestamped and
#  can be redirected to a file without code changes.
# ──────────────────────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  [%(levelname)s]  %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler("digital_twin.log", mode="a"),
    ],
)
log = logging.getLogger("DigitalTwin")

# ══════════════════════════════════════════════════════════════════════════════
#  CONFIGURATION  —  edit these constants to change behaviour without touching
#                    any logic below.
# ══════════════════════════════════════════════════════════════════════════════

# ── CSV dataset ───────────────────────────────────────────────────────────────
CSV_PATH = Path("load_data.csv")          # Must contain: Day, Hour, Base_Load_W

# ── Serial port ───────────────────────────────────────────────────────────────
SERIAL_PORT     = "COM5"                  # Change to "/dev/ttyUSB0" on Linux/Mac
SERIAL_BAUDRATE = 9600                    # Must match the Arduino sketch
SERIAL_TIMEOUT  = 1                       # seconds — write timeout

# ── Time scaling ─────────────────────────────────────────────────────────────
#
#   TIME_ACCELERATOR = 60 means:
#       1 real second  →  60 virtual minutes  →  1 virtual hour
#
#   So the main loop sleeps for (1 / TIME_ACCELERATOR) real seconds per
#   virtual-minute tick.  With the default value of 60, the loop fires
#   once per real second and each iteration advances the virtual clock
#   by exactly 60 virtual minutes (= 1 virtual hour).
#
#   You can tune this freely:
#       TIME_ACCELERATOR =  1  →  real-time (1 s = 1 virtual minute)
#       TIME_ACCELERATOR = 60  →  1 s = 1 virtual hour  (DEFAULT)
#       TIME_ACCELERATOR = 1440 →  1 s = 1 virtual day
#
TIME_ACCELERATOR = 20                     # virtual-minutes per real second so 1 virtual hr takes 3 real seconds

# ── Billing cycle ─────────────────────────────────────────────────────────────
BILLING_CYCLE_DAYS = 30                   # reset kWh counters after this many
                                          # virtual days

# ── Household node IDs ────────────────────────────────────────────────────────
NODE_IDS = list(range(101, 111))          # [101, 102, … 110]

# ── Per-node static noise seed ───────────────────────────────────────────────
#   We seed the RNG once at startup so every run produces the same
#   noise profile for a given node — reproducible "field diversity".
NOISE_SEED = 42

# ── TDMA inter-packet gap (real milliseconds) ────────────────────────────────
#
#   With 10 nodes and a 100 ms gap the total transmission window per
#   virtual hour = 10 × 100 ms = 1 000 ms = 1 real second.
#   This comfortably fits inside the 1-second real-time loop tick
#   (TIME_ACCELERATOR = 60 → 1 tick = 1 real second).
#
#   Adjust if you increase TIME_ACCELERATOR or add more nodes.
#
TDMA_GAP_MS = 250                         # milliseconds between node writes

# ── Transmission window: how often a node may transmit ───────────────────────
#   CRITICAL RULE: each node transmits at most once per virtual hour.
TRANSMIT_INTERVAL_VIRTUAL_MINUTES = 60   # do not change unless you know why


# ══════════════════════════════════════════════════════════════════════════════
#  SECTION 1 — DATA INGESTION
#  load_csv() reads the entire dataset into RAM once at startup.
#  No Pandas dependency; plain lists/dicts keep the main loop latency-free.
# ══════════════════════════════════════════════════════════════════════════════

def load_csv(path: Path) -> dict:
    """
    Load load_data.csv into a nested dict keyed by (day, hour) for O(1) lookup.

    Expected CSV columns (case-insensitive strip):
        Day           — integer 1..30
        Hour          — integer 0..23
        Base_Load_W   — float, watts

    Returns:
        load_table : dict  { (day: int, hour: int) : base_load_w: float }

    Raises:
        FileNotFoundError  — if the CSV file does not exist
        ValueError         — if required columns are absent
    """
    if not path.exists():
        raise FileNotFoundError(
            f"Dataset not found: {path.resolve()}\n"
            "Please download load_data.csv and place it beside this script."
        )

    load_table: dict = {}
    required_cols = {"day", "hour", "base_load_w"}

    with open(path, newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)

        # ── Normalise header names (strip whitespace, lower-case) ─────────────
        if reader.fieldnames is None:
            raise ValueError("CSV appears to be empty.")

        normalised = [col.strip().lower() for col in reader.fieldnames]
        missing = required_cols - set(normalised)
        if missing:
            raise ValueError(
                f"CSV is missing required columns: {missing}\n"
                f"Found: {reader.fieldnames}"
            )

        # Map original header → normalised header for robust lookup
        col_map = {orig: norm
                   for orig, norm in zip(reader.fieldnames, normalised)}

        rows_loaded = 0
        for raw_row in reader:
            row = {col_map[k]: v.strip() for k, v in raw_row.items() if k}
            try:
                day  = int(row["day"])
                hour = int(row["hour"])
                watt = float(row["base_load_w"])
                load_table[(day, hour)] = watt
                rows_loaded += 1
            except (ValueError, KeyError):
                # Skip malformed rows during ingestion; they will surface
                # as ERR packets at runtime via the try-except in the main loop.
                log.warning("Skipping malformed CSV row: %s", raw_row)

    log.info("CSV loaded: %d rows ingested from '%s'.", rows_loaded, path)
    return load_table


# ══════════════════════════════════════════════════════════════════════════════
#  SECTION 2 — PER-NODE NOISE MULTIPLIER
#  Generate one static multiplier per node (seeded RNG → reproducible).
#  Range: [0.90 … 1.10]  (±10 %)
# ══════════════════════════════════════════════════════════════════════════════

def build_noise_multipliers(node_ids: list, seed: int) -> dict:
    """
    Return a dict { node_id: multiplier } with values in [0.90, 1.10].

    The multiplier is STATIC — it is chosen once at startup and held
    constant so each house has a consistent "personality" across the
    entire billing cycle.  This avoids unrealistic second-by-second
    load jumps that would confuse the predictive model at the gateway.
    """
    rng = random.Random(seed)
    return {nid: rng.uniform(0.90, 1.10) for nid in node_ids}


# ══════════════════════════════════════════════════════════════════════════════
#  SECTION 3 — PACKET BUILDER & CHECKSUM
# ══════════════════════════════════════════════════════════════════════════════

def compute_checksum(fields: list) -> int:
    """
    Lightweight modulo-100 checksum.

    Algorithm:
        1. Concatenate all field strings (excluding the checksum field itself).
        2. Sum the ASCII/UTF-8 byte values.
        3. Return  sum % 100  (always a two-digit integer 00..99).

    The Arduino side replicates this calculation to validate every packet
    before forwarding it over LoRa.  A mismatch → the packet is dropped.

    Example:
        fields = ["101", "2345.6", "12.34"]
        checksum = sum(ord(c) for c in "1012345.612.34") % 100
    """
    raw = "".join(str(f) for f in fields)
    return sum(raw.encode("utf-8")) % 100


def build_packet(house_id: int, power_str: str, cumulative_kwh: float) -> bytes:
    """
    Assemble a single telemetry packet and append a CRLF terminator.

    Standard packet format:
        [HouseID]:[Power_W]:[Cumulative_kWh]:[Checksum]\r\n

    Fault packet format (burnt-out / missing sensor):
        [HouseID]:ERR:[Cumulative_kWh]:[Checksum]\r\n

    Field widths are intentionally compact to minimise LoRa air-time
    and avoid fragmenting the 51-byte maximum LoRaWAN payload.

    Parameters
    ----------
    house_id        : int   e.g. 101
    power_str       : str   e.g. "2345.6"  or  "ERR"
    cumulative_kwh  : float running total for this billing cycle

    Returns
    -------
    bytes — ready to pass to serial.write()
    """
    kwh_str    = f"{cumulative_kwh:.4f}"          # 4 decimal places = 0.1 Wh resolution
    fields     = [str(house_id), power_str, kwh_str]
    checksum   = compute_checksum(fields)
    packet_str = ":".join(fields) + f":{checksum:02d}\r\n"
    return packet_str.encode("utf-8")


# ══════════════════════════════════════════════════════════════════════════════
#  SECTION 4 — SERIAL PORT INITIALISATION
# ══════════════════════════════════════════════════════════════════════════════

def open_serial(port: str, baudrate: int, timeout: float):
    """
    Open the serial port with graceful error handling and port enumeration.

    Returns a serial.Serial object, or None if running in dry-run mode
    (no Arduino attached).  In dry-run mode packets are printed to stdout
    instead of being written to the wire — useful for unit testing the
    digital twin logic without hardware.
    """
    try:
        ser = serial.Serial(port, baudrate=baudrate, timeout=timeout)
        time.sleep(2)          # allow Arduino bootloader reset to complete
        log.info("Serial port opened: %s @ %d baud.", port, baudrate)
        return ser
    except serial.SerialException as exc:
        log.warning("Could not open serial port '%s': %s", port, exc)
        log.warning("┌─ Available ports ─────────────────────────────────")
        for p in serial.tools.list_ports.comports():
            log.warning("│  %s  —  %s", p.device, p.description)
        log.warning("└────────────────────────────────────────────────────")
        log.warning("Continuing in DRY-RUN mode (packets printed to stdout).")
        return None


def send_packet(ser, packet: bytes, dry_run_label: str = ""):
    """
    Write a packet to the serial port (or stdout in dry-run mode).

    Parameters
    ----------
    ser            : serial.Serial | None
    packet         : bytes
    dry_run_label  : str — label used when printing in dry-run mode
    """
    if ser is not None:
        try:
            ser.write(packet)
        except serial.SerialException as exc:
            log.error("Serial write failed: %s", exc)
    else:
        # Dry-run: decode and print without the trailing CRLF
        log.info("[DRY-RUN] %s→  %s", dry_run_label, packet.decode("utf-8").strip())


# ══════════════════════════════════════════════════════════════════════════════
#  SECTION 5 — VIRTUAL CLOCK HELPERS
# ══════════════════════════════════════════════════════════════════════════════

class VirtualClock:
    """
    Tracks virtual time in a 30-day billing cycle.

    ── Time-scaling mathematics ──────────────────────────────────────────────

    Let:
        A  = TIME_ACCELERATOR  (virtual minutes per real second)
        Δt_real  = real elapsed time between loop iterations (seconds)
        Δt_virt  = virtual time advanced per iteration  (minutes)

    Then:
        Δt_virt = A × Δt_real

    Virtual hours elapsed:
        Δh_virt = Δt_virt / 60

    This Δh_virt value is exactly what we pass to the Euler integrator
    as the integration time step:
        ΔE (kWh) = P (kW) × Δh_virt

    Example (A = 60, Δt_real = 1 s):
        Δt_virt = 60 × 1 = 60 virtual minutes = 1 virtual hour
        Δh_virt = 60 / 60 = 1.0 virtual hours

    So in this default configuration every real-second loop iteration
    represents exactly one virtual hour of household consumption.
    ──────────────────────────────────────────────────────────────────────────
    """

    def __init__(self, accelerator: int, billing_days: int):
        self.accelerator   = accelerator      # virtual minutes per real second
        self.billing_days  = billing_days

        # Virtual clock state
        self.virtual_day   = 1
        self.virtual_hour  = 0
        self.virtual_minute = 0

        # Track the real-world wall-clock time of the last tick
        self._last_real_ts = time.monotonic()

    def tick(self) -> float:
        """
        Advance the virtual clock by the time elapsed since the last tick.

        Returns
        -------
        delta_virtual_hours : float
            The virtual time step (in hours) to use in the Euler integrator.
            Typically 1.0 when TIME_ACCELERATOR = 60 and the loop runs at 1 Hz.
        """
        now           = time.monotonic()
        delta_real_s  = now - self._last_real_ts
        self._last_real_ts = now

        # Δt_virt (minutes) = A × Δt_real (seconds)
        delta_virt_min = self.accelerator * delta_real_s

        # Advance the virtual minute counter
        self.virtual_minute += delta_virt_min

        # Cascade minutes → hours → days
        while self.virtual_minute >= 60:
            self.virtual_minute -= 60
            self.virtual_hour   += 1

        while self.virtual_hour >= 24:
            self.virtual_hour -= 24
            self.virtual_day  += 1

        # Convert the elapsed virtual minutes to hours for Euler integration
        delta_virtual_hours = delta_virt_min / 60.0
        return delta_virtual_hours

    @property
    def billing_cycle_complete(self) -> bool:
        """True if we have just rolled over past Day 30 (or billing_days)."""
        return self.virtual_day > self.billing_days

    def reset_billing_cycle(self):
        """Roll over to Day 1, preserving the current hour within the day."""
        self.virtual_day    = 1
        self.virtual_hour   = 0
        self.virtual_minute = 0
        log.info(
            "═══ BILLING CYCLE COMPLETE — all kWh counters reset to 0.  "
            "Restarting at Day 1. ═══"
        )

    def due_for_transmission(self, last_tx_virtual_hour: int) -> bool:
        """
        Return True if this node is due for a transmission this iteration.

        A node must transmit at most once per TRANSMIT_INTERVAL_VIRTUAL_MINUTES
        virtual minutes (default: 60 = once per virtual hour).

        We compare the current virtual hour to the hour of the last transmission.
        A simple inequality is sufficient because the clock only ever moves
        forward and is reset cleanly at billing-cycle boundaries.
        """
        return self.virtual_hour != last_tx_virtual_hour

    def __str__(self):
        return (
            f"VDay {self.virtual_day:02d}  "
            f"VTime {self.virtual_hour:02d}:{int(self.virtual_minute):02d}"
        )


# ══════════════════════════════════════════════════════════════════════════════
#  SECTION 6 — HOUSEHOLD STATE
# ══════════════════════════════════════════════════════════════════════════════

class Household:
    """
    Represents a single virtual metered household.

    State maintained between loop iterations:
        cumulative_kwh    — Euler-integrated energy for the current billing cycle
        last_tx_vhour     — virtual hour of the last successful transmission
                            (used by the TDMA scheduler to enforce one-tx-per-hour)
    """

    def __init__(self, node_id: int, noise_mult: float):
        self.node_id         = node_id
        self.noise_mult      = noise_mult   # static ±10 % personality factor
        self.cumulative_kwh  = 0.0
        self.last_tx_vhour   = -1           # sentinel: never transmitted yet

    def compute_power(self, load_table: dict, virtual_day: int, virtual_hour: int) -> str:
        """
        Look up the base load for (day, hour) and apply the node noise multiplier.

        Uses a try-except block to handle:
            - Missing keys (day/hour not in dataset → sensor gap)
            - Non-numeric values (corrupted CSV entry → burnt-out sensor)

        Returns
        -------
        power_str : str
            Formatted watt value  e.g. "2341.8"
            or the fault sentinel "ERR"
        """
        try:
            # ── CSV lookup (O(1) dict access) ─────────────────────────────────
            #
            #   The dataset covers Day 1..30 and Hour 0..23.
            #   If virtual_day > number of CSV days, we wrap using modulo so
            #   the simulation loops over available data rather than crashing.
            #   (This also handles datasets shorter than 30 days gracefully.)
            wrapped_day = ((virtual_day - 1) % 30) + 1   # 1-indexed, mod-30 wrap

            raw_watts = load_table[(wrapped_day, virtual_hour+1)]

            # ── Validate ──────────────────────────────────────────────────────
            if not isinstance(raw_watts, (int, float)):
                raise ValueError(f"Non-numeric load value: {raw_watts!r}")

            # ── Apply static noise multiplier ─────────────────────────────────
            #   adjusted_watts = base_load × noise_mult
            #   Range example: base = 2000 W, mult = 1.07 → 2140 W
            adjusted_watts = raw_watts * self.noise_mult

            # Clamp to a physically plausible floor (avoids negative loads)
            adjusted_watts = max(0.0, adjusted_watts)

            return f"{adjusted_watts:.1f}"

        except KeyError:
            # Day/Hour combination absent from dataset → treat as sensor gap
            log.warning(
                "Node %d: no data for (day=%d, hour=%d) — emitting ERR packet.",
                self.node_id, virtual_day, virtual_hour,
            )
            return "ERR"

        except (ValueError, TypeError) as exc:
            # Corrupted value in CSV → burnt-out current sensor simulation
            log.warning(
                "Node %d: data validation failed (%s) — emitting ERR packet.",
                self.node_id, exc,
            )
            return "ERR"

    def euler_integrate(self, power_str: str, delta_hours: float):
        """
        Euler integration: accumulate energy consumed this time step.

            ΔE (kWh) = P (kW) × Δt (hours)

        If power_str is "ERR" we skip integration for this step
        (we cannot know the true power → do not add fictitious energy).
        Cumulative kWh therefore represents confirmed metered energy only.

        Parameters
        ----------
        power_str   : str   — e.g. "2341.8" or "ERR"
        delta_hours : float — virtual hours elapsed since last iteration
        """
        if power_str == "ERR":
            return   # cannot integrate unknown power

        try:
            power_w  = float(power_str)
            power_kw = power_w / 1000.0
            self.cumulative_kwh += power_kw * delta_hours
        except ValueError:
            # Should never reach here given compute_power's return contract,
            # but defensive programming is free.
            log.error("Node %d: float conversion failed for '%s'.", self.node_id, power_str)

    def reset_billing(self):
        """Zero the cumulative kWh counter at the start of a new billing cycle."""
        self.cumulative_kwh = 0.0
        self.last_tx_vhour  = -1


# ══════════════════════════════════════════════════════════════════════════════
#  SECTION 7 — MAIN LOOP
# ══════════════════════════════════════════════════════════════════════════════

def main():
    log.info("=" * 72)
    log.info("  Digital Twin — LoRa Smart Metering  |  Starting up …")
    log.info("=" * 72)

    # ── 7.1  Load dataset into RAM ────────────────────────────────────────────
    load_table = load_csv(CSV_PATH)

    # ── 7.2  Build per-node noise multipliers (static, seeded) ───────────────
    noise_mults = build_noise_multipliers(NODE_IDS, NOISE_SEED)
    log.info("Noise multipliers: %s",
             {nid: f"{m:.4f}" for nid, m in noise_mults.items()})

    # ── 7.3  Initialise household objects ─────────────────────────────────────
    households = [Household(nid, noise_mults[nid]) for nid in NODE_IDS]

    # ── 7.4  Open serial port ─────────────────────────────────────────────────
    ser = open_serial(SERIAL_PORT, SERIAL_BAUDRATE, SERIAL_TIMEOUT)

    # ── 7.5  Initialise virtual clock ─────────────────────────────────────────
    vclock = VirtualClock(
        accelerator  = TIME_ACCELERATOR,
        billing_days = BILLING_CYCLE_DAYS,
    )

    log.info(
        "Simulation started.  TIME_ACCELERATOR=%d  "
        "(1 real-sec = %d virtual-min)",
        TIME_ACCELERATOR, TIME_ACCELERATOR,
    )
    log.info(
        "Loop tick rate ≈ %.3f real-seconds  |  TDMA gap = %d ms  |  "
        "Nodes: %s",
        60.0 / TIME_ACCELERATOR,          # real seconds per virtual hour
        TDMA_GAP_MS,
        NODE_IDS,
    )

    # ────────────────────────────────────────────────────────────────────────
    #  MAIN SIMULATION LOOP
    #
    #  Each iteration:
    #   1. Tick the virtual clock  → Δt in virtual hours
    #   2. Check for billing cycle rollover
    #   3. For each household:
    #       a. Compute instantaneous power (CSV lookup + noise)
    #       b. Euler-integrate cumulative kWh
    #       c. If this node is due for transmission (TDMA rule):
    #           i.  Build checksummed packet
    #           ii. Write to serial
    #           iii. Wait TDMA_GAP_MS before the next node
    #   4. Sleep for the remainder of the real-time tick period
    # ────────────────────────────────────────────────────────────────────────

    # Real-time target period per loop iteration (seconds).
    # With TIME_ACCELERATOR=60 this is 1.0 s (one iteration = 1 virtual hour).
    LOOP_PERIOD_S = 60.0 / TIME_ACCELERATOR

    while True:
        loop_start_real = time.monotonic()

        # ── Step 1: Advance virtual clock ─────────────────────────────────────
        delta_vhours = vclock.tick()

        # ── Step 2: Billing cycle rollover check ──────────────────────────────
        if vclock.billing_cycle_complete:
            vclock.reset_billing_cycle()
            for hh in households:
                hh.reset_billing()

        log.info("┌── %s  (Δvt=%.4f vhrs) ──────────────────────────────",
                 vclock, delta_vhours)

        # ── Step 3: Per-household processing ──────────────────────────────────
        tx_count = 0   # how many nodes transmitted this iteration

        for hh in households:

            # 3a. Compute instantaneous power (includes ERR handling)
            power_str = hh.compute_power(
                load_table, vclock.virtual_day, vclock.virtual_hour
            )

            # 3b. Euler integration — accumulate kWh
            hh.euler_integrate(power_str, delta_vhours)

            # 3c. TDMA transmission gate
            #
            #   Rule: a node transmits at most ONCE per virtual hour.
            #
            #   vclock.due_for_transmission() compares the current virtual hour
            #   to the hour of the node's last transmission.  When they differ,
            #   the node is cleared to transmit.
            #
            #   Staggering (collision avoidance):
            #   ──────────────────────────────────
            #   After each serial.write() we sleep TDMA_GAP_MS milliseconds
            #   before writing the next node's packet.  With 10 nodes × 100 ms
            #   the total transmission window = 1 000 ms = 1 real second,
            #   which fits neatly inside the 1-second loop tick.
            #
            #   Timeline (real time, within one loop iteration):
            #     t = 0 ms   → Node 101 packet written
            #     t = 100 ms → Node 102 packet written
            #     t = 200 ms → Node 103 packet written
            #     …
            #     t = 900 ms → Node 110 packet written
            #     t = 1000 ms → loop sleeps until next tick
            #
            #   This mimics a software Time Division Multiple Access (TDMA)
            #   schedule, where each node "owns" a dedicated time slot and
            #   no two nodes are ever on the RF channel simultaneously.

            if vclock.due_for_transmission(hh.last_tx_vhour):

                # Build the packet
                packet = build_packet(hh.node_id, power_str, hh.cumulative_kwh)

                # Transmit (or dry-run print)
                send_packet(ser, packet, dry_run_label=f"Node {hh.node_id} ")

                # Log the transmission for auditability
                log.info(
                    "│  TX  Node %-3d  pwr=%-10s  kWh=%.4f  pkt=%s",
                    hh.node_id,
                    power_str,
                    hh.cumulative_kwh,
                    packet.decode("utf-8").strip(),
                )

                # Record the virtual hour of this transmission
                hh.last_tx_vhour = vclock.virtual_hour
                tx_count += 1

                # ── TDMA inter-packet delay ────────────────────────────────────
                #   Sleep TDMA_GAP_MS real milliseconds before the next node's
                #   packet.  This ensures the Arduino's serial RX buffer and
                #   the LoRa TX queue are never simultaneously overloaded.
                if tx_count < len(households):
                    time.sleep(TDMA_GAP_MS / 1000.0)

            else:
                # Node has already transmitted this virtual hour → skip
                log.debug("│  SKIP  Node %-3d  (already tx this vhour)", hh.node_id)

        log.info("└── %d node(s) transmitted this tick.", tx_count)

        # ── Step 4: Sleep for the remainder of the real-time tick period ───────
        #
        #   We subtract the time already spent (computation + TDMA delays)
        #   from the target loop period.  If the loop overran (negative sleep),
        #   we skip the sleep and immediately start the next iteration.
        #
        elapsed_real = time.monotonic() - loop_start_real
        remaining_s  = LOOP_PERIOD_S - elapsed_real
        if remaining_s > 0:
            time.sleep(remaining_s)
        else:
            log.warning(
                "Loop overrun by %.3f s — consider increasing TDMA_GAP_MS "
                "or reducing TIME_ACCELERATOR.",
                -remaining_s,
            )


# ══════════════════════════════════════════════════════════════════════════════
#  ENTRY POINT
# ══════════════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    try:
        main()
    except FileNotFoundError as exc:
        log.critical("Startup failed: %s", exc)
        sys.exit(1)
    except KeyboardInterrupt:
        log.info("Simulation interrupted by user.  Goodbye.")
        sys.exit(0)
#!/usr/bin/env python3
"""Renders a scene's scripted camera shots to PNG through the engine's
remote-control endpoint (the same JSON-RPC methods quat-mcp drives).

    scripts/render-shots.py SHOTS_JSON OUT_DIR [--game BIN] [--only NAME ...]

SHOTS_JSON (e.g. assets/showcase/cinematic/shots.json) names the scene, the
window size and the shots:

    {"scene": "assets/scenes/cinematic_forest.json",
     "window": [1920, 1080],
     "settle_frames": 24,
     "calls": [{"method": "render.settings.set", "params": {...}}],
     "shots": [{"name": "wide", "position": [x, y, z], "look_at": [x, y, z],
                "fov_deg": 40, "calls": [...]}]}

The game starts under Xvfb (xvfb-run) on the default Vulkan driver (set
VK_ICD_FILENAMES for lavapipe) with QUAT_SCENE, a loopback endpoint on a free
port and a random session token. `calls` (scene-wide, then per shot) are sent
as they are, so a shot can change render settings, the sun or a component
before it is taken; a call that fails stops the run with the engine's error.
Each shot: camera.set, wait `settle_frames` rendered frames (temporal
anti-aliasing history and exposure adaptation), then render.screenshot at the
window's full width into OUT_DIR/<name>.png.

Exit status: 0 = every shot written, 1 = the engine failed or a call was
refused (the message says which), 2 = bad usage.
"""

import argparse
import base64
import json
import os
import pathlib
import secrets
import signal
import socket
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
CONNECT_TIMEOUT_S = 600   # first frame on lavapipe loads and cooks every asset
CALL_TIMEOUT_S = 900


class ShotError(Exception):
    pass


def log(msg):
    print(f"render-shots: {msg}", flush=True)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Rpc:
    def __init__(self, port, token, proc):
        deadline = time.monotonic() + CONNECT_TIMEOUT_S
        while True:
            if proc.poll() is not None:
                raise ShotError(f"the game exited ({proc.returncode}) before the endpoint opened")
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise ShotError("timed out waiting for the remote endpoint") from None
                time.sleep(1)
        self.sock.settimeout(CALL_TIMEOUT_S)
        self.buf = b""
        self.next_id = 1
        self.call("session.hello", {"token": token})

    def call(self, method, params=None):
        req = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            req["params"] = params
        self.next_id += 1
        self.sock.sendall((json.dumps(req) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise ShotError(f"{method}: the engine closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        reply = json.loads(line)
        if "error" in reply:
            err = reply["error"]
            raise ShotError(f"{method}: {err.get('message', err)}")
        return reply.get("result")

    def wait_frames(self, n):
        start = self.call("engine.info")["frame"]
        while self.call("engine.info")["frame"] < start + n:
            time.sleep(0.2)


def run_calls(rpc, calls, where):
    for c in calls or []:
        if not isinstance(c, dict) or not isinstance(c.get("method"), str):
            raise ShotError(f"{where}: every call needs a string 'method'")
        rpc.call(c["method"], c.get("params"))


def on_signal(signum, _frame):
    raise ShotError(f"interrupted by signal {signum}")


def main():
    # Turn SIGTERM / SIGINT into an exception so the game's process group is
    # always signalled (an orphaned game keeps rendering on the CPU).
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("shots")
    ap.add_argument("out_dir")
    ap.add_argument("--game", default=str(ROOT / "build/release/game"))
    ap.add_argument("--only", nargs="*", help="render only these shot names")
    args = ap.parse_args()
    try:
        spec = json.loads(pathlib.Path(args.shots).read_text())
        scene = spec["scene"]
        width, height = (int(v) for v in spec.get("window", [1920, 1080]))
        settle = int(spec.get("settle_frames", 24))
        shots = [s for s in spec["shots"] if not args.only or s["name"] in args.only]
    except (OSError, ValueError, KeyError, TypeError) as e:
        log(f"bad shots file {args.shots}: {e}")
        return 2
    out = pathlib.Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)

    port, token = free_port(), secrets.token_hex(16)
    env = dict(os.environ, QUAT_SCENE=scene, QUAT_REMOTE=str(port), QUAT_REMOTE_TOKEN=token,
               QUAT_NO_UI="1", QUAT_NO_AUDIO="1", QUAT_NO_VSYNC="1",
               QUAT_WINDOW_SIZE=f"{width}x{height}", SDL_VIDEO_DRIVER="x11")
    log_path = out / "render-shots.log"
    with open(log_path, "w") as game_log:
        proc = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {width}x{height}x24", args.game],
            env=env, stdout=game_log, stderr=subprocess.STDOUT, cwd=ROOT,
            start_new_session=True)  # one process group: xvfb-run, Xvfb and the game
        try:
            rpc = Rpc(port, token, proc)
            run_calls(rpc, spec.get("calls"), "calls")
            for shot in shots:
                name = shot["name"]
                t0 = time.monotonic()
                run_calls(rpc, shot.get("calls"), f"shot {name}")
                rpc.call("camera.set", {k: shot[k] for k in ("position", "look_at", "fov_deg")
                                        if k in shot})
                rpc.wait_frames(int(shot.get("settle_frames", settle)))
                png = rpc.call("render.screenshot", {"max_size": max(width, height)})
                (out / f"{name}.png").write_bytes(base64.b64decode(png["data"]))
                log(f"{name}: {png['width']}x{png['height']} in {time.monotonic() - t0:.0f} s")
        except (ShotError, OSError, KeyError, ValueError) as e:
            log(f"FAILED: {e} (engine log: {log_path})")
            return 1
        finally:
            # Signal the whole group: terminating only xvfb-run would leave
            # the game running.
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Fetches the CC0 Poly Haven assets an asset manifest lists, verifies them
and prepares them for the engine.

    scripts/fetch-polyhaven.py MANIFEST [--cook-with QUAT_COOK] [--check]

MANIFEST is a JSON file such as assets/showcase/cinematic/assets.json: a list
of Poly Haven ids, each with its type (model / texture / hdri), resolution
and how the engine uses it. Everything downloads into the gitignored caches
under the project root (the directory above scripts/), laid out exactly as
quat-mcp's `asset_fetch` tool lays out models, so either path produces the
same tree:

    assets/meshes/polyhaven/<id>/     glTF + its files, LICENSE.md,
                                      polyhaven.json, <id>.glb (cooked)
    assets/textures/polyhaven/<id>/   texture maps (+ a generated ground glTF)
    assets/hdri/polyhaven/<id>/       the equirectangular .hdr

Every file is checked against the size and MD5 Poly Haven's API publishes; a
file already present with the right checksum is not downloaded again, so the
script is cheap to re-run over a restored CI cache. An asset's
`content_sha256` in the manifest pins those (path, MD5, size) lists, so a
republished asset fails the fetch instead of silently changing a scene.
Downloads come only over https from dl.polyhaven.org (API calls from
api.polyhaven.com), redirects included; they are streamed to disk with a size
cap and a wall-clock deadline, and every path from the API is confined to the
asset's folder.

--cook-with runs the engine's cooker (quat-cook) for each model whose manifest
entry has a `cook` list: one GLB per entry (KTX2 textures, meshopt meshes,
`--simplify` levels of detail), <id>.glb unless the entry names its
`output`; the scene files reference those. A cooked file is rebuilt only
when its glTF or cook arguments change (an `<output>.cook-stamp` records
them) or with --recook (after a cooker change; CI keys its cooked-model
cache on the cooker sources instead). --check downloads nothing: it exits 0
when every file the manifest produces is present (cooked models included),
1 otherwise.

Standard library only (it runs on a bare CI runner). Exit status: 0 = done,
1 = an asset failed (the message names it), 2 = bad usage or manifest.
"""

import argparse
import base64
import hashlib
import http.client
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
API = "https://api.polyhaven.com"
DOWNLOAD_HOST = "dl.polyhaven.org"
USER_AGENT = "quat-3d-fetch-polyhaven/1"
LICENSE = "CC0-1.0"
LICENSE_URL = "https://creativecommons.org/publicdomain/zero/1.0/"
# Per asset, like asset_fetch's default cap (QUAT_MCP_ASSET_MAX_MB).
MAX_ASSET_BYTES = 512 * 1024 * 1024
MAX_FILES = 256
MAX_API_BYTES = 16 * 1024 * 1024
# Wall-clock budget of one file: a minute plus 256 KiB/s.
MIN_RATE_BYTES_PER_S = 256 * 1024
DIRS = {
    "model": pathlib.Path("assets/meshes/polyhaven"),
    "texture": pathlib.Path("assets/textures/polyhaven"),
    "hdri": pathlib.Path("assets/hdri/polyhaven"),
}
RESOLUTIONS = ("1k", "2k", "4k")
TEXTURE_MAPS = ("Diffuse", "nor_gl", "arm", "Rough", "AO", "Displacement")


class FetchError(Exception):
    pass


def log(msg):
    print(f"fetch-polyhaven: {msg}", flush=True)


def safe_relative(rel):
    """`rel` as a relative path that cannot leave its base, else None."""
    if not isinstance(rel, str) or not rel or "\\" in rel or "\0" in rel:
        return None
    p = pathlib.PurePosixPath(rel)
    if p.is_absolute() or any(part in ("", ".", "..") for part in p.parts):
        return None
    return pathlib.Path(*p.parts)


def check_url(url, host=DOWNLOAD_HOST):
    u = urllib.parse.urlparse(url)
    if u.scheme != "https" or u.hostname != host:
        raise FetchError(f"refusing URL outside https://{host}: {url}")


class _ConfinedRedirects(urllib.request.HTTPRedirectHandler):
    """Follows a redirect only to the host the request was confined to."""

    def __init__(self, host):
        super().__init__()
        self.host = host

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        check_url(newurl, self.host)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


# Longest a single socket operation may block (connect or one recv).
SOCKET_TIMEOUT_S = 30
# Bytes per read: read1() returns after at most one recv, so the wall-clock
# deadline below is checked after every network read, not every megabyte.
READ_CHUNK = 1 << 16


def stream(url, host, sink, max_bytes, deadline_s):
    """GETs `url` (https on `host`, redirects re-checked), passing each chunk
    to `sink`; fails past `max_bytes` or the wall-clock `deadline_s`. Every
    socket operation is bounded by SOCKET_TIMEOUT_S (and by the time left),
    and the deadline is checked after each recv, so a server trickling bytes
    overruns it by at most one socket timeout. A body cut short raises
    http.client.IncompleteRead, which `retried` retries."""
    check_url(url, host)
    opener = urllib.request.build_opener(_ConfinedRedirects(host))
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    deadline = time.monotonic() + deadline_s
    got = 0
    timeout = max(1.0, min(SOCKET_TIMEOUT_S, deadline_s))
    with opener.open(req, timeout=timeout) as r:
        expected = r.headers.get("Content-Length")
        while chunk := r.read1(READ_CHUNK):
            got += len(chunk)
            if got > max_bytes:
                raise FetchError(f"{url}: more than the expected {max_bytes} bytes")
            if time.monotonic() > deadline:
                raise FetchError(f"{url}: download exceeded {deadline_s:.0f} s")
            sink(chunk)
        # read1 returns b"" at the end of the body; a connection closed early
        # leaves fewer bytes than the server announced.
        if expected is not None and expected.isdigit() and got < int(expected):
            raise http.client.IncompleteRead(b"", int(expected) - got)
    return got


def retried(what, fn, attempts=3):
    last = None
    for attempt in range(attempts):
        try:
            return fn()
        except (urllib.error.URLError, TimeoutError, ConnectionError,
                http.client.HTTPException) as e:
            # HTTPException covers IncompleteRead (a truncated body) and
            # malformed responses: retried, then reported as a FetchError.
            last = e
            time.sleep(2 * (attempt + 1))
    raise FetchError(f"{what} failed: {last}")


def api_json(path):
    url = f"{API}/{path}"
    host = urllib.parse.urlparse(API).hostname

    def get():
        parts = []
        stream(url, host, parts.append, MAX_API_BYTES, 120)
        return b"".join(parts)

    try:
        doc = json.loads(retried(f"GET {url}", get))
    except json.JSONDecodeError as e:
        raise FetchError(f"{url}: not JSON ({e})") from None
    if not isinstance(doc, dict):
        raise FetchError(f"{url}: expected a JSON object")
    return doc


def md5_of(path):
    h = hashlib.md5(usedforsecurity=False)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verified(path, size, md5):
    return path.is_file() and path.stat().st_size == size and md5_of(path) == md5


def download(entry, dest):
    """Downloads one API file entry ({url, size, md5}) to `dest`, verified."""
    url, size, md5 = entry.get("url"), entry.get("size"), entry.get("md5")
    if not isinstance(url, str) or not isinstance(size, int) or not isinstance(md5, str):
        raise FetchError(f"malformed file entry for {dest.name}")
    check_url(url)
    if verified(dest, size, md5):
        return False
    dest.parent.mkdir(parents=True, exist_ok=True)

    def get():
        # Streamed to a temporary file while hashing: bounded memory, at
        # most size + 1 bytes accepted, removed on any failure.
        fd, tmp = tempfile.mkstemp(dir=dest.parent, prefix=".part-")
        try:
            h = hashlib.md5(usedforsecurity=False)
            with os.fdopen(fd, "wb") as f:
                def sink(chunk):
                    h.update(chunk)
                    f.write(chunk)
                got = stream(url, DOWNLOAD_HOST, sink, size, 60 + size / MIN_RATE_BYTES_PER_S)
            if got != size or h.hexdigest() != md5:
                raise FetchError(f"{url}: size or checksum mismatch (got {got} bytes)")
            os.replace(tmp, dest)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)

    retried(f"GET {url}", get)
    return True


def plan_files(asset, files):
    """[(relative path, entry)] of everything one asset downloads."""
    kind, res, aid = asset["type"], asset["resolution"], asset["id"]
    try:
        if kind == "model":
            g = files["gltf"][res]["gltf"]
            out = [(f"{aid}_{res}.gltf", g)]
            include = g.get("include", {})
            if not isinstance(include, dict):
                raise TypeError("include")
            out += sorted(include.items())
        elif kind == "texture":
            out = [(pathlib.PurePosixPath(files[m][res]["jpg"]["url"]).name,
                    files[m][res]["jpg"]) for m in asset["maps"]]
        else:
            h = files["hdri"][res]["hdr"]
            out = [(pathlib.PurePosixPath(h["url"]).name, h)]
    except (KeyError, TypeError):
        raise FetchError(f"{aid}: Poly Haven has no {kind} files at {res}") from None
    plan = []
    for rel, entry in out:
        p = safe_relative(rel)
        if p is None:
            raise FetchError(f"{aid}: unsafe path from the API: {rel!r}")
        if not (isinstance(entry, dict) and isinstance(entry.get("url"), str)
                and isinstance(entry.get("size"), int) and entry["size"] >= 0
                and isinstance(entry.get("md5"), str)):
            raise FetchError(f"{aid}: malformed file entry for {rel!r} from the API")
        plan.append((p, entry))
    total = sum(e["size"] for _, e in plan)
    if len(plan) > MAX_FILES or total > MAX_ASSET_BYTES:
        raise FetchError(f"{aid}: {len(plan)} files / {total} bytes exceeds the cap")
    return plan


def content_digest(plan):
    """SHA-256 over the sorted (path, md5, size) of an asset's files: what the
    manifest pins, so a republished asset is noticed instead of silently
    changing the scene."""
    h = hashlib.sha256()
    for p, e in sorted(plan, key=lambda pe: pe[0].as_posix()):
        h.update(f"{p.as_posix()}\0{e['md5']}\0{e['size']}\n".encode())
    return h.hexdigest()


def write_provenance(asset, info, plan, folder):
    authors = info.get("authors") or asset.get("authors") or {}
    if not isinstance(authors, dict) or not all(
            isinstance(k, str) and isinstance(v, str) for k, v in authors.items()):
        raise FetchError(f"{asset['id']}: malformed authors from the API")
    name = info.get("name")
    if not isinstance(name, str):
        raise FetchError(f"{asset['id']}: malformed name from the API")
    prov = {
        "source": "Poly Haven",
        "id": asset["id"],
        "name": name,
        "source_url": f"https://polyhaven.com/a/{asset['id']}",
        "license": LICENSE,
        "license_url": LICENSE_URL,
        "authors": authors,
        "resolution": asset["resolution"],
        "files": [{"path": p.as_posix(), "url": e["url"], "bytes": e["size"], "md5": e["md5"]}
                  for p, e in plan],
    }
    (folder / "polyhaven.json").write_text(json.dumps(prov, indent=2) + "\n")
    lines = [f"# {prov['name']}", "",
             f"Downloaded from Poly Haven: {prov['source_url']}", "",
             f"License: [{LICENSE}]({LICENSE_URL}) (public domain dedication). You may use, "
             "modify and redistribute this asset, commercially or not, without asking "
             "permission; attribution is not required but appreciated.", "", "Authors:"]
    lines += [f"- {a} ({role})" for a, role in sorted(authors.items())]
    lines += ["", f"Resolution {asset['resolution']}, fetched by scripts/fetch-polyhaven.py. "
              "`polyhaven.json` lists every file with its source URL and checksum.", ""]
    (folder / "LICENSE.md").write_text("\n".join(lines))


# ---- Generated ground plane -------------------------------------------------

def ground_gltf(asset, plan, spec):
    """A flat, tiled ground plane (glTF 2.0, embedded buffer) using the
    texture maps of `asset`: `size` metres square, centred at the origin,
    facing +Y, with the texture repeating every `tile` metres."""
    size, tile = float(spec["size"]), float(spec["tile"])
    n = int(spec.get("segments", 16))
    if not (1.0 <= size <= 10000.0 and 0.05 <= tile <= size and 1 <= n <= 256):
        raise FetchError(f"{asset['id']}: ground size/tile/segments out of range")
    pos, nrm, uv, tan, idx = [], [], [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            x, z = -size / 2 + size * i / n, -size / 2 + size * j / n
            pos += [x, 0.0, z]
            nrm += [0.0, 1.0, 0.0]
            uv += [x / tile, z / tile]
            tan += [1.0, 0.0, 0.0, 1.0]
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            b, c, d = a + 1, a + n + 1, a + n + 2
            idx += [a, c, b, b, c, d]  # counter-clockwise seen from +Y
    blobs = [struct.pack(f"<{len(pos)}f", *pos), struct.pack(f"<{len(nrm)}f", *nrm),
             struct.pack(f"<{len(uv)}f", *uv), struct.pack(f"<{len(tan)}f", *tan),
             struct.pack(f"<{len(idx)}I", *idx)]
    views, offset = [], 0
    for b in blobs:
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(b)})
        offset += len(b)
    vcount = (n + 1) ** 2
    half = size / 2
    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": vcount, "type": "VEC3",
         "min": [-half, 0.0, -half], "max": [half, 0.0, half]},
        {"bufferView": 1, "componentType": 5126, "count": vcount, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": vcount, "type": "VEC2"},
        {"bufferView": 3, "componentType": 5126, "count": vcount, "type": "VEC4"},
        {"bufferView": 4, "componentType": 5125, "count": len(idx), "type": "SCALAR"},
    ]
    names = {m: p.name for m, (p, _) in zip(asset["maps"], plan)}
    for m in ("Diffuse", "nor_gl", "arm"):
        if m not in names:
            raise FetchError(f"{asset['id']}: a ground plane needs the {m} map")
    images = [{"uri": urllib.parse.quote(names[m])} for m in ("Diffuse", "nor_gl", "arm")]
    material = {
        "name": asset["id"],
        "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0},
            "metallicRoughnessTexture": {"index": 2},
            "metallicFactor": 0.0,
            "roughnessFactor": float(spec.get("roughness", 1.0)),
        },
        "normalTexture": {"index": 1, "scale": float(spec.get("normal_scale", 1.0))},
        "occlusionTexture": {"index": 2},
    }
    data = b"".join(blobs)
    return {
        "asset": {"version": "2.0", "generator": "quat-3d scripts/fetch-polyhaven.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": f"{asset['id']}_ground", "mesh": 0}],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "TANGENT": 3},
            "indices": 4, "material": 0}]}],
        "materials": [material],
        "textures": [{"sampler": 0, "source": i} for i in range(3)],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "images": images,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(data),
                     "uri": "data:application/octet-stream;base64,"
                            + base64.b64encode(data).decode()}],
    }


# ---- Cooking ----------------------------------------------------------------

def cook_outputs(asset):
    """[(output file name, quat-cook arguments)] of a model's `cook` list:
    each entry is {"output": "<name>.glb", "args": [...]}; the output
    defaults to <id>.glb (e.g. a full-detail cook plus simplified LODs)."""
    cook = asset.get("cook")
    if cook is None:
        return []
    if not isinstance(cook, list) or not cook:
        raise FetchError(f"{asset['id']}: cook must be a non-empty list of outputs")
    outs, names = [], set()
    for entry in cook:
        if not isinstance(entry, dict):
            raise FetchError(f"{asset['id']}: every cook entry must be an object")
        name = entry.get("output", f"{asset['id']}.glb")
        args = entry.get("args", [])
        p = safe_relative(name)
        if p is None or len(p.parts) != 1 or p.suffix != ".glb" or name in names:
            raise FetchError(f"{asset['id']}: cook output must be a unique file name ending .glb")
        if not isinstance(args, list) or not all(isinstance(a, str) for a in args):
            raise FetchError(f"{asset['id']}: cook args must be a list of strings")
        names.add(name)
        outs.append((name, args))
    return outs


def cook(asset, folder, cooker, gltf_rel, force):
    """Cooks every output that is missing or stale; returns how many ran."""
    ran = 0
    src = folder / gltf_rel
    src_md5 = md5_of(src)
    for name, args in cook_outputs(asset):
        out = folder / name
        stamp = folder / f"{name}.cook-stamp"
        want = json.dumps({"args": args, "input_md5": src_md5})
        if not force and out.is_file() and stamp.is_file() and stamp.read_text() == want:
            continue
        log(f"{asset['id']}: cooking {name} ({' '.join(args) or 'defaults'})")
        r = subprocess.run([str(cooker), str(src), str(out), *args], capture_output=True,
                           text=True, timeout=1800, check=False)
        if r.returncode != 0:
            raise FetchError(f"{asset['id']}: quat-cook failed ({r.returncode}): "
                             f"{(r.stderr or r.stdout).strip()[-2000:]}")
        for line in r.stdout.splitlines():  # e.g. "meshes ... 6972997 -> 451446 triangles"
            if line.strip().startswith("meshes"):
                log(f"{asset['id']}: {name}: {line.strip()}")
        stamp.write_text(want)
        ran += 1
    return ran


# ---- Manifest ---------------------------------------------------------------

def load_manifest(path):
    try:
        doc = json.loads(pathlib.Path(path).read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise FetchError(f"cannot read manifest {path}: {e}") from None
    assets = doc.get("assets") if isinstance(doc, dict) else None
    if not isinstance(assets, list) or not assets:
        raise FetchError(f"{path}: expected an object with a non-empty 'assets' list")
    seen = set()
    for a in assets:
        if not isinstance(a, dict):
            raise FetchError(f"{path}: every asset must be an object")
        aid, kind, res = a.get("id"), a.get("type"), a.get("resolution")
        if not isinstance(aid, str) or safe_relative(aid) is None or "/" in aid:
            raise FetchError(f"{path}: bad asset id {aid!r}")
        if kind not in DIRS:
            raise FetchError(f"{aid}: type must be one of {sorted(DIRS)}")
        if res not in RESOLUTIONS:
            raise FetchError(f"{aid}: resolution must be one of {RESOLUTIONS}")
        if a.get("license") != LICENSE:
            raise FetchError(f"{aid}: only {LICENSE} assets are allowed")
        if kind == "texture":
            maps = a.get("maps")
            if not isinstance(maps, list) or not maps or any(m not in TEXTURE_MAPS for m in maps):
                raise FetchError(f"{aid}: texture maps must be a list from {TEXTURE_MAPS}")
        pin = a.get("content_sha256")
        if pin is not None and (not isinstance(pin, str) or len(pin) != 64):
            raise FetchError(f"{aid}: content_sha256 must be a 64-digit hex string")
        if (kind, aid) in seen:
            raise FetchError(f"{aid}: listed twice")
        seen.add((kind, aid))
        cook_outputs(a)
    return assets


def expected_outputs(asset):
    folder = ROOT / DIRS[asset["type"]] / asset["id"]
    outs = [folder / "polyhaven.json"]
    outs += [folder / name for name, _ in cook_outputs(asset)]
    if asset["type"] == "texture" and asset.get("ground"):
        outs.append(folder / asset["ground"]["file"])
    return outs


def fetch_asset(asset, cooker, recook):
    aid, kind = asset["id"], asset["type"]
    folder = ROOT / DIRS[kind] / aid
    info = api_json(f"info/{aid}")
    if "name" not in info:
        raise FetchError(f"{aid}: not a Poly Haven asset")
    plan = plan_files(asset, api_json(f"files/{aid}"))
    digest = content_digest(plan)
    pinned = asset.get("content_sha256")
    if pinned is None:
        log(f"{aid}: not pinned; add \"content_sha256\": \"{digest}\" to the manifest")
    elif pinned != digest:
        raise FetchError(f"{aid}: Poly Haven's files differ from the pinned content_sha256 "
                         f"(now {digest}); review the asset and update the manifest")
    fresh = sum(download(e, folder / p) for p, e in plan)
    write_provenance(asset, info, plan, folder)
    note = f"{fresh} downloaded" if fresh else "up to date"
    if kind == "texture" and asset.get("ground"):
        spec = asset["ground"]
        out = safe_relative(spec.get("file", ""))
        if out is None or out.suffix != ".gltf":
            raise FetchError(f"{aid}: ground.file must be a relative .gltf name")
        (folder / out).write_text(json.dumps(ground_gltf(asset, plan, spec)) + "\n")
    if kind == "model" and asset.get("cook") is not None:
        if cooker is None:
            note += "; not cooked (no --cook-with)"
        elif (n := cook(asset, folder, cooker, plan[0][0], recook)) > 0:
            note += f"; {n} cooked"
    log(f"{aid} ({kind} {asset['resolution']}): {note}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("manifest")
    ap.add_argument("--cook-with", metavar="QUAT_COOK", help="path to the quat-cook binary")
    ap.add_argument("--recook", action="store_true",
                    help="cook every model again (after a cooker change)")
    ap.add_argument("--check", action="store_true",
                    help="download nothing; exit 1 unless every output is present")
    args = ap.parse_args()
    try:
        assets = load_manifest(args.manifest)
    except FetchError as e:
        log(str(e))
        return 2
    if args.check:
        missing = [str(p.relative_to(ROOT)) for a in assets for p in expected_outputs(a)
                   if not p.is_file()]
        for m in missing:
            log(f"missing: {m}")
        return 1 if missing else 0
    cooker = None
    if args.cook_with:
        cooker = pathlib.Path(args.cook_with).resolve()
        if not cooker.is_file() or not os.access(cooker, os.X_OK):
            log(f"--cook-with: not an executable: {cooker}")
            return 2
    failed = 0
    for asset in assets:
        try:
            fetch_asset(asset, cooker, args.recook)
        except (FetchError, OSError, subprocess.TimeoutExpired, KeyError, TypeError,
                ValueError, AttributeError) as e:
            log(f"FAILED {asset['id']}: {e}")
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

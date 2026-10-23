"""Wait for a complete, stable TGA snapshot before writing a verified PNG."""
from __future__ import annotations

import argparse
import io
import json
import math
from pathlib import Path
import sys
import time

from PIL import Image, ImageFile


def convert_when_ready(source: Path, target: Path, timeout: float, poll: float, stable: float) -> dict:
    ImageFile.LOAD_TRUNCATED_IMAGES = False
    deadline = time.monotonic() + timeout
    last_key = None
    stable_since = None
    last_error = "source has not appeared"
    while time.monotonic() < deadline:
        decoded = None
        try:
            before = source.stat()
            payload = source.read_bytes()
            after = source.stat()
            if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns) or len(payload) != after.st_size:
                raise OSError("TGA changed while being read")
            with Image.open(io.BytesIO(payload)) as candidate:
                if candidate.format != "TGA":
                    raise ValueError("capture is not a TGA")
                candidate.load()
                if candidate.width <= 0 or candidate.height <= 0:
                    raise ValueError("TGA has invalid dimensions")
                decoded = candidate.copy()
            key = (after.st_size, after.st_mtime_ns, decoded.size)
            now = time.monotonic()
            if key != last_key:
                stable_since = now
                last_key = key
            if stable_since is not None and now - stable_since >= stable:
                # Exclusive creation prevents any old PNG from being reported as this capture.
                with target.open("xb") as output:
                    decoded.save(output, format="PNG")
                with Image.open(target) as result:
                    result.load()
                    if result.format != "PNG" or result.size != decoded.size:
                        raise ValueError("PNG verification failed")
                return {"png": str(target), "width": decoded.width, "height": decoded.height, "tgaBytes": len(payload)}
        except (OSError, ValueError, SyntaxError) as exc:
            # A header, RLE packet, or pixel payload may still be in flight.
            last_error = str(exc)
            stable_since = None
            last_key = None
            if target.exists():
                raise RuntimeError(f"PNG was already present or could not be verified: {target}") from exc
        finally:
            if decoded is not None:
                decoded.close()
        time.sleep(min(poll, max(0.0, deadline - time.monotonic())))
    raise TimeoutError(f"TGA was not complete and stable within {timeout:g}s: {source}; last error: {last_error}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("target", type=Path)
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--poll-ms", type=float, default=50)
    parser.add_argument("--stable-ms", type=float, default=200)
    args = parser.parse_args()
    if not all(math.isfinite(v) for v in (args.timeout, args.poll_ms, args.stable_ms)) or not 0 < args.timeout <= 300 or args.poll_ms <= 0 or args.stable_ms < 0:
        parser.error("timeout must be in (0, 300], poll must be positive, stable-ms nonnegative; all must be finite")
    if args.source.resolve() == args.target.resolve():
        parser.error("source and target must be different files")
    try:
        result = convert_when_ready(args.source, args.target, args.timeout, args.poll_ms / 1000, args.stable_ms / 1000)
    except (OSError, ValueError, RuntimeError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print("TGA_CONVERTED: " + json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

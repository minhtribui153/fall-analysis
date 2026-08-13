from __future__ import annotations

import argparse
import getpass
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

from dotenv import load_dotenv

load_dotenv()

DEFAULT_URL = "http://127.0.0.1:8765"
VALID_STATUSES = ("pending", "approved", "rejected", "all")


def base_url() -> str:
    return os.getenv("REGISTRATION_ADMIN_URL", DEFAULT_URL).rstrip("/")


def admin_token() -> str:
    return os.getenv("REGISTRATION_ADMIN_TOKEN", "").strip()


def request_json(method: str, path: str, body: dict[str, Any] | None = None, timeout: float = 10.0) -> Any:
    data = None if body is None else json.dumps(body).encode("utf-8")
    headers = {"Accept": "application/json", "Content-Type": "application/json"}
    token = admin_token()
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(base_url() + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
            return None if not raw else json.loads(raw.decode("utf-8"))
    except urllib.error.HTTPError as error:
        raw = error.read().decode("utf-8", errors="replace")
        try:
            detail = json.loads(raw)
        except json.JSONDecodeError:
            detail = raw
        raise RuntimeError(
            f"Server returned HTTP {error.code}: "
            f"{json.dumps(detail) if isinstance(detail, dict) else detail}"
        ) from error
    except urllib.error.URLError as error:
        raise RuntimeError(f"Could not connect to {base_url()}: {error.reason}") from error


def encoded_device_id(value: str) -> str:
    return urllib.parse.quote(value.strip().upper(), safe="")


def print_nodes(payload: dict[str, Any]) -> None:
    rows = payload.get("nodes", [])
    if not rows:
        print("No matching registrations")
        return
    columns = (
        "device_id", "worker_id", "worker_name", "node_uuid", "approval_status",
        "session_id", "token_expires_at", "requested_at", "decided_by",
    )
    widths = {c: len(c) for c in columns}
    for row in rows:
        for c in columns:
            widths[c] = max(widths[c], len(str(row.get(c) or "")))
    print("  ".join(c.ljust(widths[c]) for c in columns))
    print("  ".join("-" * widths[c] for c in columns))
    for row in rows:
        print("  ".join(str(row.get(c) or "").ljust(widths[c]) for c in columns))


def print_workers(payload: dict[str, Any]) -> None:
    rows = payload.get("workers", [])
    if not rows:
        print("No workers")
        return
    columns = ("worker_id", "worker_name", "worker_uuid", "active", "created_at")
    widths = {c: len(c) for c in columns}
    for row in rows:
        for c in columns:
            widths[c] = max(widths[c], len(str(row.get(c) or "")))
    print("  ".join(c.ljust(widths[c]) for c in columns))
    print("  ".join("-" * widths[c] for c in columns))
    for row in rows:
        print("  ".join(str(row.get(c) or "").ljust(widths[c]) for c in columns))


def confirm(action: str, device_id: str, worker: str | None = None) -> bool:
    suffix = f" -> {worker}" if worker else ""
    answer = input(f"{action.title()} {device_id}{suffix}? [y/N] ").strip().lower()
    return answer in {"y", "yes"}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="n1-admin",
        description=(
            "Manage disposable Nesso N1 registrations and assign them to durable workers. "
            "Approving a node creates an expiring bearer-token monitoring session."
        ),
    )
    parser.add_argument("--url", help=f"Registration API URL (default: {DEFAULT_URL})")
    parser.add_argument("--token", help="Admin bearer token; blank uses local-only server mode")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("health")

    lp = sub.add_parser("list", help="List Nesso registration requests")
    lp.add_argument("--status", choices=VALID_STATUSES, default="pending")
    lp.add_argument("--json", action="store_true")

    wp = sub.add_parser("workers", help="List durable workers")
    wp.add_argument("--json", action="store_true")

    sp = sub.add_parser("show", help="Show one Nesso registration and audit history")
    sp.add_argument("device_id")

    ap = sub.add_parser("approve", help="Approve a Nesso and assign it to a worker")
    ap.add_argument("device_id")
    assignment = ap.add_mutually_exclusive_group(required=True)
    assignment.add_argument("--worker-name", help="Existing/new worker name, maximum 16 characters")
    assignment.add_argument("--worker-id", help="Existing worker_id to reuse")
    ap.add_argument("--actor", default=getpass.getuser() or "local-admin")
    ap.add_argument("--reason", default="")
    ap.add_argument("--yes", action="store_true")

    rp = sub.add_parser("reject", help="Reject a Nesso registration")
    rp.add_argument("device_id")
    rp.add_argument("--actor", default=getpass.getuser() or "local-admin")
    rp.add_argument("--reason", default="")
    rp.add_argument("--yes", action="store_true")
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    if args.url:
        os.environ["REGISTRATION_ADMIN_URL"] = args.url
    if args.token is not None:
        os.environ["REGISTRATION_ADMIN_TOKEN"] = args.token

    if args.command == "health":
        print(json.dumps(request_json("GET", "/health"), indent=2))
        return
    if args.command == "workers":
        payload = request_json("GET", "/v1/admin/workers")
        if args.json:
            print(json.dumps(payload, indent=2))
        else:
            print_workers(payload)
        return
    if args.command == "list":
        query = urllib.parse.urlencode({"status": args.status})
        payload = request_json("GET", f"/v1/admin/nodes?{query}")
        if args.json:
            print(json.dumps(payload, indent=2))
        else:
            print_nodes(payload)
        return

    device_id = args.device_id.strip().upper()
    encoded = encoded_device_id(device_id)
    if args.command == "show":
        print(json.dumps(request_json("GET", f"/v1/admin/nodes/{encoded}"), indent=2))
        return

    if args.command == "approve":
        worker_label = args.worker_id or args.worker_name
        if args.worker_name and len(" ".join(args.worker_name.strip().split())) > 16:
            raise ValueError("worker_name must be at most 16 characters")
        if not args.yes and not confirm("approve", device_id, worker_label):
            print("Cancelled")
            return
        body = {
            "actor": args.actor,
            "reason": args.reason,
            "worker_id": args.worker_id,
            "worker_name": args.worker_name,
        }
        print(json.dumps(request_json("POST", f"/v1/admin/nodes/{encoded}/approve", body), indent=2))
        return

    if not args.yes and not confirm("reject", device_id):
        print("Cancelled")
        return
    print(json.dumps(request_json(
        "POST",
        f"/v1/admin/nodes/{encoded}/reject",
        {"actor": args.actor, "reason": args.reason},
    ), indent=2))


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error

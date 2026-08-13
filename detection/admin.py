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
    # When blank, n1_registration_server.py only accepts admin calls from a
    # direct loopback connection on the Raspberry Pi.
    return os.getenv("REGISTRATION_ADMIN_TOKEN", "").strip()


def request_json(
    method: str,
    path: str,
    body: dict[str, Any] | None = None,
    timeout: float = 10.0,
) -> Any:
    data = None if body is None else json.dumps(body).encode("utf-8")
    headers = {
        "Accept": "application/json",
        "Content-Type": "application/json",
    }
    token = admin_token()
    if token:
        headers["Authorization"] = f"Bearer {token}"

    request = urllib.request.Request(
        base_url() + path,
        data=data,
        method=method,
        headers=headers,
    )

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
        raise RuntimeError(
            f"Could not connect to {base_url()}: {error.reason}"
        ) from error


def print_rows(payload: dict[str, Any]) -> None:
    rows = payload.get("nodes", [])
    if not rows:
        print("No matching registrations")
        return

    columns = (
        "device_id",
        "node_uuid",
        "approval_status",
        "token_issued",
        "requested_at",
        "decided_at",
        "decided_by",
        "decision_reason",
        "last_authenticated_at",
        "last_auth_failure_at",
    )
    widths = {column: len(column) for column in columns}

    for row in rows:
        for column in columns:
            widths[column] = max(widths[column], len(str(row.get(column) or "")))

    print("  ".join(column.ljust(widths[column]) for column in columns))
    print("  ".join("-" * widths[column] for column in columns))
    for row in rows:
        print(
            "  ".join(
                str(row.get(column) or "").ljust(widths[column])
                for column in columns
            )
        )


def encoded_device_id(value: str) -> str:
    return urllib.parse.quote(value.strip().upper(), safe="")


def confirm_decision(action: str, device_id: str) -> bool:
    answer = input(
        f"{action.title()} {device_id.strip().upper()}? [y/N] "
    ).strip().lower()
    return answer in {"y", "yes"}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="n1-admin",
        description=(
            "Approve/reject Tervanix BLE registrations through "
            "n1_registration_server.py. With no admin token, run this CLI "
            "locally on the Raspberry Pi using 127.0.0.1."
        ),
    )
    parser.add_argument(
        "--url",
        help=(
            "Registration server URL. Overrides REGISTRATION_ADMIN_URL "
            f"(default: {DEFAULT_URL})"
        ),
    )
    parser.add_argument(
        "--token",
        help=(
            "Admin bearer token. Overrides REGISTRATION_ADMIN_TOKEN. "
            "Leave blank for local-only mode."
        ),
    )

    subcommands = parser.add_subparsers(dest="command", required=True)
    subcommands.add_parser("health", help="Show server health and admin mode")

    list_parser = subcommands.add_parser("list", help="List registrations")
    list_parser.add_argument(
        "--status",
        choices=VALID_STATUSES,
        default="pending",
    )
    list_parser.add_argument(
        "--json",
        action="store_true",
        help="Print the complete JSON response",
    )

    show_parser = subcommands.add_parser(
        "show",
        help="Show one registration and its audit history",
    )
    show_parser.add_argument("device_id")

    for action in ("approve", "reject"):
        decision_parser = subcommands.add_parser(
            action,
            help=f"{action.title()} a registration",
        )
        decision_parser.add_argument("device_id")
        decision_parser.add_argument(
            "--actor",
            default=getpass.getuser() or "local-admin",
        )
        decision_parser.add_argument("--reason", default="")
        decision_parser.add_argument(
            "--yes",
            action="store_true",
            help="Do not ask for confirmation",
        )

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

    if args.command == "list":
        query = urllib.parse.urlencode({"status": args.status})
        payload = request_json("GET", f"/v1/admin/nodes?{query}")
        if args.json:
            print(json.dumps(payload, indent=2))
        else:
            print_rows(payload)
        return

    device_id = args.device_id.strip().upper()
    encoded = encoded_device_id(device_id)

    if args.command == "show":
        payload = request_json("GET", f"/v1/admin/nodes/{encoded}")
        print(json.dumps(payload, indent=2))
        return

    if not args.yes and not confirm_decision(args.command, device_id):
        print("Cancelled")
        return

    payload = request_json(
        "POST",
        f"/v1/admin/nodes/{encoded}/{args.command}",
        {
            "actor": args.actor,
            "reason": args.reason,
        },
    )
    print(json.dumps(payload, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error

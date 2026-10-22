#!/usr/bin/env python3
"""Resolve Provider capabilities and bind approval to command file identity.

Approval is a user confirmation, not a sandbox or a defense against a process
that can continuously modify the same user's executable files.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

MAX_CONFIG = 1024 * 1024
MAX_PROGRAM = 256 * 1024 * 1024


def _stamp(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def _read_config(source):
    with source.open("rb") as stream:
        before = os.fstat(stream.fileno())
        data = stream.read(MAX_CONFIG + 1)
        after = os.fstat(stream.fileno())
    if len(data) > MAX_CONFIG:
        raise ValueError("Provider configuration exceeds 1 MiB")
    if _stamp(before) != _stamp(after) or _stamp(after) != _stamp(source.stat()):
        raise ValueError("Provider configuration changed while reading")
    value = json.loads(data.decode("utf-8"))
    if not isinstance(value, dict):
        raise ValueError("Provider configuration must be an object")
    return value


def command_identity(program):
    """Resolve once and fingerprint the actual executable, without executing it."""
    expanded = os.path.expanduser(os.path.expandvars(str(program)))
    resolved = shutil.which(expanded)
    if not resolved:
        raise ValueError("Provider command is not an executable file")
    path = Path(resolved).resolve(strict=True)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        if before.st_size > MAX_PROGRAM:
            raise ValueError("Provider executable exceeds the 256 MiB audit limit")
        count = 0
        while True:
            data = stream.read(1024 * 1024)
            if not data:
                break
            count += len(data)
            if count > MAX_PROGRAM:
                raise ValueError("Provider executable exceeds the 256 MiB audit limit")
            digest.update(data)
        after = os.fstat(stream.fileno())
    if _stamp(before) != _stamp(after) or _stamp(after) != _stamp(path.stat()):
        raise ValueError("Provider executable changed while reading")
    return (
        str(path),
        dict(
            dev=after.st_dev,
            inode=after.st_ino,
            size=after.st_size,
            mtime_ns=after.st_mtime_ns,
            ctime_ns=after.st_ctime_ns,
            sha256=digest.hexdigest(),
        ),
    )


def verify_command(provider):
    program, identity = command_identity(provider.get("program", ""))
    if program != provider.get("program") or identity != provider.get("_programIdentity"):
        raise ValueError("Provider executable changed; review and authorize it again")
    return program


def load_policy(path):
    path = Path(path).expanduser().resolve()
    base = path.parent
    config = _read_config(path)
    rows = config.get("providers", [])
    if not isinstance(rows, list):
        raise ValueError("providers must be an array")
    active = []
    for original in rows:
        if not isinstance(original, dict):
            raise ValueError("Invalid provider entry")
        if not original.get("enabled", True):
            continue
        row = dict(original)
        inherited = row.pop("inheritFrom", None)
        key = row.pop("inheritKey", "")
        if inherited:
            source = Path(os.path.expandvars(inherited)).expanduser()
            source = (base / source).resolve() if not source.is_absolute() else source.resolve()
            if source != base and base not in source.parents:
                raise ValueError("inheritFrom must stay inside the configuration directory")
            value = _read_config(source)
            for part in str(key).split(".") if key else []:
                value = value.get(part) if isinstance(value, dict) else None
            if not isinstance(value, dict):
                raise ValueError("inheritKey must select an object")
            if value.get("inheritFrom"):
                raise ValueError("Nested provider inheritance is not supported")
            row = dict(value, **row)
        row.pop("_programIdentity", None)
        if row.get("type", "command") == "command":
            arguments = row.get("arguments", [])
            if not isinstance(arguments, list):
                raise ValueError("command provider arguments must be an array")
            row["program"], row["_programIdentity"] = command_identity(row.get("program", ""))
            row["arguments"] = [os.path.expandvars(str(value)) for value in arguments]
        active.append(row)
    encoded = json.dumps(active, sort_keys=True, ensure_ascii=False).encode("utf-8")
    return active, hashlib.sha256(encoded).hexdigest()


def audit(path):
    rows, digest = load_policy(path)
    capabilities = []
    for row in rows:
        kind = row.get("type", "command")
        if kind == "command":
            target = " ".join([row["program"], *row["arguments"]])
        elif kind == "http":
            target = str(row.get("url", ""))
        else:
            target = " ".join(str(row.get(key, "")) for key in ("service", "objectPath", "method"))
        capabilities.append(
            dict(type=kind, target=target, sendRootPaths=bool(row.get("includeRoots", False)))
        )
    return dict(digest=digest, capabilities=capabilities)


if __name__ == "__main__":
    try:
        print(json.dumps(audit(sys.argv[1]), ensure_ascii=False))
    except (OSError, ValueError, IndexError, TypeError) as error:
        print(json.dumps(dict(error=str(error)), ensure_ascii=False))
        sys.exit(1)

"""`python3 -m testdeck setup` — write a testdeck.toml by looking at the host.

The old first run was "copy the example, read 100 lines of comments, edit five
of them". Almost all of that is derivable: an AzerothCore checkout announces
its own layout, and worldserver.conf already says whether SOAP is on and on
which port. This asks only for what the host cannot know — where the install
is if the guess is wrong, and the bridge account's credentials.

Every function below the prompts is pure, so the detection can be tested
without a console.
"""

import os
import re
import shutil
import sys
from pathlib import Path

from . import hostenv

# Where worldserver.conf hides relative to whatever the operator points at:
# the conf itself, an etc/ dir, a dist/ root, a workspace root, or bin/.
CONF_CANDIDATES = (
    "worldserver.conf",
    "etc/worldserver.conf",
    "dist/etc/worldserver.conf",
    "env/dist/etc/worldserver.conf",
    "../etc/worldserver.conf",
    "build/etc/worldserver.conf",
)

DEFAULT_SOAP_USER = "tdbridge"
DEFAULT_SOAP_PORT = 7878


# ---------------------------------------------------------------------------
# Detection (pure)
# ---------------------------------------------------------------------------


def find_worldserver_conf(candidate):
    """Locate worldserver.conf from any sensible path the operator might give.

    A .conf.dist is accepted as a last resort: it means the server has been
    built but never configured, and saying so beats "not found".
    """
    if not candidate:
        return None
    p = Path(str(candidate)).expanduser()
    if p.is_file():
        return p
    for rel in CONF_CANDIDATES:
        hit = (p / rel).resolve()
        if hit.is_file():
            return hit
    for rel in CONF_CANDIDATES:
        hit = (p / (rel + ".dist")).resolve()
        if hit.is_file():
            return hit
    return None


def layout_from_conf(conf):
    """{base, dist, log_dir, worldserver_conf} implied by a worldserver.conf.

    AzerothCore installs as <dist>/etc/worldserver.conf with the binaries and
    every runtime sidecar in <dist>/bin, so the conf's grandparent is the whole
    layout. `base` is the workspace above it — one more level up when the dist
    sits in the conventional env/dist.
    """
    conf = Path(conf).resolve()
    dist = conf.parent.parent
    log_dir = dist / "bin"
    base = dist.parent.parent if dist.parent.name == "env" else dist.parent
    return {"base": base, "dist": dist, "log_dir": log_dir,
            "worldserver_conf": conf}


def install_roots(app_dir):
    """Directories to look for an install under, nearest-match last resort.

    The deck lives at <core>/modules/mod-dungeon-clear/testdeck, and the built
    server can sit on either side of that:

        parents[3]  <workspace>/env/dist   — the core checked out beside env/
        parents[2]  <core>/env/dist        — what `acore.sh` builds by default

    Both are real layouts, and only the first used to be tried, so the stock
    install was the one the wizard could not find. The workspace stays first
    so a host that already resolved that way keeps the same answer; note that
    find_worldserver_conf() accepts a .conf.dist as a last resort, and trying
    the nearer root first would let an unconfigured template outrank a real
    conf one level up.
    """
    app_dir = Path(app_dir).resolve()
    parents = app_dir.parents
    roots = [parents[3] if len(parents) > 3 else None,
             parents[2] if len(parents) > 2 else None,
             app_dir.parent,
             Path.cwd()]
    return [r for r in roots if r is not None]


def guess_layout(app_dir):
    """The install this checkout most likely belongs to, or None.

    Only trusted where a worldserver.conf actually turns up — a guess that
    names a directory nobody built into is worse than asking.
    """
    for candidate in install_roots(app_dir):
        conf = find_worldserver_conf(candidate)
        if conf:
            return layout_from_conf(conf)
    return None


def read_soap_settings(conf_path):
    """(enabled, ip, port) as worldserver.conf currently has them.

    Last assignment wins, matching the core's own parse order.
    """
    try:
        text = Path(conf_path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False, "127.0.0.1", DEFAULT_SOAP_PORT

    def last(key, default):
        m = re.findall(rf'^\s*{key}\s*=\s*"?([^"\s]+)"?', text, re.M)
        return m[-1] if m else default

    enabled = last("SOAP.Enabled", "0") == "1"
    ip = last("SOAP.IP", "127.0.0.1")
    try:
        port = int(last("SOAP.Port", str(DEFAULT_SOAP_PORT)))
    except ValueError:
        port = DEFAULT_SOAP_PORT
    return enabled, ip, port


def soap_url(ip, port):
    """0.0.0.0 means "listen everywhere", which is not an address to dial."""
    host = "127.0.0.1" if ip in ("0.0.0.0", "::", "") else ip
    return f"http://{host}:{port}/"


# ---------------------------------------------------------------------------
# Rendering (pure)
# ---------------------------------------------------------------------------


def _toml_str(value):
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


def render_toml(values):
    """The config file for these answers — only the keys that differ from the
    defaults, so it stays readable and the example file remains the reference
    for everything else."""
    layout = values["layout"]
    lines = [
        "# DC Test Deck — written by `python3 -m testdeck setup`.",
        "# Every other setting has a default; see testdeck.example.toml.",
        "",
        "[paths]",
        f"base = {_toml_str(layout['base'])}",
    ]
    # Spell out the derived paths only where this install does not match the
    # stock layout config.py would infer from `base`.
    if layout["dist"] != layout["base"] / "env" / "dist":
        lines.append(f"dist = {_toml_str(layout['dist'])}")
    if layout["log_dir"] != layout["dist"] / "bin":
        lines.append(f"log_dir = {_toml_str(layout['log_dir'])}")
    if layout["worldserver_conf"] != layout["dist"] / "etc" / "worldserver.conf":
        lines.append(
            f"worldserver_conf = {_toml_str(layout['worldserver_conf'])}")
    if values.get("mysql_bin"):
        lines.append("# No installer puts the MySQL client on PATH on Windows.")
        lines.append(f"mysql_bin = {_toml_str(values['mysql_bin'])}")

    lines += [
        "",
        "[server]",
        'host = "0.0.0.0"',
        f"port = {int(values['port'])}",
        "",
        "[bridge]",
        'type = "soap"',
        f"soap_url = {_toml_str(values['soap_url'])}",
        f"soap_user = {_toml_str(values['soap_user'])}",
    ]
    if values.get("soap_pass"):
        lines.append(f"soap_pass = {_toml_str(values['soap_pass'])}")
    else:
        lines.append("# Password comes from the TESTDECK_SOAP_PASS environment"
                     " variable.")
        lines.append("#soap_pass = \"\"")

    lines += [
        "",
        "[realm]",
        '# "process" watches for the worldserver process; see the example file',
        "# for the systemd option.",
        'status_check = "process"',
        f"process_name = {_toml_str(values['process_name'])}",
        "",
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# The console flow
# ---------------------------------------------------------------------------


def _ask(prompt, default="", secret=False, interactive=True):
    if not interactive:
        return default
    suffix = f" [{default}]" if default and not secret else ""
    try:
        if secret:
            import getpass
            answer = getpass.getpass(f"{prompt}: ")
        else:
            answer = input(f"{prompt}{suffix}: ")
    except (EOFError, KeyboardInterrupt):
        print()
        raise SystemExit("setup aborted.")
    return answer.strip() or default


def _yes(prompt, interactive=True):
    if not interactive:
        return True
    return _ask(f"{prompt} [Y/n]", "y", interactive=True).lower() not in ("n", "no")


def run(app_dir, config_path=None, force=False, interactive=None):
    """Write a config. Returns the path written, or None if nothing was."""
    if interactive is None:
        interactive = sys.stdin.isatty()

    target = Path(config_path).expanduser() if config_path else \
        Path(app_dir) / "testdeck.toml"

    print("DC Test Deck — setup\n")
    if target.is_file() and not force:
        print(f"{target} already exists.")
        if not _yes("Overwrite it?", interactive):
            print("Left it alone. Run `python3 -m testdeck check` to test it.")
            return None
        print()

    # -- 1. the install -----------------------------------------------------
    layout = guess_layout(app_dir)
    if layout:
        print("Found an AzerothCore install:")
        print(f"  worldserver.conf   {layout['worldserver_conf']}")
        print(f"  logs and sidecars  {layout['log_dir']}")
        if not _yes("Is that the server you want to drive?", interactive):
            layout = None
        print()
    while layout is None:
        answer = _ask("Path to your AzerothCore install (or to "
                      "worldserver.conf)", "", interactive=interactive)
        if not answer:
            print("A path is required — Test Deck reads the server's own "
                  "config and log directory.\n", file=sys.stderr)
            if not interactive:
                return None
            continue
        conf = find_worldserver_conf(answer)
        if not conf:
            print(f"No worldserver.conf under {answer}.\n", file=sys.stderr)
            if not interactive:
                return None
            continue
        layout = layout_from_conf(conf)
        print(f"  using {conf}\n")

    # -- 2. the web port ----------------------------------------------------
    port = _ask("Port for the Test Deck web UI", "8790", interactive=interactive)
    try:
        port = int(port)
    except ValueError:
        port = 8790

    # -- 3. the bridge ------------------------------------------------------
    enabled, ip, soap_port = read_soap_settings(layout["worldserver_conf"])
    print("\nTest Deck drives the worldserver over its SOAP interface.")
    if enabled:
        print(f"  worldserver.conf has SOAP enabled on {ip}:{soap_port}. Good.")
    else:
        print(f"  SOAP is currently OFF in {layout['worldserver_conf']}.")
        print("  Set these there and restart the worldserver:\n")
        print("      SOAP.Enabled = 1")
        print('      SOAP.IP      = "127.0.0.1"')
        print(f"      SOAP.Port    = {soap_port}\n")
    print("  It needs a dedicated account at administrator level:\n")
    print(f"      account create {DEFAULT_SOAP_USER} <a long random password>")
    print(f"      account set gmlevel {DEFAULT_SOAP_USER} 3 -1\n")

    soap_user = _ask("SOAP account name", DEFAULT_SOAP_USER,
                     interactive=interactive)
    soap_pass = _ask(f"Password for {soap_user} (blank = read "
                     "$TESTDECK_SOAP_PASS at startup)", "", secret=True,
                     interactive=interactive)

    # -- 4. host facts it can work out itself -------------------------------
    mysql = hostenv.find_mysql(None, layout["base"])
    mysql_bin = "" if (mysql and shutil.which("mysql")) else (mysql or "")
    process_name = "worldserver.exe" if hostenv.IS_WINDOWS else "worldserver"

    text = render_toml({
        "layout": layout, "port": port, "soap_url": soap_url(ip, soap_port),
        "soap_user": soap_user, "soap_pass": soap_pass,
        "mysql_bin": mysql_bin, "process_name": process_name,
    })
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")
    except OSError as e:
        print(f"\ncannot write {target}: {e}", file=sys.stderr)
        return None

    # The file can hold the bridge password; keep it off other users' eyes on
    # POSIX. Windows inherits the user profile's ACL, which is already right.
    if soap_pass and not hostenv.IS_WINDOWS:
        try:
            os.chmod(target, 0o600)
        except OSError:
            pass

    print(f"\nWrote {target}")
    if not mysql:
        print("\nWARNING: no MySQL client found. Test Deck runs its database "
              "queries\nthrough it, so login and the roster picker need one. "
              "Install it and\nset [paths] mysql_bin, or put it on PATH.")
    return target

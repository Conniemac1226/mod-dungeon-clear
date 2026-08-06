"""config.py: derivation from [paths] base, refusals, and the portability
knobs ([bridge], [realm])."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from testdeck import config as tdconfig  # noqa: E402


def load_toml(tmp_path, text, app_dir=None):
    p = tmp_path / "testdeck.toml"
    p.write_text(text)
    return tdconfig.load(str(p), app_dir=app_dir or tmp_path / "app")


def test_paths_derive_from_base(tmp_path):
    cfg = load_toml(tmp_path, f'[paths]\nbase = "{tmp_path}"\n')
    assert cfg.dist == tmp_path / "env" / "dist"
    assert cfg.log_dir == tmp_path / "env" / "dist" / "bin"
    assert cfg.worldserver_conf == cfg.dist / "etc" / "worldserver.conf"
    assert cfg.testruns_file == cfg.log_dir / "dc_testruns.jsonl"


def make_install(root, rel="env/dist"):
    """A built server under `root`; returns its worldserver.conf."""
    dist = root / rel
    (dist / "etc").mkdir(parents=True)
    (dist / "bin").mkdir(parents=True)
    (dist / "etc" / "worldserver.conf").write_text("")
    return dist / "etc" / "worldserver.conf"


def test_a_spelled_out_base_is_never_second_guessed(tmp_path):
    """Detection must not reach around an explicit base: the documented
    contract is that every other path derives from that value."""
    make_install(tmp_path / "elsewhere")
    app_dir = tmp_path / "elsewhere" / "modules" / "mod-dungeon-clear" / "testdeck"
    app_dir.mkdir(parents=True)
    cfg = load_toml(tmp_path, f'[paths]\nbase = "{tmp_path / "named"}"\n',
                    app_dir=app_dir)
    assert cfg.base == tmp_path / "named"
    assert cfg.dist == tmp_path / "named" / "env" / "dist"


def test_paths_without_a_base_find_the_stock_install(tmp_path, monkeypatch):
    """No config at all is a real state — `check` runs there, and the launcher
    serves there when setup is declined. The paths it reports have to name the
    install the deck is actually inside, including the `acore.sh` layout that
    the positional four-levels-up guess never reached."""
    monkeypatch.chdir(tmp_path)
    core = tmp_path / "azerothcore-wotlk"
    conf = make_install(core)
    app_dir = core / "modules" / "mod-dungeon-clear" / "testdeck"
    app_dir.mkdir(parents=True)

    cfg = tdconfig.load(None, app_dir=app_dir)
    assert cfg.worldserver_conf == conf
    assert cfg.base == core
    assert cfg.log_dir == core / "env" / "dist" / "bin"


def test_paths_without_a_base_fall_back_to_the_positional_guess(tmp_path,
                                                                monkeypatch):
    """Nothing found anywhere still has to produce a config — the server boots
    on derived defaults and says what is wrong in its banner."""
    monkeypatch.chdir(tmp_path)
    app_dir = tmp_path / "core" / "modules" / "mod-dungeon-clear" / "testdeck"
    app_dir.mkdir(parents=True)
    cfg = tdconfig.load(None, app_dir=app_dir)
    assert cfg.base == tmp_path
    assert cfg.dist == tmp_path / "env" / "dist"


def test_defaults(tmp_path):
    cfg = load_toml(tmp_path, "")
    assert cfg.port == 8790
    assert cfg.bridge_type == "soap"
    assert cfg.use_sudo is False           # sudo is opt-in, never assumed
    assert cfg.min_gmlevel == 1
    assert cfg.resolved_status_check() == "process"   # no unit named


def test_status_auto_prefers_systemd_when_unit_set(tmp_path):
    cfg = load_toml(tmp_path, '[realm]\nunit = "ac-worldserver"\n')
    assert cfg.resolved_status_check() == "systemd"


def test_bad_bridge_type_refused(tmp_path):
    with pytest.raises(tdconfig.ConfigError):
        load_toml(tmp_path, '[bridge]\ntype = "telnet"\n')


def test_bad_status_check_refused(tmp_path):
    with pytest.raises(tdconfig.ConfigError):
        load_toml(tmp_path, '[realm]\nstatus_check = "psychic"\n')


def test_min_gmlevel_zero_refused(tmp_path):
    """gmlevel 0 would admit every player account — a config typo must not
    fail open."""
    with pytest.raises(tdconfig.ConfigError):
        load_toml(tmp_path, "[auth]\nmin_gmlevel = 0\n")


def test_empty_allowed_nets_refused(tmp_path):
    with pytest.raises(tdconfig.ConfigError):
        load_toml(tmp_path, "[server]\nallowed_nets = []\n")


def test_soap_pass_from_env(tmp_path, monkeypatch):
    cfg = load_toml(tmp_path, '[bridge]\ntype = "soap"\nsoap_user = "b"\n')
    monkeypatch.setenv("TESTDECK_SOAP_PASS", "sekrit")
    assert cfg.resolved_soap_pass() == "sekrit"


def test_validate_flags_missing_soap_creds(tmp_path):
    cfg = load_toml(tmp_path, '[bridge]\ntype = "soap"\n')
    tdconfig.validate(cfg, check_privileges=False)
    keys = {p.key for p in cfg.problems if p.level == "error"}
    assert "bridge" in keys


def test_validate_missing_dist_is_a_problem(tmp_path):
    cfg = load_toml(tmp_path, f'[paths]\nbase = "{tmp_path}"\n')
    tdconfig.validate(cfg, check_privileges=False)
    assert any(p.key == "frontend" for p in cfg.problems)
    assert any(p.key == "paths" for p in cfg.problems)


def test_config_search_env(tmp_path, monkeypatch):
    p = tmp_path / "elsewhere.toml"
    p.write_text("[server]\nport = 9999\n")
    monkeypatch.setenv("TESTDECK_CONFIG", str(p))
    cfg = tdconfig.load(app_dir=tmp_path / "app")
    assert cfg.port == 9999

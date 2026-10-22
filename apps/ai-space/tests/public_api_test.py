"""Exercise the actual production executable, without test-only D-Bus controls."""
import ast
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

binary = Path(sys.argv[1]).resolve()
service = "io.github.xikario.AiSpace"
with tempfile.TemporaryDirectory(prefix="ai-space-public-api-") as directory:
    home = Path(directory)
    env = dict(
        os.environ,
        HOME=str(home),
        XDG_CONFIG_HOME=str(home / "config"),
        XDG_CACHE_HOME=str(home / "cache"),
        XDG_DATA_HOME=str(home / "data"),
        PYTHONPATH="",
        PYTHONNOUSERSITE="1",
    )
    # Even enabling legacy test environment flags must not export test controls.
    env["AI_SPACE_TEST_NO_BOOTSTRAP"] = "1"
    preferences = home / "config/xikario"
    preferences.mkdir(parents=True)
    old_settings = preferences / "ai-space.conf"
    old_settings.write_text(
        "[migration]\nlegacyPreferencesV1=true\n"
        "[smartSpace]\nresultSortMode=2\n"
        "[compatibility]\nkeep=old-native-format\n",
        encoding="utf-8",
    )
    original_settings = old_settings.read_bytes()
    process = subprocess.Popen(
        [str(binary), "--show"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
    )

    def call(method, *arguments, check=True):
        return subprocess.run(
            [
                "gdbus",
                "call",
                "--session",
                "--dest",
                service,
                "--object-path",
                "/aiSpace",
                "--method",
                service + "." + method,
                *arguments,
            ],
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=check,
        )

    try:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            result = call("status", check=False)
            if result.returncode == 0:
                state = json.loads(ast.literal_eval(result.stdout)[0])
                if state.get("ready"):
                    break
            time.sleep(0.05)
        else:
            raise AssertionError("production app did not become ready")
        xml = subprocess.check_output(
            [
                "gdbus",
                "introspect",
                "--session",
                "--dest",
                service,
                "--object-path",
                "/aiSpace",
                "--xml",
            ],
            env=env,
            text=True,
        )
        interface = ET.fromstring(xml).find("interface[@name='%s']" % service)
        allowed = {"showWindow", "hideWindow", "toggleWindow", "showSettings", "quit", "status"}
        assert {row.attrib["name"] for row in interface.findall("method")} == allowed, xml
        for method, arguments in [
            ("startFullIndex", ()),
            ("excludeFolder", ("/tmp/example",)),
            ("setAutoStart", ("true",)),
            ("search", ("example",)),
            ("setGlassOptics", ("true",)),
            ("hideToEdge", ()),
        ]:
            result = call(method, *arguments, check=False)
            assert result.returncode != 0 and "UnknownMethod" in result.stderr, result
        retired = {
            "glassOptics",
            "edgeHidden",
            "materialBuilds",
            "glassBackend",
            "usedGpu",
            "active",
        }
        assert not retired.intersection(state), state
        assert state["statusVersion"] == 1 and not state["indexBusy"]
        ini_settings = preferences / "ai-space.ini"
        assert ini_settings.is_file()
        assert "keep=old-native-format" in ini_settings.read_text()
        assert old_settings.read_bytes() == original_settings
        version = (
            subprocess.check_output([str(binary), "--version"], env=env, text=True)
            .strip()
            .split()[-1]
        )
        assert state["version"] == version
        for flag in ("--hide", "--show", "--settings"):
            subprocess.run([str(binary), flag], env=env, check=True, timeout=10)
        again = json.loads(ast.literal_eval(call("status").stdout)[0])
        assert again["pid"] == state["pid"]
        # The checkout may remain present, but no compiled source-script fallback exists.
        checkout = Path(__file__).resolve().parents[3]
        assert str(checkout / "apps/ai-space/scripts").encode() not in binary.read_bytes()
        subprocess.run([str(binary), "--quit"], env=env, check=True, timeout=10)
        assert process.wait(timeout=10) == 0
        # Existing canonical settings always win over a stale legacy file.
        ini_settings.write_text(
            ini_settings.read_text().replace("old-native-format", "canonical-format")
        )
        process = subprocess.Popen(
            [str(binary), "--hide"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
        )
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            result = call("status", check=False)
            if result.returncode == 0 and json.loads(ast.literal_eval(result.stdout)[0]).get(
                "ready"
            ):
                break
            time.sleep(0.05)
        else:
            raise AssertionError("canonical settings restart did not become ready")
        subprocess.run([str(binary), "--quit"], env=env, check=True, timeout=10)
        assert process.wait(timeout=10) == 0
        assert "keep=canonical-format" in ini_settings.read_text()
        assert old_settings.read_bytes() == original_settings
        print(
            "PASS: production D-Bus allowlist, retired method rejection, single instance, version and relocatable runtime"
        )
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)

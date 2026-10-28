"""The Amlogic profiler must remain an explicitly opt-in NG diagnostic."""
import unittest
from pathlib import Path
from xml.etree import ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


class ProfileSettingTests(unittest.TestCase):
    def test_profile_toggle_is_off_by_default(self):
        root = ET.parse(ROOT / "resources/settings.xml").getroot()
        matching = [setting for setting in root.iter("setting")
                    if setting.get("id") == "aml_profile"]
        self.assertEqual(len(matching), 1)
        self.assertEqual(matching[0].findtext("default"), "false")
        self.assertEqual(matching[0].get("type"), "boolean")

    def test_tuning_probe_is_opt_in_and_hevc_tuning_has_no_user_gate(self):
        root = ET.parse(ROOT / "resources/settings.xml").getroot()
        settings = [item for item in root.iter("setting")
                    if item.get("id") == "aml_tuning_probe"]
        self.assertEqual(len(settings), 1)
        self.assertEqual(settings[0].get("type"), "boolean")
        self.assertEqual(settings[0].findtext("default"), "false")
        self.assertFalse(any(
            item.get("id") == "aml_hevc_tuning"
            for item in root.iter("setting")))

        source = (ROOT / "resources/lib/util/moonlighthelper.py").read_text()
        self.assertIn(
            "('aml_tuning_probe', 'MOONLIGHT_AML_TUNING_PROBE')", source)
        self.assertNotIn("aml_hevc_tuning", source)
        self.assertNotIn("MOONLIGHT_AML_HEVC_TUNING", source)

    def test_frame_pacing_defaults_to_latency_with_only_two_explicit_paths(self):
        root = ET.parse(ROOT / "resources/settings.xml").getroot()
        entries = [entry for entry in root.iter("setting")
                   if entry.get("id") == "frame_pacing"]
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0].get("type"), "string")
        self.assertEqual(entries[0].findtext("default"), "latency")
        values = [option.text for option in
                  entries[0].find("constraints/options").findall("option")]
        self.assertEqual(values, ["latency", "balanced"])
        self.assertFalse(any(
            entry.get("id") == "aml_vfm" for entry in root.iter("setting")))

        source = (ROOT / "resources/lib/util/moonlighthelper.py").read_text()
        self.assertIn("env['MOONLIGHT_FRAME_PACING'] = frame_pacing", source)
        self.assertNotIn("MOONLIGHT_AML_VFM", source)
        self.assertNotIn("getSettingBool('aml_vfm')", source)

    def test_cpu_performance_governor_is_off_by_default(self):
        root = ET.parse(ROOT / "resources/settings.xml").getroot()
        matching = [setting for setting in root.iter("setting")
                    if setting.get("id") == "cpu_performance_governor"]
        self.assertEqual(len(matching), 1)
        self.assertEqual(matching[0].get("type"), "boolean")
        self.assertEqual(matching[0].findtext("default"), "false")

        source = (ROOT / "resources/lib/util/moonlighthelper.py").read_text()
        self.assertIn("getSettingBool('cpu_performance_governor')", source)
        self.assertIn("env['MOONLIGHT_CPU_PERFORMANCE'] = '1'", source)
        self.assertIn("env.pop('MOONLIGHT_CPU_PERFORMANCE', None)", source)

    def test_alsa_resync_is_opt_in_and_only_in_stream_environment(self):
        root = ET.parse(ROOT / "resources/settings.xml").getroot()
        matching = [setting for setting in root.iter("setting")
                    if setting.get("id") == "alsa_resync"]
        self.assertEqual(len(matching), 1)
        self.assertEqual(matching[0].get("type"), "boolean")
        self.assertEqual(matching[0].findtext("default"), "false")
        source = (ROOT / "resources/lib/util/moonlighthelper.py").read_text()
        self.assertIn("getSettingBool('alsa_resync')", source)
        self.assertIn("env['MOONLIGHT_ALSA_RESYNC'] = '1'", source)
        self.assertIn("env.pop('MOONLIGHT_ALSA_RESYNC', None)", source)
        self.assertIn("subprocess.Popen([scripts_path + 'launch.sh'", source)

    def test_input_handoff_is_disabled_by_default(self):
        root = ET.parse(ROOT / "resources/settings.xml").getroot()
        for setting_id in ("input_handoff_probe", "input_recovery"):
            entries = [entry for entry in root.iter("setting")
                       if entry.get("id") == setting_id]
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0].findtext("default"), "false")
        source = (ROOT / "resources/lib/util/moonlighthelper.py").read_text()
        for pair in (
                "('input_handoff_probe', 'MOONLIGHT_INPUT_HANDOFF_PROBE')",
                "('input_recovery', 'MOONLIGHT_INPUT_RECOVERY')"):
            self.assertIn(pair, source)
        self.assertIn("env.pop(variable, None)", source)

    def test_environment_is_applied_only_to_stream_launch(self):
        source = (ROOT / "resources/lib/util/moonlighthelper.py").read_text()
        self.assertIn("getSettingBool('aml_profile')", source)
        self.assertIn("env['MOONLIGHT_AML_PROFILE'] = '1'", source)
        self.assertIn("env.pop('MOONLIGHT_AML_PROFILE', None)", source)
        self.assertIn("subprocess.Popen([scripts_path + 'launch.sh'", source)


if __name__ == "__main__":
    unittest.main()

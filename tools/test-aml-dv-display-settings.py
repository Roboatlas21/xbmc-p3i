#!/usr/bin/env python3
"""Compile the production DV-off and HDMI settings functions with fake sysfs.

Checks live chroma/depth restoration, the required DV-off operation order,
skip/no-mode-change paths, and preservation of resolution.ini persistence.
The only production text substitution redirects /flash to a temporary folder.
This exercises C++ control flow, not HDMI hardware or transition timing.
Requires a C++17 compiler; CXX may include arguments (for example, 'zig c++').
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

static int checks = 0, failures = 0, statCalls = 0, mountCalls = 0;
static int existingMode = 2, selectedCS = 0, selectedDepth = 0, probedCS = -1;
static bool prefer12 = false, supports12 = false, nativeGui = true;
static std::vector<std::string> events;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
  std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
constexpr unsigned int DOLBY_VISION_OUTPUT_MODE_BYPASS = 5;
constexpr unsigned int FLAG_FORCE_RGB_OUTPUT = 1, FLAG_FORCE_DOVI_LL = 2;
constexpr unsigned int FLAG_TOGGLE_FRAME = 0x80000000u;
constexpr int DOLBY_VISION_LL_DISABLE = 0, DOLBY_VISION_FOLLOW_SOURCE = 0;
constexpr int DOLBY_VISION_FORCE_OUTPUT_MODE = 2, LOGDEBUG = 0;
constexpr unsigned long MS_RDONLY = 1, MS_NOATIME = 2, MS_REMOUNT = 4;
struct statfs { unsigned long f_flags; };
int statfs(const char*, struct statfs* fs) {
  ++statCalls; fs->f_flags = MS_RDONLY; return 0;
}
int mount(const char*, const char*, const char*, unsigned long, const void*) {
  ++mountCalls; return 0;
}
struct RESOLUTION_INFO { std::string strId; float fRefreshRate; };
struct CSettings {
  enum { SETTING_COREELEC_AMLOGIC_FORCE_CS, SETTING_COREELEC_AMLOGIC_LIMIT_CD,
         SETTING_COREELEC_AMLOGIC_PREFER_12BIT, SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING };
  int GetInt(int id) const { return id == SETTING_COREELEC_AMLOGIC_FORCE_CS ? selectedCS : selectedDepth; }
  bool GetBool(int id) const { return id == SETTING_COREELEC_AMLOGIC_PREFER_12BIT ? prefer12 : nativeGui; }
};
struct SettingsComponent {
  std::shared_ptr<CSettings> GetSettings() {
    static auto settings = std::make_shared<CSettings>(); return settings;
  }
};
struct CServiceBroker {
  static SettingsComponent* GetSettingsComponent() { static SettingsComponent c; return &c; }
};
bool aml_display_support_12bit(int cs) { probedCS = cs; return supports12; }
struct CLog { template<class... Args> static void Log(Args&&...) {} };
struct CDVCoreGuard { explicit CDVCoreGuard(const char*) {} };
struct CSysfsPath {
  std::string path;
  explicit CSysfsPath(std::string p) : path(std::move(p)) {}
  template<class T> CSysfsPath(std::string p, const T& value) : path(std::move(p)) { Set(value); }
  bool Exists() const { return true; }
  template<class T> std::optional<T> Get() const {
    if constexpr (std::is_same_v<T, std::string>) return std::string("test-formats");
    else return static_cast<T>(path.find("dolby_vision_mode") != std::string::npos ? existingMode : 0);
  }
  template<class T> void Set(const T& value) {
    std::ostringstream text; text << value;
    if (path == "/sys/class/amhdmitx/amhdmitx0/attr") events.push_back("attr=" + text.str());
    if (path == "/sys/module/amdolby_vision/parameters/dolby_vision_enable")
      events.push_back("enable=" + text.str());
  }
};
static std::shared_ptr<const void> s_dvPlaybackSession;
static unsigned int s_dvModeCached;
static std::atomic<unsigned int> s_guiDvOutputMode{0};
static bool aml_linux_force_422 = true;
void aml_dv_backend_invalidate(const std::shared_ptr<const void>&) {}
void aml_dv_detect_active_area_stop() {}
void aml_dv_auto_letterbox_watch_stop() {}
unsigned int aml_dv_output_mode_to_string(unsigned int mode) { return mode; }
void aml_dv_toggle_frame(unsigned int mode) { CHECK(mode == DOLBY_VISION_OUTPUT_MODE_BYPASS); events.push_back("toggle"); }
void aml_dv_reset_l5_signals() {}
void aml_dv_display_auto_now() { events.push_back("auto"); }
void aml_display_mode_round_trip(const char*) { events.push_back("reset"); }
void aml_dv_dump_state(const char*) { events.push_back("dump"); }
'''

CASES = r'''
static std::string readConfig() {
  std::ifstream input("resolution.ini");
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
static void clearTrace() { events.clear(); statCalls = mountCalls = 0; probedCS = -1; }
int main() {
  struct ColorCase { int cs, depth; bool prefer, supported; const char* expected; };
  const ColorCase cases[] = {
    {0, 0, false, false, ","}, {0, 1, false, false, ",8bit"},
    {0, 2, false, false, ",10bit"}, {0, 3, false, false, ",12bit"},
    {0, 4, false, false, ",16bit"}, {1, 2, false, false, ",rgb,10bit"},
    {2, 1, false, false, ",420,8bit"}, {3, 4, false, false, ",422,16bit"},
    {4, 0, false, false, ",444,"}, {0, 1, true, true, ",422,12bit"},
    {1, 1, true, true, ",rgb,12bit"}, {2, 2, true, true, ",420,12bit"},
    {3, 4, true, true, ",422,12bit"}, {4, 1, true, true, ",444,12bit"},
    {0, 2, true, false, ",10bit"}, {2, 2, true, false, ",420,10bit"},
    {4, 1, true, false, ",444,8bit"}, {0, 1, false, true, ",8bit"}
  };
  for (const auto& c : cases) {
    selectedCS = c.cs; selectedDepth = c.depth; prefer12 = c.prefer; supports12 = c.supported;
    clearTrace();
    aml_apply_hdmi_color_attributes();
    aml_apply_hdmi_color_attributes(); // An unchanged setting still needs a live write.
    const std::string expected = std::string("attr=") + c.expected;
    CHECK((events == std::vector<std::string>{expected, expected}));
    CHECK(statCalls == 0 && mountCalls == 0);
    CHECK(probedCS == (prefer12 ? selectedCS : -1));

    // The persistent path must emit the identical attributes and retain its boot fields.
    clearTrace();
    CHECK(write_resolution_ini({"2160p30hz", 30.0f}));
    CHECK(statCalls == 1 && mountCalls == 2);
    CHECK((events == std::vector<std::string>{expected}));
    const auto config = readConfig();
    CHECK(config.find("kernel_hdmimode=2160p30hz\n") != std::string::npos);
    CHECK(config.find("frac_rate_policy=0\n") != std::string::npos);
    CHECK(config.find("native_4k_gui=1\n") != std::string::npos);
    CHECK(config.find(std::string("hdmitx=") + c.expected + "\n") != std::string::npos);
    CHECK(config.find("allfmt_names=test-formats\n") != std::string::npos);
  }
  nativeGui = false;
  CHECK(write_resolution_ini({"1080p24hz", 23.976f}));
  CHECK(readConfig().find("frac_rate_policy=1\n") != std::string::npos);
  CHECK(readConfig().find("native_4k_gui=0\n") != std::string::npos);

  selectedCS = 0; selectedDepth = 1; prefer12 = supports12 = true;
  for (int mode : {0, 1, 2, 3, 4, 5}) {
    for (bool skip : {false, true}) {
      // An unrelated sentinel proves DV-off does not touch the boot file at all.
      { std::ofstream sentinel("resolution.ini"); sentinel << "unchanged boot configuration\n"; }
      existingMode = mode; clearTrace();
      aml_dv_off(skip);
      std::vector<std::string> expected;
      if (mode != 5) expected.push_back("toggle");
      expected.push_back("enable=N");
      if (mode != 5 && !skip) {
        expected.push_back("auto"); expected.push_back("attr=,422,12bit");
        expected.push_back("reset");
      }
      expected.push_back("dump");
      CHECK(events == expected);
      CHECK(statCalls == 0 && mountCalls == 0);
      CHECK(readConfig() == "unchanged boot configuration\n");
      CHECK(s_dvModeCached == 5 && s_guiDvOutputMode.load() == 5);
      CHECK(!aml_linux_force_422);
    }
  }
  std::printf("DV display settings: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    settings = (ROOT / 'xbmc/settings/DisplaySettings.cpp').read_text(encoding='utf-8')
    aml = (ROOT / 'xbmc/utils/AMLUtils.cpp').read_text(encoding='utf-8')
    production = '\n'.join(function(settings, signature) for signature in (
        'std::string GetHDMIColorAttributes()', 'void aml_apply_hdmi_color_attributes()',
        'bool write_resolution_ini(RESOLUTION_INFO res)'))
    production = production.replace('"/flash"', '"."')
    off = function(aml, 'void aml_dv_off(bool skip_hdmi_update)')
    variants = [('production', off, True)]
    if args.negative_controls:
        variants.extend([
            ('flash-rewrite', off.replace('aml_apply_hdmi_color_attributes();',
                                         'write_resolution_ini({"2160p24hz", 24.0f});'), False),
            ('missing-live-write', off.replace('aml_apply_hdmi_color_attributes();', ''), False),
            ('missing-vpp-reset', off.replace('aml_display_mode_round_trip(__FUNCTION__);', ''), False),
        ])
    compiler = shlex.split(os.environ.get('CXX', 'g++'))
    with tempfile.TemporaryDirectory(prefix='aml-dv-display-settings-') as tmp:
        folder = Path(tmp)
        for name, body, should_pass in variants:
            cpp = folder / (name + '.cpp')
            exe = folder / (name + ('.exe' if os.name == 'nt' else ''))
            cpp.write_text(PRELUDE + production + body + CASES, encoding='utf-8')
            subprocess.run(compiler + ['-std=c++17', '-Wall', '-Wextra', '-Werror',
                                      str(cpp), '-o', str(exe)], check=True, timeout=180)
            result = subprocess.run([str(exe)], cwd=folder, text=True, capture_output=True, timeout=15)
            if (result.returncode == 0) != should_pass:
                raise SystemExit(f'{name}: unexpected result\n{result.stdout}\n{result.stderr}')
            if should_pass:
                print(result.stdout.strip())
            else:
                print(f'Negative control {name}: rejected as expected')


if __name__ == '__main__':
    main()

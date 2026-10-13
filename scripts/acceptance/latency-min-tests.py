"""claude/latency-min-20261013 acceptance: real windowed and borderless-fullscreen swapchains
for each display-sync mode x low-queue setting, checking the actual Present arguments.

Expected after restoring Automatic (2.0.4 semantics):
  Tearing (0)   -> Present(0, DXGI_PRESENT_ALLOW_TEARING)
  Vsync (1)     -> Present(1, 0)
  Automatic (2) -> Present(0, 0)  (newest frame replaces an unshown one; never tears)

usage: python -X utf8 -B -I scripts/acceptance/latency-min-tests.py <app dir> <log dir> [display-sync|lifecycle]
"""
import re, subprocess, sys, time, json, os
from pathlib import Path

app, logs = Path(sys.argv[1]), Path(sys.argv[2]); case = sys.argv[3] if len(sys.argv) > 3 else 'display-sync'
logs.mkdir(parents=True, exist_ok=True)
env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('VEYRA_', 'QT_', 'QSG_', 'QML_'))}
tmp = logs / ('tmp-' + case); tmp.mkdir(exist_ok=True); env.update(TEMP=str(tmp), TMP=str(tmp))
running = [l for l in subprocess.check_output(['tasklist', '/fo', 'csv', '/nh'], text=True, encoding='mbcs', errors='replace').splitlines() if l.lower().startswith('"veyra')]
assert not running, 'another Veyra process is running: ' + repr(running)
if case == 'display-sync':
    command = [str(app / 'veyra_fg_presentation_tests.exe'), str(tmp / 'pixels'), str(app / 'runtime/experimental'), 'display-sync']
elif case == 'lifecycle':
    command = [str(app / 'veyra_present_sink_lifecycle_tests.exe'), str(tmp / 'lifecycle')]
else:
    raise SystemExit('unknown case ' + case)
log = logs / (case + '-' + time.strftime('%H%M%S') + '.log'); begin = time.monotonic()
with log.open('xb') as console:
    try: rc = subprocess.run(command, cwd=app, env=env, stdout=console, stderr=subprocess.STDOUT, timeout=290).returncode
    except subprocess.TimeoutExpired: rc = 124
body = log.read_text(encoding='utf8', errors='replace')
errors = [l for l in body.splitlines() if 'D3D12 ERROR' in l or 'D3D12 CORRUPTION' in l]
result = dict(case=case, exit=rc, seconds=round(time.monotonic() - begin, 1), log=str(log), gpuErrors=errors)
passed = rc == 0 and not errors
if case == 'display-sync':
    expected = {'0': (0, 0x200), '1': (1, 0), '2': (0, 0)}
    sections = re.findall(r'DISPLAY_SYNC_BEGIN full=(\d) lowQueue=(\d) mode=(\d)(.*?)DISPLAY_SYNC_END full=\d lowQueue=\d mode=\d pass=(\d)', body, re.S)
    rows = []
    for full, low, mode, section, ok in sections:
        calls = re.findall(r'\[present-contract\] backend=DXGI sync=(\d) flags=0x([0-9A-F]+) hr=0x([0-9A-F]+)', section)
        good = ok == '1' and bool(calls) and all((int(s), int(f, 16)) == expected[mode] and int(h, 16) == 0 for s, f, h in calls)
        rows.append(dict(full=full, lowQueue=low, mode=mode, calls=sorted(set(calls)), passed=good)); passed = passed and good
    passed = passed and len(sections) == 12
    result['sections'] = rows
result['passed'] = passed
(logs / (case + '.json')).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf8')
print(json.dumps(result, ensure_ascii=False, indent=1)[:3000]); raise SystemExit(0 if passed else 1)

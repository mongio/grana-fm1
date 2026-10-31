#!/usr/bin/env python3
"""web/grana-editor.html: the editor and simulator as one page, with everything it needs inside it.

  python3 web/make_page.py VERSION PRODUCT     (after ./build.sh --release VERSION and web/build_wasm.sh)

Inside: the simulator (web/grana.wasm), GRANA's code without a bank (build/grana-app.bin), the default bank,
the update loader (build/loader/ota.bin), three files of the JieLi AC79 SDK ($AC79_SDK, Apache-2.0), the
package builder (web/fwsc_make.js) and Felucca's USB update (web/fm1ota.js)."""
import base64, os, sys
from pathlib import Path
W = Path(__file__).resolve().parent
R = W.parent
ver, product = sys.argv[1], sys.argv[2]
product_bt = sys.argv[3] if len(sys.argv) > 3 else product   # GRANA + Bluetooth (build/grana-app-bt.bin)
sdk = Path(os.environ.get("AC79_SDK", Path.home() / "fw-AC79_AIoT_SDK")) / "cpu" / "wl82" / "tools"
b64 = lambda p: base64.b64encode(Path(p).read_bytes()).decode()
ota_js = (W / "fm1ota.js").read_text().replace("export function", "function").replace("export class", "class")
html = (W / "index.template.html").read_text()
import json
kit = {str(f.relative_to(R / "rescue")): b64(f) for f in sorted((R / "rescue").rglob("*"))
       if f.is_file() and "__pycache__" not in f.parts}          # the first-aid kit (rescue/): built into a zip by the page
for k, v in {"__WASM_B64__": b64(W / "grana.wasm"), "__APP_B64__": b64(R / "build" / "grana-app.bin"), "__APP_BT_B64__": b64(R / "build" / ("grana-app-bt.bin" if (R / "build" / "grana-app-bt.bin").exists() else "grana-app.bin")),
             "__BANK_B64__": b64(R / "firmware" / "gen" / "bank_default.bin"), "__OTA_B64__": b64(R / "build" / "loader" / "ota.bin"),
             "__UBOOT_B64__": b64(sdk / "uboot.boot"), "__CFGTOOL_B64__": b64(sdk / "cfg_tool.bin"), "__EQ_B64__": b64(sdk / "cfg" / "eq_cfg_hw.bin"),
             "__FWSC_JS__": (W / "fwsc_make.js").read_text(), "__OTA_JS__": ota_js,
             "__KIT_JSON__": json.dumps(kit), "__PRODUCT_BT__": product_bt, "__PRODUCT__": product, "__VERSION__": ver}.items():
    html = html.replace(k, v)
assert "__" not in "".join(t for t in ("__WASM_B64__", "__APP_B64__") if t in html)
(W / "grana-editor.html").write_text(html)
print("web/grana-editor.html", len(html), "B")

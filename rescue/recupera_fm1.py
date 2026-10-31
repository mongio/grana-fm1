#!/usr/bin/env python3
"""recupera_fm1.py - pronto soccorso per l'M-VAVE FM-1 rimasto in WL80UBOOT (schermo nero, USB 4C4A:8057).

    sudo python3 recupera_fm1.py [PACCHETTO.fwsc]

Su un Raspberry Pi o un PC con Linux, con l'FM-1 collegato via USB direttamente. In un'unica sessione:
  1. controlla che i file del kit siano quelli giusti (SHA256SUMS);
  2. aspetta l'FM-1 in modalita' UBOOT, carica il programma di servizio di JieLi (jl-uboot-tool) e controlla il chip;
  3. legge due volte tutta la memoria e salva la copia in copie/ con data e ora;
  4. controlla il pacchetto (GRANA, oppure il firmware ufficiale M-VAVE V15 riconosciuto dall'impronta) e la zona di
     avvio dell'FM-1;
  5. chiede conferma, riscrive solo i settori diversi della zona del programma (0x4000 .. 0x93000), ognuno riletto;
  6. rilegge tutta la memoria e la confronta con quella attesa.
La zona di avvio (sotto 0x4000) e quella dei dati (da 0x93000) non vengono mai scritte.
Ogni passo finisce anche nel registro in copie/. Usa jl-uboot-tool di kagaimiq (MIT) e il formato dei pacchetti di
Felucca (GPL-3.0); fa parte di Mario Texture's GRANA."""

import hashlib
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
JLT = os.path.join(HERE, "jl-uboot-tool")
COPIE = os.path.join(HERE, "copie")
FLASH, APP_LO, APP_HI, SEC = 0x100000, 0x4000, 0x93000, 0x1000
STOCK_V15_SHA256 = "db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a"
STOCK_V15_SIZE = 699956
CHIP_KEY, FLASH_ID = 0x980F, 0x856014
STAMP = time.strftime("%Y%m%d-%H%M%S")
_log = None


def say(*a):
    s = " ".join(str(x) for x in a)
    print(s, flush=True)
    if _log:
        _log.write(s + "\n")
        _log.flush()


def stop(msg):
    say("")
    say("FERMO. " + msg)
    say("Non e' stato scritto niente di nuovo dopo questo punto. Il registro e' in copie/registro-" + STAMP + ".txt")
    sys.exit(1)


def ask(question):
    try:
        return input(question).strip()
    except EOFError:
        return ""


# ---- 0. root, il registro, le impronte dei file del kit
def check_kit():
    sums = os.path.join(HERE, "SHA256SUMS")
    if not os.path.exists(sums):
        say("(nessun SHA256SUMS: salto il controllo dei file del kit)")
        return
    bad = []
    for line in open(sums, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        want, name = line.split(None, 1)
        name = name.lstrip("*")
        p = os.path.join(HERE, name)
        if not os.path.exists(p):
            bad.append(name + " (manca)")
        elif hashlib.sha256(open(p, "rb").read()).hexdigest() != want:
            bad.append(name + " (diverso)")
    if bad:
        stop("Alcuni file del kit non sono quelli giusti: " + ", ".join(bad) +
             ". Riscarica il kit dall'editor di GRANA (a volte il browser scambia o tronca i file).")
    say("File del kit: tutti integri.")


# ---- 1. le librerie di jl-uboot-tool
def need_libs():
    try:
        import crcmod, yaml, tqdm  # noqa: F401
        import Cryptodome  # noqa: F401
        return
    except ImportError:
        pass
    say("Mancano alcune librerie (python3-crcmod, python3-pycryptodome, python3-tqdm, python3-yaml).")
    if ask("Le installo adesso con apt (serve internet)? Scrivi SI: ").upper() != "SI":
        stop("Installale con:  sudo apt install -y python3-crcmod python3-pycryptodome python3-tqdm python3-yaml")
    r = subprocess.call(["apt-get", "install", "-y", "python3-crcmod", "python3-pycryptodome", "python3-tqdm", "python3-yaml"])
    if r != 0:
        stop("L'installazione delle librerie non e' riuscita (internet? prova prima: sudo apt update).")
    os.execv(sys.executable, [sys.executable] + sys.argv)


# ---- 2. il pacchetto
def pick_package(arg):
    if arg:
        return arg
    d = os.path.join(HERE, "pacchetti")
    pk = sorted((f for f in os.listdir(d) if f.endswith(".fwsc")), key=lambda f: os.path.getmtime(os.path.join(d, f))) if os.path.isdir(d) else []
    if not pk:
        stop("Non trovo un pacchetto .fwsc: mettilo nella cartella pacchetti/ o scrivilo dopo il comando.")
    if len(pk) > 1:
        say("Nella cartella pacchetti/ ci sono piu' pacchetti:")
        for i, f in enumerate(pk, 1):
            say(f"  {i}. {f}")
        n = ask(f"Quale uso? (1-{len(pk)}, Invio = l'ultimo): ")
        f = pk[int(n) - 1] if n.isdigit() and 1 <= int(n) <= len(pk) else pk[-1]
    else:
        f = pk[0]
    return os.path.join(d, f)


def load_package(path):
    from fm1fw import Firmware
    data = open(path, "rb").read()
    stock = len(data) == STOCK_V15_SIZE and hashlib.sha256(data).hexdigest() == STOCK_V15_SHA256
    try:
        fw = Firmware(data)
    except Exception as e:
        if b"FM-1" in data[:0x400] or len(data) > 700000:
            stop(f"{os.path.basename(path)} non si legge come pacchetto GRANA/V15 ({e}). Se e' il firmware "
                 "ufficiale V16 di M-VAVE: ha una disposizione della memoria diversa e questo strumento non lo usa. "
                 "Usa un pacchetto di GRANA (lo crea l'editor) o la V15 originale.")
        stop(f"{os.path.basename(path)} non e' un pacchetto dell'FM-1 valido ({e}).")
    if not stock and not fw.product.startswith("FM-1_95"):
        stop(f"{os.path.basename(path)} e' '{fw.product}': non e' ne' GRANA ne' il firmware ufficiale V15 non modificato. Non lo uso.")
    e = next(e for e in fw.entries if e["type"] == 0)
    img = fw.raw[e["data_off"] + fw.skew:e["data_off"] + fw.skew + e["size"]]
    if len(img) != APP_HI:
        stop(f"Immagine di {len(img):#x} byte invece di {APP_HI:#x}: disposizione della memoria non prevista.")
    kind = "firmware ufficiale M-VAVE V15" if stock else f"GRANA ({fw.product})"
    say(f"Pacchetto: {os.path.basename(path)} -> {kind}, integro.")
    return img, stock


# ---- 3. l'FM-1
def find_fm1(wait_s=180):
    from jldevfind import find_jl_devices
    try:
        subprocess.call(["modprobe", "sg"], stderr=subprocess.DEVNULL)   # /dev/sg*: il modo di Linux per parlargli
    except OSError:
        pass
    say("")
    say("Cerco l'FM-1 in modalita' UBOOT. Se non e' ancora collegato:")
    say("  spegnilo dall'interruttore, collegalo con il cavo a una porta USB di questo computer (senza hub), poi accendilo.")
    t0 = time.time()
    dots = 0
    while time.time() - t0 < wait_s:
        devs = find_jl_devices(venfilter="wl82")
        if devs:
            say("")
            say("Trovato: " + devs[0]["name"])
            return devs[0]["path"]
        print(".", end="", flush=True)
        dots += 1
        time.sleep(1)
    say("")
    try:
        lsusb = subprocess.run(["lsusb"], capture_output=True, text=True).stdout
    except OSError:
        lsusb = ""
    if "4c4a:8057" in lsusb.lower():
        stop("Il computer vede l'FM-1 (4c4a:8057) ma non riesco ad aprirlo: hai avviato con sudo?")
    stop("Non trovo l'FM-1 in modalita' UBOOT. Controlla cavo e porta, spegnilo, riaccendilo collegato e riprova. "
         "Se l'FM-1 si avvia normalmente (o resta nero ma il computer non lo vede come 4c4a:8057) questo non e' il caso giusto.")


def open_fm1(path):
    import yaml
    from jltech.uboot import JL_MSCDevice, JL_UBOOT, JL_LoaderV2
    dev = JL_MSCDevice(path)
    vendor, product, _rev = dev.inquiry()
    say("")
    if vendor.lower() != "wl82":
        stop(f"Il chip risponde '{vendor}', non 'WL82': non e' quello dell'FM-1.")
    if product == "UBOOT1.00":                 # il programma di servizio di JieLi, nella RAM del chip
        spec = yaml.safe_load(open(os.path.join(JLT, "data", "usb-loaders.yaml")))["wl82"]
        up = JL_UBOOT(dev.dev)
        addr = spec["address"]
        with open(os.path.join(JLT, "data", spec["file"]), "rb") as f:
            while True:
                block = f.read(512)
                if not block:
                    break
                up.mem_write(addr, block)       # wl82: il file e' gia' cifrato come lo vuole il chip
                addr += len(block)
        up.mem_jump(spec["address"], 0x0001)
        time.sleep(0.3)
    ld = JL_LoaderV2(dev.dev)
    key = ld.chip_key()
    od = ld.online_device()
    say(f"Chip WL82 (AC791N), chiave 0x{key:04X}, memoria 0x{od['id']:06X}")
    if key != CHIP_KEY or od["id"] != FLASH_ID:
        stop(f"Chiave o memoria inattese (attese 0x{CHIP_KEY:04X} e 0x{FLASH_ID:06X}): non e' un FM-1 come quelli conosciuti.")
    try:
        io = max(256, min(ld.usb_buffer_size(), 4096))
    except Exception:
        io = 512
    return dev, ld, io


def read_all(ld, io, label):
    from tqdm import tqdm
    out = bytearray()
    with tqdm(desc=label, total=FLASH, unit="B", unit_scale=True, unit_divisor=1024) as tq:
        a = 0
        while a < FLASH:
            n = min(io, FLASH - a)
            out += ld.flash_read(a, n)
            a += n
            tq.update(n)
    return bytes(out)


def head_ok(ref):
    from fm1fw import crc16, enc
    h = bytearray(ref[:32])
    enc(h, 0, 32)
    return crc16(h[2:32]) == (h[0] | h[1] << 8)


def write_sector(ld, io, a, data):
    ld.flash_erase_sector(a)
    o = 0
    while o < SEC:
        ld.flash_write(a + o, data[o:o + io])
        o += io
    back = b"".join(ld.flash_read(a + o, io) for o in range(0, SEC, io))
    return back == data


def main():
    global _log
    if os.geteuid() != 0:
        print("Va avviato con sudo:  sudo python3 recupera_fm1.py")
        sys.exit(1)
    os.makedirs(COPIE, exist_ok=True)
    _log = open(os.path.join(COPIE, f"registro-{STAMP}.txt"), "w", encoding="utf-8")
    sys.path.insert(0, HERE)
    sys.path.insert(0, JLT)
    say("=== Pronto soccorso FM-1 (Mario Texture's GRANA) ===   " + time.strftime("%d/%m/%Y %H:%M"))
    check_kit()
    need_libs()
    img, stock = load_package(pick_package(sys.argv[1] if len(sys.argv) > 1 else None))
    path = find_fm1()
    dev, ld, io = open_fm1(path)

    say("")
    say("Leggo tutta la memoria due volte (copia di sicurezza)...")
    r1 = read_all(ld, io, "Lettura 1")
    r2 = read_all(ld, io, "Lettura 2")
    tries = 0
    while r1 != r2 and tries < 2:
        say("Le due letture non coincidono: rileggo.")
        r1, r2 = r2, read_all(ld, io, "Lettura")
        tries += 1
    if r1 != r2:
        stop("Le letture della memoria non sono stabili: controlla il cavo (diretto, senza hub) e riprova.")
    if r1 in (b"\xff" * FLASH, b"\x00" * FLASH):
        stop("La memoria letta e' tutta uguale: lettura non valida.")
    bak = os.path.join(COPIE, f"fm1-{STAMP}.bin")
    open(bak, "wb").write(r1)
    say(f"Copia salvata: copie/{os.path.basename(bak)}  (sha256 {hashlib.sha256(r1).hexdigest()[:16]}...)")

    if not head_ok(r1):
        stop("L'intestazione della zona di avvio dell'FM-1 non torna: questo strumento non la tocca e non sa "
             "ripararla. Conserva la copia e chiedi aiuto.")
    if stock and r1[:APP_LO] != img[:APP_LO]:
        stop("La zona di avvio dell'FM-1 non coincide con quella del firmware V15: con la V15 non scrivo. "
             "Usa un pacchetto di GRANA.")
    say("Zona di avvio dell'FM-1: integra.")

    diff = [a for a in range(APP_LO, APP_HI, SEC) if img[a:a + SEC] != r1[a:a + SEC]]
    expected = r1[:APP_LO] + img[APP_LO:APP_HI] + r1[APP_HI:]
    if not diff:
        say("")
        say("La memoria contiene gia' esattamente questo pacchetto: non c'e' niente da scrivere.")
        say("Spegni l'FM-1, scollegalo e riaccendilo senza cavo.")
        return
    say("")
    say(f"Da riscrivere: {len(diff)} settori su {(APP_HI - APP_LO) // SEC} della zona del programma "
        f"({diff[0]:#07x} .. {diff[-1] + SEC:#07x}). La zona di avvio e i dati non vengono toccati.")
    if ask("Scrivo? Scrivi SI per continuare: ").upper() != "SI":
        stop("Annullato da te: la memoria non e' stata toccata.")

    from tqdm import tqdm
    failed = []
    with tqdm(desc="Scrittura", total=len(diff), unit="settori") as tq:
        for a in diff:
            ok = False
            for _ in range(3):
                if write_sector(ld, io, a, img[a:a + SEC]):
                    ok = True
                    break
            if not ok:
                failed.append(a)
            tq.update(1)
    if failed:
        stop("Alcuni settori non si sono scritti bene: " + " ".join(f"{a:#07x}" for a in failed) +
             ". NON spegnere l'FM-1: rilancia subito lo strumento, riprova da solo.")

    say("Rileggo tutto per la verifica finale...")
    fin = read_all(ld, io, "Verifica")
    open(os.path.join(COPIE, f"fm1-{STAMP}-dopo.bin"), "wb").write(fin)
    bad = [a for a in range(0, FLASH, SEC) if fin[a:a + SEC] != expected[a:a + SEC]]
    if bad:
        stop("La verifica finale non torna nei settori " + " ".join(f"{a:#07x}" for a in bad) +
             ". NON spegnere l'FM-1: rilancia lo strumento.")
    say("")
    say("FATTO: la memoria dell'FM-1 e' quella attesa, settore per settore.")
    say("Ora scollega l'FM-1, spegnilo e riaccendilo SENZA cavo:")
    say(" - se parte normalmente: e' recuperato;")
    say(" - se resta nero: sta aspettando di finire un vecchio aggiornamento. Collegalo al computer di sempre,")
    say("   apri l'editor di GRANA in Chrome e premi 'Installa sull'FM-1'.")
    try:
        dev.close()
    except Exception:
        pass


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        stop("Interrotto con Ctrl+C.")

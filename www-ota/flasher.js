// Web flasher: first USB flash of the OTA build, keeping the clock's settings.
// 1. read the clock's partition table and LittleFS over Web Serial and take config.json from it,
// 2. write the release (partition table, otadata, firmware, LittleFS) published next to this page,
// 3. leave config.json in the last 64 KB of the ota_1 slot; the firmware copies it to LittleFS on its first boot.
import { ESPLoader, Transport } from "./vendor/esptool-js-0.7.1.js"
import { findApp, findLittlefs, parsePartitions, readLittlefsFile, stagedConfigAddress, stagedConfigBlob } from "./flash-lib.js"

const $ = id => document.getElementById(id)
// 460800 rather than 921600: the clock's CH340 USB chip is more reliable at it
const BAUD = 460800
const RELEASE_FILES = ["partitions.bin", "boot_app0.bin", "firmware.bin", "littlefs.bin"]

let loader = null
let transport = null
let settings = null // Uint8Array of config.json
let release = null

const log = text => {
    const line = document.createElement("div")
    line.textContent = text
    $("log").append(line)
    $("log").scrollTop = $("log").scrollHeight
}
const terminal = { clean() {}, writeLine: t => { if (t.trim()) log(t) }, write: () => {} }
const progress = (fraction, label) => {
    $("progress").hidden = false
    $("progress").value = Math.round(fraction * 100)
    if (label) $("step").textContent = label
}

/**
 * Log "label: 40 % (400 / 1024 Ko, 85 Ko/s)" every 10 %.
 * @returns {(done: number) => void}
 */
function progressLog(label, total) {
    const start = performance.now()
    let next = 0
    return done => {
        const pct = Math.floor(done / total * 100)
        if (pct < next && done < total) return
        const seconds = Math.max((performance.now() - start) / 1000, 0.1)
        log(`${label} : ${pct} % (${Math.round(done / 1024)} / ${Math.round(total / 1024)} Ko, ${Math.round(done / 1024 / seconds)} Ko/s)`)
        next = pct - (pct % 10) + 10
    }
}

function showSettings(source) {
    let ssid = "?"
    try { ssid = JSON.parse(new TextDecoder().decode(settings)).ssid || "pas de Wi-Fi réglé" } catch (e) {
        settings = null
        $("settings").textContent = "Ce fichier n'est pas un fichier de réglages valide."
        return
    }
    $("settings").textContent = `Réglages ${source} : Wi-Fi « ${ssid} », ${settings.length} octets.`
    const url = URL.createObjectURL(new Blob([settings], { type: "application/json" }))
    $("backup").href = url
    $("backup").hidden = false
    updateButtons()
}

function updateButtons() {
    $("install").disabled = !(loader && release && (settings || $("nosettings").checked))
}

async function loadRelease() {
    const r = await fetch("firmware/release.json", { cache: "no-store" })
    if (!r.ok) throw new Error("Pas de release publiée à côté de cette page")
    release = await r.json()
    $("version").textContent = release.version
    $("install").textContent = `Installer la version ${release.version}`
}

async function connect() {
    if (!("serial" in navigator)) return
    $("connect").disabled = true
    try {
        const port = await navigator.serial.requestPort({})
        transport = new Transport(port, false)
        loader = new ESPLoader({ transport, baudrate: BAUD, terminal })
        progress(0, "Connexion à l'horloge…")
        const chip = await loader.main()
        log(`Connecté : ${chip}`)

        progress(0, "Lecture de la table de partitions…")
        const parts = parsePartitions(await loader.readFlash(0x8000, 0xc00))
        for (const p of parts) log(`  ${p.name.padEnd(8)} 0x${p.offset.toString(16)} ${p.size / 1024} Ko`)
        const fs = findLittlefs(parts)
        if (!fs) throw new Error("Aucune partition LittleFS sur l'horloge")

        progress(0, `Lecture des réglages (${fs.size / 1024} Ko)…`)
        log(`Lecture de la partition ${fs.name} (0x${fs.offset.toString(16)}, ${fs.size / 1024} Ko)`)
        const readLog = progressLog("Lecture", fs.size)
        const image = await loader.readFlash(fs.offset, fs.size, (_p, done, total) => { progress(done / total); readLog(done) })
        log("Lecture terminée, recherche de config.json dans LittleFS")
        const wasm = await fetch("vendor/mklfs-0.1.0.wasm").then(r => r.arrayBuffer())
        try {
            settings = await readLittlefsFile(wasm, image, "config.json")
            log(`config.json trouvé (${settings.length} octets)`)
            showSettings("lus sur l'horloge")
            progress(1, "Réglages lus. Vérifie le Wi-Fi ci-dessous puis installe.")
        } catch (e) {
            log(`Réglages illisibles : ${e.message}`)
            $("settings").textContent = "Les réglages n'ont pas pu être lus. Charge un fichier de réglages, ou installe sans."
            progress(1, "Lecture des réglages impossible.")
        }
    } catch (e) {
        log(`Erreur : ${e.message}`)
        $("step").textContent = "Connexion impossible. Débranche et rebranche l'horloge, puis réessaie."
        loader = null
        if (transport) await transport.disconnect().catch(() => {})
        $("connect").disabled = false
    }
    updateButtons()
}

async function install() {
    $("install").disabled = $("connect").disabled = true
    try {
        progress(0, "Téléchargement de la release…")
        const files = {}
        for (const name of RELEASE_FILES) {
            const r = await fetch(`firmware/${name}`, { cache: "no-store" })
            if (!r.ok) throw new Error(`firmware/${name} introuvable`)
            files[name] = new Uint8Array(await r.arrayBuffer())
            log(`Téléchargé : ${name} (${Math.round(files[name].length / 1024)} Ko)`)
            const expected = release.sha256 && release.sha256[name]
            if (expected) {
                const hash = [...new Uint8Array(await crypto.subtle.digest("SHA-256", files[name]))].map(b => b.toString(16).padStart(2, "0")).join("")
                if (hash !== expected) throw new Error(`${name} : empreinte SHA-256 incorrecte`)
                log(`  SHA-256 ok`)
            }
        }
        const parts = parsePartitions(files["partitions.bin"])
        const app0 = findApp(parts, 0)
        const fs = findLittlefs(parts)
        if (!app0 || !fs) throw new Error("Table de partitions de la release invalide")
        if (files["littlefs.bin"].length !== fs.size) throw new Error("littlefs.bin ne correspond pas à la partition")

        const fileArray = [
            { name: "partitions.bin", address: 0x8000, data: files["partitions.bin"] },
            { name: "boot_app0.bin", address: 0xe000, data: files["boot_app0.bin"] },
            { name: "firmware.bin", address: app0.offset, data: files["firmware.bin"] },
            { name: "littlefs.bin", address: fs.offset, data: files["littlefs.bin"] },
        ]
        if (settings) fileArray.push({ name: "réglages", address: stagedConfigAddress(parts), data: stagedConfigBlob(settings) })
        for (const f of fileArray) log(`À écrire : ${f.name} à 0x${f.address.toString(16)} (${Math.round(f.data.length / 1024)} Ko)`)
        const total = fileArray.reduce((n, f) => n + f.data.length, 0)
        const before = fileArray.map((_, i) => fileArray.slice(0, i).reduce((n, f) => n + f.data.length, 0))
        const writeLogs = fileArray.map(f => progressLog(`Écriture ${f.name}`, f.data.length))

        progress(0, "Écriture sur l'horloge… ne la débranche pas.")
        await loader.writeFlash({
            fileArray: fileArray.map(f => ({ address: f.address, data: f.data })),
            flashSize: "keep", flashMode: "keep", flashFreq: "keep",
            eraseAll: false, compress: true,
            reportProgress: (i, written, size) => {
                progress((before[i] + written / size * fileArray[i].data.length) / total)
                writeLogs[i](Math.round(written / size * fileArray[i].data.length))
            },
        })
        log("Écriture terminée, redémarrage de l'horloge")
        await loader.after("hard_reset")
        await transport.disconnect().catch(() => {})
        progress(1, settings
            ? "Terminé. L'horloge redémarre avec ses réglages ; les prochaines mises à jour passent par le Wi-Fi."
            : "Terminé. L'horloge redémarre sans réglages : connecte-toi à son point d'accès pour la configurer.")
        log("Installation terminée.")
    } catch (e) {
        log(`Erreur : ${e.message}`)
        $("step").textContent = "L'installation a échoué. Garde l'horloge branchée et réessaie ; rien n'est perdu tant que la copie des réglages est téléchargée."
        $("install").disabled = false
    }
}

$("connect").addEventListener("click", connect)
$("install").addEventListener("click", install)
$("nosettings").addEventListener("change", updateButtons)
$("file").addEventListener("change", async () => {
    const file = $("file").files[0]
    if (!file) return
    settings = new Uint8Array(await file.arrayBuffer())
    showSettings("du fichier")
})

if (!("serial" in navigator)) {
    $("unsupported").hidden = false
    $("connect").disabled = true
}
loadRelease().catch(e => { $("version").textContent = "aucune"; log(e.message) })

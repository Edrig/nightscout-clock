// Pure helpers of the web flasher: partition tables, reading LittleFS, the staged-settings blob.
// No browser APIs here, so test/test_web_flasher.mjs runs them under Node.

/** @typedef {{ name: string, type: number, subtype: number, offset: number, size: number }} Partition */

/**
 * Parse a binary ESP32 partition table (as written at 0x8000).
 * @param {Uint8Array} table
 * @returns {Partition[]}
 */
export function parsePartitions(table) {
    const view = new DataView(table.buffer, table.byteOffset, table.byteLength)
    const parts = []
    for (let i = 0; i + 32 <= table.length; i += 32) {
        if (view.getUint16(i, true) !== 0x50aa) break
        let name = ""
        for (let j = 0; j < 16 && table[i + 12 + j] !== 0; j++) name += String.fromCharCode(table[i + 12 + j])
        parts.push({ name, type: table[i + 2], subtype: table[i + 3], offset: view.getUint32(i + 4, true), size: view.getUint32(i + 8, true) })
    }
    return parts
}

/** @param {Partition[]} parts */
export const findLittlefs = parts => parts.find(p => p.type === 0x01 && (p.subtype === 0x82 || p.subtype === 0x83))
/** @param {Partition[]} parts */
export const findApp = (parts, slot) => parts.find(p => p.type === 0x00 && p.subtype === 0x10 + slot)

/** Standard CRC-32 (zlib), as the firmware computes it. */
export function crc32(data) {
    let crc = 0xffffffff
    for (let i = 0; i < data.length; i++) {
        crc ^= data[i]
        for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1))
    }
    return (crc ^ 0xffffffff) >>> 0
}

export const STAGED_CONFIG_AREA = 0x10000
const MAGIC = [..."NSCLKCFG"].map(c => c.charCodeAt(0))

/**
 * The block the firmware imports on its first boot (OtaManager::importStagedConfig): "NSCLKCFG", length, CRC-32,
 * then the settings JSON, padded with 0xFF to whole 4 KB sectors.
 * @param {Uint8Array} json
 * @returns {Uint8Array}
 */
export function stagedConfigBlob(json) {
    if (json.length > STAGED_CONFIG_AREA - 16) throw new Error("The settings are too large to carry over")
    const size = Math.ceil((16 + json.length) / 4096) * 4096
    const blob = new Uint8Array(size).fill(0xff)
    const view = new DataView(blob.buffer)
    blob.set(MAGIC, 0)
    view.setUint32(8, json.length, true)
    view.setUint32(12, crc32(json), true)
    blob.set(json, 16)
    return blob
}

/** Where the blob goes: the last 64 KB of the ota_1 slot, unused until the first OTA update. */
export function stagedConfigAddress(parts) {
    const slot = findApp(parts, 1)
    if (!slot) throw new Error("The release's partition table has no ota_1 slot")
    return slot.offset + slot.size - STAGED_CONFIG_AREA
}

// ---------- reading LittleFS ----------
// Uses littlefs compiled to WebAssembly by @wasm-os/mklfs 0.1.0 (MIT, littlefs BSD-3-Clause).
// The module has no imports at all; this is a read-only port of its openLittlefsImage().

const GEOMETRIES = [
    { readSize: 128, progSize: 128 }, // esp_littlefs, files the clock wrote
    { readSize: 256, progSize: 256 }, // mklittlefs (PlatformIO images)
]

/**
 * Read one file from a LittleFS image.
 * @param {ArrayBuffer | Uint8Array} wasmBytes - mklfs.wasm
 * @param {Uint8Array} image - The whole partition.
 * @param {string} path - File name without the leading slash.
 * @returns {Promise<Uint8Array>}
 */
export async function readLittlefsFile(wasmBytes, image, path) {
    let lastError = null
    for (const g of GEOMETRIES) {
        const { instance } = await WebAssembly.instantiate(wasmBytes, {})
        const x = instance.exports
        if (x._initialize) x._initialize()
        const mem = () => new Uint8Array(x.memory.buffer)
        mem().set(image, x.mk_image_ptr())
        const rc = x.mk_mount(4096, image.length / 4096, g.readSize, g.progSize, 512, 128)
        if (rc !== 0) { lastError = new Error(`LittleFS mount failed (${rc})`); continue }
        const name = new TextEncoder().encode(path)
        mem().set(name, x.mk_name_buf())
        mem()[x.mk_name_buf() + name.length] = 0
        const size = x.mk_read_file()
        if (size < 0) { lastError = new Error(size === -2 ? `${path} is not on the clock` : `LittleFS read failed (${size})`); continue }
        return mem().slice(x.mk_file_buf(), x.mk_file_buf() + size)
    }
    throw lastError
}

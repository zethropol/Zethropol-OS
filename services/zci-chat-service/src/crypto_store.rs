use serde::{Deserialize, Serialize};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};
use uuid::Uuid;

const SHA256_K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

const MAX_STORE_FILE_BYTES: u64 = 8 * 1024 * 1024;
const STORE_FORMAT_TAG: &str = "zethropol-zci-chat-enc-v1";
const CIPHER_SUITE_TAG: &str = "sha256-ctr-hmac-sha256";

pub fn now_unix_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

pub fn sha256_bytes(data: &[u8]) -> [u8; 32] {
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
        0x5be0cd19,
    ];
    let bit_len = (data.len() as u64).wrapping_mul(8);
    let mut padded = Vec::with_capacity(((data.len() + 9 + 63) / 64) * 64);
    padded.extend_from_slice(data);
    padded.push(0x80);
    while (padded.len() % 64) != 56 {
        padded.push(0);
    }
    padded.extend_from_slice(&bit_len.to_be_bytes());

    for chunk in padded.chunks_exact(64) {
        let mut w = [0u32; 64];
        for (i, word) in chunk.chunks_exact(4).enumerate() {
            w[i] = u32::from_be_bytes([word[0], word[1], word[2], word[3]]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }

        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh] = h;
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ ((!e) & g);
            let temp1 = hh
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(SHA256_K[i])
                .wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let temp2 = s0.wrapping_add(maj);

            hh = g;
            g = f;
            f = e;
            e = d.wrapping_add(temp1);
            d = c;
            c = b;
            b = a;
            a = temp1.wrapping_add(temp2);
        }

        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
        h[5] = h[5].wrapping_add(f);
        h[6] = h[6].wrapping_add(g);
        h[7] = h[7].wrapping_add(hh);
    }

    let mut out = [0u8; 32];
    for (i, word) in h.iter().enumerate() {
        out[i * 4..i * 4 + 4].copy_from_slice(&word.to_be_bytes());
    }
    out
}

pub fn hmac_sha256(key: &[u8], data: &[u8]) -> [u8; 32] {
    let mut key_block = [0u8; 64];
    if key.len() > 64 {
        let hashed = sha256_bytes(key);
        key_block[..32].copy_from_slice(&hashed);
    } else {
        key_block[..key.len()].copy_from_slice(key);
    }

    let mut ipad = [0x36u8; 64];
    let mut opad = [0x5cu8; 64];
    for i in 0..64 {
        ipad[i] ^= key_block[i];
        opad[i] ^= key_block[i];
    }

    let mut inner = Vec::with_capacity(64 + data.len());
    inner.extend_from_slice(&ipad);
    inner.extend_from_slice(data);
    let inner_hash = sha256_bytes(&inner);

    let mut outer = Vec::with_capacity(64 + 32);
    outer.extend_from_slice(&opad);
    outer.extend_from_slice(&inner_hash);
    sha256_bytes(&outer)
}

fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for (&x, &y) in a.iter().zip(b.iter()) {
        diff |= x ^ y;
    }
    diff == 0
}

fn to_hex(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        use std::fmt::Write;
        let _ = write!(&mut out, "{b:02x}");
    }
    out
}

fn from_hex(hex: &str) -> Result<Vec<u8>, String> {
    if hex.len() % 2 != 0 {
        return Err("Odd-length hex encoding.".to_string());
    }
    let mut out = Vec::with_capacity(hex.len() / 2);
    let bytes = hex.as_bytes();
    for pair in bytes.chunks_exact(2) {
        let hi = (pair[0] as char)
            .to_digit(16)
            .ok_or_else(|| "Invalid hex character.".to_string())?;
        let lo = (pair[1] as char)
            .to_digit(16)
            .ok_or_else(|| "Invalid hex character.".to_string())?;
        out.push(((hi << 4) | lo) as u8);
    }
    Ok(out)
}

fn read_os_random(buf: &mut [u8]) -> Result<(), String> {
    let mut urandom = File::open("/dev/urandom")
        .map_err(|e| format!("Failed to open /dev/urandom for encryption entropy: {e}"))?;
    urandom
        .read_exact(buf)
        .map_err(|e| format!("Failed reading random entropy: {e}"))
}

fn derive_subkeys(master_key: &[u8; 32]) -> ([u8; 32], [u8; 32]) {
    let enc_key = hmac_sha256(master_key, b"zethropol-zci-chat-enc-key-v1");
    let mac_key = hmac_sha256(master_key, b"zethropol-zci-chat-mac-key-v1");
    (enc_key, mac_key)
}

fn xor_keystream(enc_key: &[u8; 32], nonce: &[u8; 16], data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len());
    let mut counter: u64 = 0;
    for chunk in data.chunks(32) {
        let mut ctr_input = [0u8; 24];
        ctr_input[..16].copy_from_slice(nonce);
        ctr_input[16..24].copy_from_slice(&counter.to_be_bytes());
        let block = hmac_sha256(enc_key, &ctr_input);
        for (i, &b) in chunk.iter().enumerate() {
            out.push(b ^ block[i]);
        }
        counter = counter.wrapping_add(1);
    }
    out
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct EncryptedEnvelope {
    format: String,
    cipher: String,
    nonce_hex: String,
    ciphertext_hex: String,
    mac_hex: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChatMessage {
    pub message_id: String,
    pub role: String, // "user" | "assistant" | "system_notice"
    pub content: String,
    pub timestamp_unix: u64,
    pub executable: bool,
    pub policy_authority: String,
    pub envelope_id: String,
    pub injection_detected: bool,
    pub system_context_attached: bool,
    pub model_id: String,
    pub latency_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChatSession {
    pub session_id: String,
    pub title: String,
    pub created_at_unix: u64,
    pub updated_at_unix: u64,
    pub include_system_context_default: bool,
    pub messages: Vec<ChatMessage>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct PersistedChatData {
    pub schema_version: u32,
    pub sessions: Vec<ChatSession>,
}

/// 12.06 — Permission-restricted (`0700`/`0600`), authenticated-encrypted local chat store.
pub struct EncryptedChatStore {
    storage_dir: PathBuf,
    master_key: [u8; 32],
}

impl EncryptedChatStore {
    pub fn open(storage_dir: PathBuf) -> Result<Self, String> {
        ensure_private_dir(&storage_dir)?;
        let key_path = storage_dir.join("key.bin");
        let master_key = load_or_create_key(&key_path)?;
        Ok(Self {
            storage_dir,
            master_key,
        })
    }

    pub fn storage_dir(&self) -> &Path {
        &self.storage_dir
    }

    pub fn sessions_file(&self) -> PathBuf {
        self.storage_dir.join("sessions.enc.json")
    }

    pub fn key_file(&self) -> PathBuf {
        self.storage_dir.join("key.bin")
    }

    /// Verifies directory (`0700`) and file (`0600`) permission invariants on disk.
    pub fn verify_strict_permissions(&self) -> bool {
        let Ok(d_meta) = fs::symlink_metadata(&self.storage_dir) else {
            return false;
        };
        if d_meta.file_type().is_symlink() || !d_meta.is_dir() {
            return false;
        }
        if (d_meta.permissions().mode() & 0o077) != 0 {
            return false;
        }
        for path in [self.key_file(), self.sessions_file()] {
            if path.exists() {
                let Ok(m) = fs::symlink_metadata(&path) else {
                    return false;
                };
                if m.file_type().is_symlink() || !m.is_file() {
                    return false;
                }
                if (m.permissions().mode() & 0o077) != 0 {
                    return false;
                }
            }
        }
        true
    }

    pub fn load(&self) -> Result<PersistedChatData, String> {
        let path = self.sessions_file();
        if !path.exists() {
            return Ok(PersistedChatData {
                schema_version: 1,
                sessions: Vec::new(),
            });
        }
        let meta = fs::symlink_metadata(&path)
            .map_err(|e| format!("Failed to stat chat history file: {e}"))?;
        if meta.file_type().is_symlink() || !meta.is_file() {
            return Err("Refusing symlink or non-regular chat history file.".to_string());
        }
        if (meta.permissions().mode() & 0o077) != 0 {
            return Err("Chat history file permissions are too permissive (expected 0600).".to_string());
        }
        if meta.len() > MAX_STORE_FILE_BYTES {
            return Err("Chat history file exceeds 8 MiB safety cap.".to_string());
        }

        let raw = fs::read_to_string(&path)
            .map_err(|e| format!("Failed reading encrypted chat history: {e}"))?;
        let env: EncryptedEnvelope = serde_json::from_str(&raw)
            .map_err(|e| format!("Malformed encrypted chat envelope: {e}"))?;
        if env.format != STORE_FORMAT_TAG || env.cipher != CIPHER_SUITE_TAG {
            return Err("Unsupported encrypted chat envelope format or cipher.".to_string());
        }

        let nonce_vec = from_hex(&env.nonce_hex)?;
        if nonce_vec.len() != 16 {
            return Err("Invalid encryption nonce length.".to_string());
        }
        let mut nonce = [0u8; 16];
        nonce.copy_from_slice(&nonce_vec);
        let ciphertext = from_hex(&env.ciphertext_hex)?;
        let mac_vec = from_hex(&env.mac_hex)?;

        let (enc_key, mac_key) = derive_subkeys(&self.master_key);
        let mut mac_input = Vec::with_capacity(STORE_FORMAT_TAG.len() + 16 + ciphertext.len());
        mac_input.extend_from_slice(STORE_FORMAT_TAG.as_bytes());
        mac_input.extend_from_slice(&nonce);
        mac_input.extend_from_slice(&ciphertext);
        let expected_mac = hmac_sha256(&mac_key, &mac_input);

        if !constant_time_eq(&expected_mac, &mac_vec) {
            return Err("Fail-closed rejection: encrypted chat history MAC verification failed (tampered file).".to_string());
        }

        let plaintext = xor_keystream(&enc_key, &nonce, &ciphertext);
        let parsed: PersistedChatData = serde_json::from_slice(&plaintext)
            .map_err(|e| format!("Failed decoding decrypted chat JSON: {e}"))?;
        if parsed.schema_version != 1 {
            return Err(format!("Unsupported chat schema_version {}", parsed.schema_version));
        }
        Ok(parsed)
    }

    pub fn save(&self, data: &PersistedChatData) -> Result<(), String> {
        ensure_private_dir(&self.storage_dir)?;
        let payload = PersistedChatData {
            schema_version: 1,
            sessions: data.sessions.clone(),
        };
        let plaintext = serde_json::to_vec(&payload)
            .map_err(|e| format!("Failed serializing chat sessions: {e}"))?;

        let mut nonce = [0u8; 16];
        read_os_random(&mut nonce)?;
        let (enc_key, mac_key) = derive_subkeys(&self.master_key);
        let ciphertext = xor_keystream(&enc_key, &nonce, &plaintext);

        let mut mac_input = Vec::with_capacity(STORE_FORMAT_TAG.len() + 16 + ciphertext.len());
        mac_input.extend_from_slice(STORE_FORMAT_TAG.as_bytes());
        mac_input.extend_from_slice(&nonce);
        mac_input.extend_from_slice(&ciphertext);
        let mac = hmac_sha256(&mac_key, &mac_input);

        let env = EncryptedEnvelope {
            format: STORE_FORMAT_TAG.to_string(),
            cipher: CIPHER_SUITE_TAG.to_string(),
            nonce_hex: to_hex(&nonce),
            ciphertext_hex: to_hex(&ciphertext),
            mac_hex: to_hex(&mac),
        };
        let encoded = serde_json::to_vec_pretty(&env)
            .map_err(|e| format!("Failed encoding encrypted envelope: {e}"))?;

        let target = self.sessions_file();
        let tmp = self
            .storage_dir
            .join(format!(".sessions.tmp.{}", Uuid::new_v4()));
        {
            let mut f = OpenOptions::new()
                .create_new(true)
                .write(true)
                .mode(0o600)
                .open(&tmp)
                .map_err(|e| format!("Failed creating temp chat store: {e}"))?;
            f.write_all(&encoded)
                .map_err(|e| format!("Failed writing temp chat store: {e}"))?;
            f.sync_all()
                .map_err(|e| format!("Failed syncing temp chat store: {e}"))?;
        }
        fs::set_permissions(&tmp, fs::Permissions::from_mode(0o600))
            .map_err(|e| format!("Failed setting 0600 on chat store: {e}"))?;
        fs::rename(&tmp, &target).map_err(|e| {
            let _ = fs::remove_file(&tmp);
            format!("Failed atomically replacing chat store: {e}")
        })?;
        Ok(())
    }
}

fn ensure_private_dir(dir: &Path) -> Result<(), String> {
    if dir.exists() {
        let meta = fs::symlink_metadata(dir)
            .map_err(|e| format!("Failed to stat storage dir '{}': {e}", dir.display()))?;
        if meta.file_type().is_symlink() || !meta.is_dir() {
            return Err("Chat storage path must be a non-symlink directory.".to_string());
        }
        fs::set_permissions(dir, fs::Permissions::from_mode(0o700))
            .map_err(|e| format!("Failed enforcing 0700 on chat storage dir: {e}"))?;
        return Ok(());
    }

    if let Some(parent) = dir.parent() {
        fs::create_dir_all(parent)
            .map_err(|e| format!("Failed creating parent data directory: {e}"))?;
    }
    fs::DirBuilder::new()
        .mode(0o700)
        .create(dir)
        .map_err(|e| format!("Failed creating private 0700 chat directory: {e}"))?;
    fs::set_permissions(dir, fs::Permissions::from_mode(0o700))
        .map_err(|e| format!("Failed enforcing 0700 on new chat directory: {e}"))?;
    Ok(())
}

fn load_or_create_key(key_path: &Path) -> Result<[u8; 32], String> {
    if key_path.exists() {
        let meta = fs::symlink_metadata(key_path)
            .map_err(|e| format!("Failed to stat chat key file: {e}"))?;
        if meta.file_type().is_symlink() || !meta.is_file() {
            return Err("Refusing symlink or non-regular chat key file.".to_string());
        }
        if (meta.permissions().mode() & 0o077) != 0 {
            return Err("Chat key file permissions must be 0600.".to_string());
        }
        let bytes = fs::read(key_path).map_err(|e| format!("Failed reading chat key: {e}"))?;
        if bytes.len() != 32 {
            return Err("Corrupted local chat encryption key length.".to_string());
        }
        let mut key = [0u8; 32];
        key.copy_from_slice(&bytes);
        return Ok(key);
    }

    let mut key = [0u8; 32];
    read_os_random(&mut key)?;
    let mut f = OpenOptions::new()
        .create_new(true)
        .write(true)
        .mode(0o600)
        .open(key_path)
        .map_err(|e| format!("Failed creating 0600 local chat key: {e}"))?;
    f.write_all(&key)
        .map_err(|e| format!("Failed writing local chat key: {e}"))?;
    f.sync_all()
        .map_err(|e| format!("Failed syncing local chat key: {e}"))?;
    fs::set_permissions(key_path, fs::Permissions::from_mode(0o600))
        .map_err(|e| format!("Failed setting 0600 on chat key: {e}"))?;
    Ok(key)
}

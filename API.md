# Dokumentasi API — Panic Button IoT

Dokumentasi lengkap REST API untuk server Panic Button IoT.

**Base URL:** `http://192.168.2.50:3000`

## Daftar Isi

- [Ringkasan Endpoint](#ringkasan-endpoint)
- [Autentikasi HMAC-SHA256](#autentikasi-hmac-sha256)
- [Detail Endpoint](#detail-endpoint)
- [POST /api/heartbeat](#post-apiheartbeat)
- [POST /api/panic](#post-apipanic)
- [POST /api/panicoff](#post-apipanicoff)
- [GET /api/devices](#get-apidevices)
- [DELETE /api/devices/:device_id](#delete-apidevicesdevice_id)
- [GET /api/status/:device_id](#get-apistatusdevice_id)
- [GET /api/panic/active](#get-apipanicactive)
- [GET /api/events](#get-apievents)
- [GET /api/events/:device_id](#get-apieventsdevice_id)
- [GET /api/heartbeat/history/:device_id](#get-apiheartbeat-historydevice_id)
- [POST /api/silent-mode](#post-apisilent-mode)
- [GET /api/silent-mode/:device_id](#get-apisilent-modedevice_id)
- [PATCH /api/silent-mode/:device_id](#patch-apisilent-modedevice_id)
- [DELETE /api/silent-mode/:device_id](#delete-apisilent-modedevice_id)
- [GET /api/heartbeat-interval/:device_id](#get-apiheartbeat-intervaldevice_id)
- [PATCH /api/heartbeat-interval/:device_id](#patch-apiheartbeat-intervaldevice_id)
- [Skema Database](#skema-database)

---

## Ringkasan Endpoint

### Endpoint Terproteksi (HMAC-SHA256)

Endpoint ini **wajib** menyertakan header autentikasi HMAC-SHA256. Request tanpa tanda tangan yang valid akan ditolak.

| Method | Endpoint | Deskripsi |
|--------|----------|-----------|
| `POST` | `/api/heartbeat` | Menerima heartbeat dari perangkat |
| `POST` | `/api/panic` | Menerima sinyal panic ON |
| `POST` | `/api/panicoff` | Menerima sinyal panic OFF |

### Endpoint Publik

Endpoint ini dapat diakses tanpa autentikasi (digunakan oleh dashboard web).

| Method | Endpoint | Deskripsi |
|--------|----------|-----------|
| `GET` | `/api/devices` | Daftar semua perangkat |
| `DELETE` | `/api/devices/:device_id` | Hapus perangkat beserta semua data terkait |
| `GET` | `/api/status/:device_id` | Status perangkat tertentu |
| `GET` | `/api/panic/active` | Daftar perangkat dengan panic aktif |
| `GET` | `/api/events` | Riwayat semua event panic |
| `GET` | `/api/events/:device_id` | Riwayat event panic per perangkat |
| `GET` | `/api/heartbeat/history/:device_id` | Riwayat heartbeat per perangkat |
| `POST` | `/api/silent-mode` | Atur mode senyap (lengkap) |
| `GET` | `/api/silent-mode/:device_id` | Ambil pengaturan mode senyap |
| `PATCH` | `/api/silent-mode/:device_id` | Perbarui sebagian mode senyap |
| `DELETE` | `/api/silent-mode/:device_id` | Hapus pengaturan mode senyap |
| `GET` | `/api/heartbeat-interval/:device_id` | Ambil interval heartbeat |
| `PATCH` | `/api/heartbeat-interval/:device_id` | Perbarui interval heartbeat |

---

## Autentikasi HMAC-SHA256

### Gambaran Umum

Semua endpoint `POST` yang menerima data dari Arduino dilindungi oleh verifikasi tanda tangan HMAC-SHA256. Ini memastikan bahwa request benar-benar berasal dari Arduino yang sah dan data tidak dimanipulasi.

### Header yang Diperlukan

| Header | Format | Contoh | Deskripsi |
|--------|--------|--------|-----------|
| `X-Device-ID` | `ARDPBXXXX` | `ARDPB0001` | ID perangkat Arduino |
| `X-Timestamp` | Angka (millis) | `12345678` | Timestamp millis() Arduino |
| `X-Nonce` | 8 karakter hex | `a1b2c3d4` | Nilai sekali pakai (anti-replay) |
| `X-Signature` | 64 karakter hex | `3a7f2b...` | HMAC-SHA256 signature |

### Algoritma Signature

```
Signature = HMAC-SHA256(
  key:     32-byte secret key (sama di Arduino dan server),
  message: deviceId + timestamp + nonce + rawBody
)
```

**Contoh:**

```
deviceId  = "ARDPB0001"
timestamp = "12345678"
nonce     = "a1b2c3d4"
rawBody   = '{"device_id":"ARDPB0001","status":"heartbeat",...}'

message = "ARDPB000112345678a1b2c3d4{\"device_id\":\"ARDPB0001\",\"status\":\"heartbeat\",...}"

signature = HMAC-SHA256(secretKey, message)
```

### Perlindungan Anti-Replay

Setiap nonce hanya dapat digunakan **satu kali**. Server menyimpan nonce yang telah digunakan selama 10 menit. Jika nonce yang sama dikirim ulang dalam periode tersebut, request ditolak dengan status `403`.

### Penanganan Error Autentikasi

| Status | Kondisi | Pesan |
|--------|---------|-------|
| `401` | Header HMAC tidak lengkap | `Missing authentication headers` |
| `401` | Format Device ID tidak valid | `Invalid device ID format` |
| `401` | Format timestamp tidak valid | `Invalid timestamp format` |
| `401` | Format nonce tidak valid | `Invalid nonce format (expected 8-char hex)` |
| `401` | Format signature tidak valid | `Invalid signature format (expected 64-char hex)` |
| `403` | Nonce sudah digunakan (replay) | `Nonce already used (replay detected)` |
| `403` | Signature tidak cocok | `Invalid signature` |

### Contoh Request dengan HMAC

```http
POST /api/heartbeat HTTP/1.1
Host: 192.168.2.50:3000
Content-Type: application/json
X-Device-ID: ARDPB0001
X-Timestamp: 12345678
X-Nonce: a1b2c3d4
X-Signature: 3a7f2b9c8e1d4f6a5b0c3d2e1f4a7b9c8e1d4f6a5b0c3d2e1f4a7b9c8e1d4f6a
Content-Length: 95

{"device_id":"ARDPB0001","status":"heartbeat","timestamp":12345678,"led_red":false,...}
```

### Konfigurasi Kunci

**Kunci Default (untuk pengembangan):**

```
0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20
```

**Kunci Produksi:**

Atur melalui environment variable:
```bash
set HMAC_SECRET_KEY=<64 karakter hex>
```

Generate kunci baru:
```bash
node -e "console.log(require('crypto').randomBytes(32).toString('hex'))"
```

**Kunci Per-Perangkat:**

Server mendukung kunci unik per perangkat yang disimpan di tabel `device_keys`. Jika perangkat memiliki kunci sendiri, kunci tersebut digunakan. Jika tidak, server menggunakan kunci default.

---

## Detail Endpoint

### POST /api/heartbeat

Menerima heartbeat dari perangkat Arduino. Endpoint ini **terproteksi HMAC**.

**Header Autentikasi:** `X-Device-ID`, `X-Timestamp`, `X-Nonce`, `X-Signature`

**Request Body:**

```json
{
  "device_id": "ARDPB0001",
  "status": "heartbeat",
  "timestamp": 12345678,
  "led_red": false,
  "led_yellow": false,
  "led_green": true,
  "panic_button": false,
  "sirene": false,
  "rotator": false,
  "panic_state": false
}
```

| Field | Tipe | Wajib | Deskripsi |
|-------|------|:-----:|-----------|
| `device_id` | string | ✅ | ID perangkat (format `ARDPBXXXX`) |
| `status` | string | ❌ | Status perangkat |
| `timestamp` | number | ❌ | Timestamp millis() Arduino |
| `led_red` | boolean | ❌ | Status LED merah |
| `led_yellow` | boolean | ❌ | Status LED kuning |
| `led_green` | boolean | ❌ | Status LED hijau |
| `panic_button` | boolean | ❌ | Status tombol panic |
| `sirene` | boolean | ❌ | Status relay sirene |
| `rotator` | boolean | ❌ | Status relay rotator |
| `panic_state` | boolean | ❌ | Status panic aktif |

**Response 200 OK:**

```json
{
  "success": true,
  "message": "Heartbeat received",
  "device_id": "ARDPB0001",
  "status": "online",
  "server_time_gmt7": "14:30:00",
  "silent_mode": {
    "enabled": false,
    "start_time": "00:00:00",
    "end_time": "00:00:00"
  },
  "heartbeat_interval": {
    "interval_seconds": 60
  },
  "command": null
}
```

**Response 401 (Autentikasi Gagal):**

```json
{
  "success": false,
  "message": "Missing authentication headers. Required: X-Device-ID, X-Timestamp, X-Nonce, X-Signature"
}
```

**Response 403 (Signature Tidak Valid / Replay):**

```json
{
  "success": false,
  "message": "Invalid signature"
}
```

**Response 400:**

```json
{
  "success": false,
  "message": "device_id is required"
}
```

---

### POST /api/panic

Menerima sinyal panic ON dari perangkat Arduino. Endpoint ini **terproteksi HMAC**.

**Header Autentikasi:** `X-Device-ID`, `X-Timestamp`, `X-Nonce`, `X-Signature`

**Request Body:**

```json
{
  "device_id": "ARDPB0001",
  "status": "panic",
  "timestamp": 12345678
}
```

**Response 200 OK:**

```json
{
  "success": true,
  "message": "Panic event received",
  "event_id": 42,
  "device_id": "ARDPB0001",
  "server_time_gmt7": "14:30:00",
  "silent_mode": {
    "enabled": false,
    "start_time": "00:00:00",
    "end_time": "00:00:00"
  },
  "heartbeat_interval": {
    "interval_seconds": 60
  },
  "command": null
}
```

---

### POST /api/panicoff

Menerima sinyal panic OFF dari perangkat Arduino. Endpoint ini **terproteksi HMAC**.

**Header Autentikasi:** `X-Device-ID`, `X-Timestamp`, `X-Nonce`, `X-Signature`

**Request Body:**

```json
{
  "device_id": "ARDPB0001",
  "status": "off",
  "timestamp": 12345678
}
```

**Response 200 OK:**

```json
{
  "success": true,
  "message": "Panic OFF received",
  "event_id": 43,
  "device_id": "ARDPB0001",
  "server_time_gmt7": "14:35:00",
  "silent_mode": {
    "enabled": false,
    "start_time": "00:00:00",
    "end_time": "00:00:00"
  },
  "heartbeat_interval": {
    "interval_seconds": 60
  },
  "command": null
}
```

---

### GET /api/devices

Mengambil daftar semua perangkat yang terdaftar.

**Response 200 OK:**

```json
{
  "success": true,
  "count": 2,
  "devices": [
    {
      "id": 1,
      "device_id": "ARDPB0001",
      "device_name": null,
      "status": "normal",
      "last_seen": "2025-12-01 14:30:00",
      "created_at": "2025-12-01 10:00:00"
    },
    {
      "id": 2,
      "device_id": "ARDPB0002",
      "device_name": "Gedung B Lt.3",
      "status": "panic",
      "last_seen": "2025-12-01 14:28:00",
      "created_at": "2025-12-01 11:00:00"
    }
  ]
}
```

---

### DELETE /api/devices/:device_id

Menghapus perangkat beserta **semua data terkait** (heartbeat, event panic, mode senyap, pengaturan heartbeat, perintah, dan kunci HMAC).

**Parameter URL:**

| Parameter | Deskripsi |
|-----------|-----------|
| `device_id` | ID perangkat (contoh: `ARDPB0001`) |

**Response 200 OK:**

```json
{
  "success": true,
  "message": "Device ARDPB0001 and all related data deleted"
}
```

**Response 404:**

```json
{
  "success": false,
  "message": "Device not found"
}
```

---

### GET /api/status/:device_id

Mengambil status perangkat tertentu.

**Parameter URL:**

| Parameter | Deskripsi |
|-----------|-----------|
| `device_id` | ID perangkat (contoh: `ARDPB0001`) |

**Response 200 OK:**

```json
{
  "success": true,
  "device": {
    "id": 1,
    "device_id": "ARDPB0001",
    "device_name": null,
    "status": "normal",
    "last_seen": "2025-12-01 14:30:00",
    "created_at": "2025-12-01 10:00:00"
  }
}
```

---

### GET /api/panic/active

Mengambil daftar perangkat yang sedang dalam status panic.

**Response 200 OK:**

```json
{
  "success": true,
  "count": 1,
  "devices": [
    {
      "id": 2,
      "device_id": "ARDPB0002",
      "device_name": "Gedung B Lt.3",
      "status": "panic",
      "last_seen": "2025-12-01 14:28:00",
      "created_at": "2025-12-01 11:00:00"
    }
  ]
}
```

---

### GET /api/events

Mengambil riwayat semua event panic dari semua perangkat.

**Query Parameters:**

| Parameter | Default | Deskripsi |
|-----------|---------|-----------|
| `limit` | `100` | Jumlah maksimum event yang dikembalikan |

**Response 200 OK:**

```json
{
  "success": true,
  "count": 2,
  "events": [
    {
      "id": 43,
      "device_id": "ARDPB0001",
      "status": "off",
      "timestamp": 12345678,
      "device_ip": "::ffff:192.168.2.248",
      "received_at": "2025-12-01 14:35:00"
    },
    {
      "id": 42,
      "device_id": "ARDPB0001",
      "status": "panic",
      "timestamp": 12340000,
      "device_ip": "::ffff:192.168.2.248",
      "received_at": "2025-12-01 14:30:00"
    }
  ]
}
```

---

### GET /api/events/:device_id

Mengambil riwayat event panic untuk perangkat tertentu.

**Query Parameters:**

| Parameter | Default | Deskripsi |
|-----------|---------|-----------|
| `limit` | `100` | Jumlah maksimum event yang dikembalikan |

**Response 200 OK:**

```json
{
  "success": true,
  "device_id": "ARDPB0001",
  "count": 2,
  "events": [
    {
      "id": 43,
      "device_id": "ARDPB0001",
      "status": "off",
      "timestamp": 12345678,
      "device_ip": "::ffff:192.168.2.248",
      "received_at": "2025-12-01 14:35:00"
    }
  ]
}
```

---

### GET /api/heartbeat/history/:device_id

Mengambil riwayat heartbeat untuk perangkat tertentu.

**Query Parameters:**

| Parameter | Default | Deskripsi |
|-----------|---------|-----------|
| `limit` | `50` | Jumlah maksimum heartbeat yang dikembalikan |

**Response 200 OK:**

```json
{
  "success": true,
  "device_id": "ARDPB0001",
  "count": 1,
  "heartbeats": [
    {
      "id": 100,
      "device_id": "ARDPB0001",
      "timestamp": 12345678,
      "device_ip": "::ffff:192.168.2.248",
      "led_red": 0,
      "led_yellow": 0,
      "led_green": 1,
      "panic_button": 0,
      "sirene": 0,
      "rotator": 0,
      "panic_state": 0,
      "received_at": "2025-12-01 14:30:00"
    }
  ]
}
```

---

### POST /api/silent-mode

Mengatur pengaturan mode senyap untuk perangkat (pengaturan lengkap). Jika perangkat belum terdaftar, otomatis didaftarkan.

**Request Body:**

```json
{
  "device_id": "ARDPB0001",
  "enabled": true,
  "start_time": "22:00:00",
  "end_time": "06:00:00"
}
```

| Field | Tipe | Wajib | Deskripsi |
|-------|------|:-----:|-----------|
| `device_id` | string | ✅ | ID perangkat |
| `enabled` | boolean | ✅ | Aktifkan/nonaktifkan mode senyap |
| `start_time` | string | ✅ | Waktu mulai (format `HH:MM:SS`) |
| `end_time` | string | ✅ | Waktu selesai (format `HH:MM:SS`) |

**Response 200 OK:**

```json
{
  "success": true,
  "message": "Silent mode configured",
  "silent_mode": {
    "device_id": "ARDPB0001",
    "enabled": 1,
    "start_time": "22:00:00",
    "end_time": "06:00:00"
  }
}
```

---

### GET /api/silent-mode/:device_id

Mengambil pengaturan mode senyap untuk perangkat tertentu.

**Response 200 OK:**

```json
{
  "success": true,
  "device_id": "ARDPB0001",
  "silent_mode": {
    "id": 1,
    "device_id": "ARDPB0001",
    "enabled": 1,
    "start_time": "22:00:00",
    "end_time": "06:00:00",
    "updated_at": "2025-12-01 14:00:00"
  }
}
```

**Response 404:**

```json
{
  "success": true,
  "device_id": "ARDPB0001",
  "silent_mode": null,
  "message": "No silent mode configured"
}
```

---

### PATCH /api/silent-mode/:device_id

Memperbarui sebagian pengaturan mode senyap. Hanya field yang disertakan dalam body yang diperbarui.

**Request Body (semua field opsional):**

```json
{
  "enabled": false
}
```

```json
{
  "start_time": "20:00:00",
  "end_time": "07:00:00"
}
```

**Response 200 OK:**

```json
{
  "success": true,
  "message": "Silent mode updated",
  "silent_mode": {
    "id": 1,
    "device_id": "ARDPB0001",
    "enabled": 0,
    "start_time": "22:00:00",
    "end_time": "06:00:00",
    "updated_at": "2025-12-01 15:00:00"
  }
}
```

---

### DELETE /api/silent-mode/:device_id

Menghapus pengaturan mode senyap untuk perangkat tertentu.

**Response 200 OK:**

```json
{
  "success": true,
  "message": "Silent mode deleted for device ARDPB0001"
}
```

---

### GET /api/heartbeat-interval/:device_id

Mengambil pengaturan interval heartbeat untuk perangkat tertentu.

**Response 200 OK:**

```json
{
  "success": true,
  "device_id": "ARDPB0001",
  "heartbeat_interval": {
    "id": 1,
    "device_id": "ARDPB0001",
    "interval_seconds": 60,
    "updated_at": "2025-12-01 14:00:00"
  }
}
```

**Response 404:**

```json
{
  "success": true,
  "device_id": "ARDPB0001",
  "heartbeat_interval": {
    "interval_seconds": 60
  },
  "message": "Using default interval"
}
```

---

### PATCH /api/heartbeat-interval/:device_id

Memperbarui interval heartbeat untuk perangkat tertentu. Minimum 5 detik.

**Request Body:**

```json
{
  "interval_seconds": 30
}
```

| Field | Tipe | Wajib | Deskripsi |
|-------|------|:-----:|-----------|
| `interval_seconds` | number | ✅ | Interval dalam detik (min: 5) |

**Response 200 OK:**

```json
{
  "success": true,
  "message": "Heartbeat interval updated to 30s",
  "heartbeat_interval": {
    "id": 1,
    "device_id": "ARDPB0001",
    "interval_seconds": 30,
    "updated_at": "2025-12-01 15:00:00"
  }
}
```

**Response 400:**

```json
{
  "success": false,
  "message": "interval_seconds must be a number (minimum 5)"
}
```

---

## Skema Database

Database SQLite disimpan di `server/data/panicbutton.db`.

### Tabel `devices`

| Kolom | Tipe | Keterangan |
|-------|------|------------|
| `id` | INTEGER | Primary key, auto increment |
| `device_id` | TEXT | ID unik perangkat, format `ARDPBXXXX` |
| `device_name` | TEXT | Nama perangkat (opsional) |
| `status` | TEXT | Status: `normal`, `panic`, `online`, `offline` |
| `last_seen` | DATETIME | Terakhir terlihat |
| `created_at` | DATETIME | Waktu pendaftaran |

### Tabel `heartbeats`

| Kolom | Tipe | Keterangan |
|-------|------|------------|
| `id` | INTEGER | Primary key, auto increment |
| `device_id` | TEXT | Foreign key ke `devices` |
| `timestamp` | INTEGER | Timestamp millis() dari Arduino |
| `device_ip` | TEXT | Alamat IP perangkat |
| `led_red` | INTEGER | Status LED merah (0/1) |
| `led_yellow` | INTEGER | Status LED kuning (0/1) |
| `led_green` | INTEGER | Status LED hijau (0/1) |
| `panic_button` | INTEGER | Status tombol panic (0/1) |
| `sirene` | INTEGER | Status relay sirene (0/1) |
| `rotator` | INTEGER | Status relay rotator (0/1) |
| `panic_state` | INTEGER | Status panic aktif (0/1) |
| `received_at` | DATETIME | Waktu penerimaan di server |

### Tabel `panic_events`

| Kolom | Tipe | Keterangan |
|-------|------|------------|
| `id` | INTEGER | Primary key, auto increment |
| `device_id` | TEXT | Foreign key ke `devices` |
| `status` | TEXT | Status: `panic` atau `off` |
| `timestamp` | INTEGER | Timestamp millis() dari Arduino |
| `device_ip` | TEXT | Alamat IP perangkat |
| `received_at` | DATETIME | Waktu penerimaan di server |

### Tabel `silent_mode_settings`

| Kolom | Tipe | Keterangan |
|-------|------|------------|
| `id` | INTEGER | Primary key, auto increment |
| `device_id` | TEXT | Foreign key ke `devices`, unique |
| `enabled` | INTEGER | 0 = nonaktif, 1 = aktif |
| `start_time` | TEXT | Waktu mulai (`HH:MM:SS`) |
| `end_time` | TEXT | Waktu selesai (`HH:MM:SS`) |
| `updated_at` | DATETIME | Terakhir diperbarui |

### Tabel `device_commands`

| Kolom | Tipe | Keterangan |
|-------|------|------------|
| `id` | INTEGER | Primary key, auto increment |
| `device_id` | TEXT | Foreign key ke `devices` |
| `command` | TEXT | Perintah (contoh: `reset`) |
| `created_at` | DATETIME | Waktu pembuatan perintah |

### Tabel `heartbeat_settings`

| Kolom | Tipe | Keterangan |
|-------|------|------------|
| `id` | INTEGER | Primary key, auto increment |
| `device_id` | TEXT | Foreign key ke `devices`, unique |
| `interval_seconds` | INTEGER | Interval heartbeat dalam detik (default: 60, min: 5) |
| `updated_at` | DATETIME | Terakhir diperbarui |

### Tabel `device_keys`

| Kolom | Tipe | Keterangan |
|-------|------|------------|
| `id` | INTEGER | Primary key, auto increment |
| `device_id` | TEXT | Foreign key ke `devices`, unique |
| `secret_key` | TEXT | Kunci HMAC-SHA256 per-perangkat (64 karakter hex) |
| `created_at` | DATETIME | Waktu pembuatan kunci |

> **Catatan:** Tabel `device_keys` digunakan untuk kunci HMAC per-perangkat. Jika perangkat tidak memiliki entri di tabel ini, server menggunakan kunci default dari environment variable `HMAC_SECRET_KEY`.
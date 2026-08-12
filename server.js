const express = require("express");
const mysql = require("mysql2/promise");
const crypto = require("crypto");
const cors = require("cors");
const path = require("path");

const app = express();
const PORT = 3000;

// Default Secret Key (Matches Arduino)
const HMAC_SECRET_KEY =
  process.env.HMAC_SECRET_KEY ||
  "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, "public")));

// ═══════════════════════════════════════════════════════════════════
// DATABASE INITIALIZATION
// ═══════════════════════════════════════════════════════════════════

// Configure your MySQL connection parameters here (or via ENV vars)
const pool = mysql.createPool({
  host: process.env.DB_HOST || "localhost",
  user: process.env.DB_USER || "root",
  password: process.env.DB_PASSWORD || "",
  database: process.env.DB_NAME || "panicbutton",
  waitForConnections: true,
  connectionLimit: 10,
  queueLimit: 0,
});

async function initDB() {
  try {
    // Devices table
    await pool.query(`CREATE TABLE IF NOT EXISTS devices (
        id INT AUTO_INCREMENT PRIMARY KEY,
        device_id VARCHAR(50) UNIQUE,
        device_name VARCHAR(100),
        status VARCHAR(50) DEFAULT 'normal',
        device_ip VARCHAR(50),
        last_seen DATETIME,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    )`);

    // Heartbeats history table
    await pool.query(`CREATE TABLE IF NOT EXISTS heartbeats (
        id INT AUTO_INCREMENT PRIMARY KEY,
        device_id VARCHAR(50),
        timestamp BIGINT,
        device_ip VARCHAR(50),
        led_red INT DEFAULT 0,
        led_yellow INT DEFAULT 0,
        led_green INT DEFAULT 0,
        panic_button INT DEFAULT 0,
        sirene INT DEFAULT 0,
        rotator INT DEFAULT 0,
        panic_state INT DEFAULT 0,
        received_at DATETIME DEFAULT CURRENT_TIMESTAMP
    )`);

    // Panic events table
    await pool.query(`CREATE TABLE IF NOT EXISTS panic_events (
        id INT AUTO_INCREMENT PRIMARY KEY,
        device_id VARCHAR(50),
        status VARCHAR(50),
        timestamp BIGINT,
        device_ip VARCHAR(50),
        received_at DATETIME DEFAULT CURRENT_TIMESTAMP
    )`);

    // Silent mode settings table
    await pool.query(`CREATE TABLE IF NOT EXISTS silent_mode_settings (
        id INT AUTO_INCREMENT PRIMARY KEY,
        device_id VARCHAR(50) UNIQUE,
        enabled INT DEFAULT 0,
        start_time TIME DEFAULT '00:00:00',
        end_time TIME DEFAULT '00:00:00',
        updated_at DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
    )`);

    // Device commands table
    await pool.query(`CREATE TABLE IF NOT EXISTS device_commands (
        id INT AUTO_INCREMENT PRIMARY KEY,
        device_id VARCHAR(50),
        command VARCHAR(255),
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    )`);

    // Heartbeat settings table
    await pool.query(`CREATE TABLE IF NOT EXISTS heartbeat_settings (
        id INT AUTO_INCREMENT PRIMARY KEY,
        device_id VARCHAR(50) UNIQUE,
        interval_seconds INT DEFAULT 60,
        updated_at DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
    )`);

    // Device-specific HMAC keys table
    await pool.query(`CREATE TABLE IF NOT EXISTS device_keys (
        id INT AUTO_INCREMENT PRIMARY KEY,
        device_id VARCHAR(50) UNIQUE,
        secret_key VARCHAR(255),
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP
    )`);



    console.log("Connected to MySQL database and initialized tables.");
  } catch (err) {
    console.error("Database initialization failed:", err.message);
  }
}

initDB();

// ═══════════════════════════════════════════════════════════════════
// NONCE REPLAY PROTECTION
// ═══════════════════════════════════════════════════════════════════

const usedNonces = new Map();

setInterval(
  () => {
    const now = Date.now();
    for (const [nonce, expiry] of usedNonces) {
      if (now > expiry) usedNonces.delete(nonce);
    }
  },
  5 * 60 * 1000,
);

// ═══════════════════════════════════════════════════════════════════
// HELPER FUNCTIONS (Refactored to async/await)
// ═══════════════════════════════════════════════════════════════════

function getServerTimeGMT7() {
  const now = new Date();
  const gmt7 = new Date(now.getTime() + 7 * 60 * 60 * 1000);
  return gmt7.toISOString().slice(11, 19);
}

async function getSilentMode(deviceId) {
  const [rows] = await pool.query(
    `SELECT * FROM silent_mode_settings WHERE device_id = ?`,
    [deviceId],
  );

  let settings = {
    enabled: false,
    start_time: "00:00:00",
    end_time: "00:00:00",
  };
  let isActive = false;

  if (rows.length > 0) {
    settings = {
      enabled: !!rows[0].enabled,
      start_time: rows[0].start_time,
      end_time: rows[0].end_time,
    };

    if (settings.enabled) {
      const now = new Date();
      const gmt7 = new Date(now.getTime() + 7 * 60 * 60 * 1000);
      const currentTime = gmt7.toISOString().slice(11, 19);

      const start = settings.start_time;
      const end = settings.end_time;

      if (start <= end) {
        isActive = currentTime >= start && currentTime <= end;
      } else {
        isActive = currentTime >= start || currentTime <= end;
      }
    }
  }

  return { ...settings, is_active: isActive };
}

async function getHeartbeatInterval(deviceId) {
  const [rows] = await pool.query(
    `SELECT * FROM heartbeat_settings WHERE device_id = ?`,
    [deviceId],
  );
  if (rows.length > 0) {
    return { interval_seconds: rows[0].interval_seconds };
  }
  return { interval_seconds: 60 };
}

async function getAndConsumeCommand(deviceId) {
  const [rows] = await pool.query(
    `SELECT * FROM device_commands WHERE device_id = ? ORDER BY created_at DESC LIMIT 1`,
    [deviceId],
  );
  // Delete ALL queued commands for this device (prevents stacking)
  await pool.query(`DELETE FROM device_commands WHERE device_id = ?`, [deviceId]);
  if (rows.length > 0) {
    return rows[0].command;
  }
  return null;
}

// ═══════════════════════════════════════════════════════════════════
// HMAC-SHA256 MIDDLEWARE (Refactored)
// ═══════════════════════════════════════════════════════════════════

const verifyHMAC = async (req, res, next) => {
  const deviceId = req.headers["x-device-id"];
  const timestamp = req.headers["x-timestamp"];
  const nonce = req.headers["x-nonce"];
  const signature = req.headers["x-signature"];

  if (!deviceId || !timestamp || !nonce || !signature) {
    return res.status(401).json({
      success: false,
      message:
        "Missing headers: X-Device-ID, X-Timestamp, X-Nonce, X-Signature",
    });
  }

  if (!/^ARDPB\d{4}$/.test(deviceId)) {
    return res
      .status(401)
      .json({ success: false, message: "Invalid device ID format" });
  }
  if (!/^\d+$/.test(timestamp)) {
    return res
      .status(401)
      .json({ success: false, message: "Invalid timestamp format" });
  }
  if (!/^[a-f0-9]{8}$/i.test(nonce)) {
    return res
      .status(401)
      .json({ success: false, message: "Invalid nonce format" });
  }
  if (!/^[a-f0-9]{64}$/i.test(signature)) {
    return res
      .status(401)
      .json({ success: false, message: "Invalid signature format" });
  }

  if (usedNonces.has(nonce)) {
    return res
      .status(403)
      .json({ success: false, message: "Nonce already used" });
  }

  try {
    const [rows] = await pool.query(
      `SELECT secret_key FROM device_keys WHERE device_id = ?`,
      [deviceId],
    );
    const key =
      rows.length > 0 && rows[0].secret_key
        ? rows[0].secret_key
        : HMAC_SECRET_KEY;

    const rawBody = JSON.stringify(req.body);
    const message = `${deviceId}${timestamp}${nonce}${rawBody}`;

    const expectedSignature = crypto
      .createHmac("sha256", Buffer.from(key, "hex"))
      .update(message)
      .digest("hex");

    if (signature !== expectedSignature) {
      return res
        .status(403)
        .json({ success: false, message: "Invalid signature" });
    }

    usedNonces.set(nonce, Date.now() + 10 * 60 * 1000);
    next();
  } catch (err) {
    res.status(500).json({ success: false, message: "Auth database error" });
  }
};

// ═══════════════════════════════════════════════════════════════════
// PROTECTED ENDPOINTS
// ═══════════════════════════════════════════════════════════════════

app.post("/api/heartbeat", verifyHMAC, async (req, res) => {
  const {
    device_id,
    timestamp,
    led_red,
    led_yellow,
    led_green,
    panic_button,
    sirene,
    rotator,
    panic_state,
  } = req.body;

  if (!device_id)
    return res
      .status(400)
      .json({ success: false, message: "device_id is required" });

  const ip = req.ip;
  const deviceStatus =
    panic_state || sirene || rotator || led_red ? "panic" : "normal";

  try {
    await pool.query(
      `INSERT INTO devices (device_id, status, device_ip, last_seen)
       VALUES (?, ?, ?, NOW())
       ON DUPLICATE KEY UPDATE status=VALUES(status), device_ip=VALUES(device_ip), last_seen=NOW()`,
      [device_id, deviceStatus, ip],
    );

    await pool.query(
      `INSERT INTO heartbeats (device_id, timestamp, device_ip, led_red, led_yellow, led_green, panic_button, sirene, rotator, panic_state)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      [
        device_id,
        timestamp || 0,
        ip,
        led_red ? 1 : 0,
        led_yellow ? 1 : 0,
        led_green ? 1 : 0,
        panic_button ? 1 : 0,
        sirene ? 1 : 0,
        rotator ? 1 : 0,
        panic_state ? 1 : 0,
      ],
    );

    const silentMode = await getSilentMode(device_id);
    const heartbeatInterval = await getHeartbeatInterval(device_id);
    const command = await getAndConsumeCommand(device_id);

    res.json({
      command,
      success: true,
      message: "Heartbeat received",
      device_id,
      status: "online",
      server_time_gmt7: getServerTimeGMT7(),
      silent_mode: silentMode,
      heartbeat_interval: heartbeatInterval,
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.post("/api/panic", verifyHMAC, async (req, res) => {
  const {
    device_id,
    timestamp,
    led_red,
    led_yellow,
    led_green,
    panic_button,
    sirene,
    rotator,
    panic_state,
  } = req.body;

  if (!device_id)
    return res
      .status(400)
      .json({ success: false, message: "device_id is required" });

  const ip = req.ip;

  try {
    await pool.query(
      `UPDATE devices SET status = 'panic', last_seen = NOW(), device_ip = ? WHERE device_id = ?`,
      [ip, device_id],
    );

    await pool.query(
      `INSERT INTO heartbeats (device_id, timestamp, device_ip, led_red, led_yellow, led_green, panic_button, sirene, rotator, panic_state)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      [
        device_id,
        timestamp || 0,
        ip,
        led_red ? 1 : 0,
        led_yellow ? 1 : 0,
        led_green ? 1 : 0,
        panic_button ? 1 : 0,
        sirene ? 1 : 0,
        rotator ? 1 : 0,
        panic_state ? 1 : 0,
      ],
    );

    const [result] = await pool.query(
      `INSERT INTO panic_events (device_id, status, timestamp, device_ip) VALUES (?, 'panic', ?, ?)`,
      [device_id, timestamp || 0, ip],
    );

    const silentMode = await getSilentMode(device_id);
    const heartbeatInterval = await getHeartbeatInterval(device_id);
    const command = await getAndConsumeCommand(device_id);

    res.json({
      success: true,
      message: "Panic event received",
      event_id: result.insertId,
      device_id,
      server_time_gmt7: getServerTimeGMT7(),
      silent_mode: silentMode,
      heartbeat_interval: heartbeatInterval,
      command,
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.post("/api/panicoff", verifyHMAC, async (req, res) => {
  const {
    device_id,
    timestamp,
    led_red,
    led_yellow,
    led_green,
    panic_button,
    sirene,
    rotator,
    panic_state,
  } = req.body;

  if (!device_id)
    return res
      .status(400)
      .json({ success: false, message: "device_id is required" });

  const ip = req.ip;

  try {
    await pool.query(
      `UPDATE devices SET status = 'normal', last_seen = NOW(), device_ip = ? WHERE device_id = ?`,
      [ip, device_id],
    );

    await pool.query(
      `INSERT INTO heartbeats (device_id, timestamp, device_ip, led_red, led_yellow, led_green, panic_button, sirene, rotator, panic_state)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      [
        device_id,
        timestamp || 0,
        ip,
        led_red ? 1 : 0,
        led_yellow ? 1 : 0,
        led_green ? 1 : 0,
        panic_button ? 1 : 0,
        sirene ? 1 : 0,
        rotator ? 1 : 0,
        panic_state ? 1 : 0,
      ],
    );

    const [result] = await pool.query(
      `INSERT INTO panic_events (device_id, status, timestamp, device_ip) VALUES (?, 'off', ?, ?)`,
      [device_id, timestamp || 0, ip],
    );

    const silentMode = await getSilentMode(device_id);
    const heartbeatInterval = await getHeartbeatInterval(device_id);
    const command = await getAndConsumeCommand(device_id);

    res.json({
      success: true,
      message: "Panic OFF received",
      event_id: result.insertId,
      device_id,
      server_time_gmt7: getServerTimeGMT7(),
      silent_mode: silentMode,
      heartbeat_interval: heartbeatInterval,
      command,
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

// ═══════════════════════════════════════════════════════════════════
// PUBLIC ENDPOINTS
// ═══════════════════════════════════════════════════════════════════

app.get("/api/devices", async (req, res) => {
  try {
    const [rows] = await pool.query(`
      SELECT d.*, 
      TIMESTAMPDIFF(SECOND, d.last_seen, NOW()) as seconds_ago,
      h.led_red, h.led_yellow, h.led_green, h.sirene, h.rotator, h.panic_button, h.panic_state
      FROM devices d
      LEFT JOIN (
          SELECT device_id, led_red, led_yellow, led_green, sirene, rotator, panic_button, panic_state
          FROM heartbeats h1
          WHERE id = (SELECT MAX(id) FROM heartbeats h2 WHERE h1.device_id = h2.device_id)
      ) h ON d.device_id = h.device_id
      ORDER BY d.created_at ASC
    `);

    let online = 0,
      offline = 0,
      panic = 0;
    const devices = rows.map((row) => {
      const isOnline = row.seconds_ago !== null && row.seconds_ago < 90;
      if (!isOnline) offline++;
      else if (row.status === "panic") panic++;
      else online++;
      return { ...row, isOnline };
    });

    res.json({
      success: true,
      count: devices.length,
      stats: { online, offline, panic },
      devices,
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.delete("/api/devices/:device_id", async (req, res) => {
  const { device_id } = req.params;

  try {
    const [devices] = await pool.query(
      `SELECT * FROM devices WHERE device_id = ?`,
      [device_id],
    );
    if (devices.length === 0) {
      return res
        .status(404)
        .json({ success: false, message: "Device not found" });
    }

    await pool.query(`DELETE FROM heartbeats WHERE device_id = ?`, [device_id]);
    await pool.query(`DELETE FROM panic_events WHERE device_id = ?`, [
      device_id,
    ]);
    await pool.query(`DELETE FROM silent_mode_settings WHERE device_id = ?`, [
      device_id,
    ]);
    await pool.query(`DELETE FROM device_commands WHERE device_id = ?`, [
      device_id,
    ]);
    await pool.query(`DELETE FROM heartbeat_settings WHERE device_id = ?`, [
      device_id,
    ]);
    await pool.query(`DELETE FROM device_keys WHERE device_id = ?`, [
      device_id,
    ]);
    await pool.query(`DELETE FROM devices WHERE device_id = ?`, [device_id]);

    res.json({
      success: true,
      message: `Device ${device_id} and all related data deleted`,
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.get("/api/status/:device_id", async (req, res) => {
  try {
    const [rows] = await pool.query(
      `SELECT * FROM devices WHERE device_id = ?`,
      [req.params.device_id],
    );
    if (rows.length === 0)
      return res
        .status(404)
        .json({ success: false, message: "Device not found" });
    res.json({ success: true, device: rows[0] });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.get("/api/panic/active", async (req, res) => {
  try {
    const [rows] = await pool.query(
      `SELECT * FROM devices WHERE status = 'panic'`,
    );
    res.json({ success: true, count: rows.length, devices: rows });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.get("/api/events", async (req, res) => {
  const limit = Math.min(Math.max(parseInt(req.query.limit) || 100, 1), 1000);
  try {
    const [rows] = await pool.query(
      `SELECT * FROM panic_events ORDER BY received_at DESC LIMIT ?`,
      [limit],
    );
    res.json({ success: true, count: rows.length, events: rows });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.get("/api/events/:device_id", async (req, res) => {
  const limit = Math.min(Math.max(parseInt(req.query.limit) || 100, 1), 1000);
  try {
    const [rows] = await pool.query(
      `SELECT * FROM panic_events WHERE device_id = ? ORDER BY received_at DESC LIMIT ?`,
      [req.params.device_id, limit],
    );
    res.json({
      success: true,
      device_id: req.params.device_id,
      count: rows.length,
      events: rows,
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.get("/api/heartbeat/history/:device_id", async (req, res) => {
  const limit = Math.min(Math.max(parseInt(req.query.limit) || 50, 1), 1000);
  try {
    const [rows] = await pool.query(
      `SELECT * FROM heartbeats WHERE device_id = ? ORDER BY received_at DESC LIMIT ?`,
      [req.params.device_id, limit],
    );
    res.json({
      success: true,
      device_id: req.params.device_id,
      count: rows.length,
      heartbeats: rows,
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

// ═══════════════════════════════════════════════════════════════════
// SILENT MODE ENDPOINTS
// ═══════════════════════════════════════════════════════════════════

app.post("/api/silent-mode", async (req, res) => {
  const { device_id, enabled, start_time, end_time } = req.body;

  if (!device_id || enabled === undefined || !start_time || !end_time) {
    return res
      .status(400)
      .json({ success: false, message: "Missing required fields" });
  }

  try {
    await pool.query(
      `INSERT IGNORE INTO devices (device_id, status, last_seen) VALUES (?, 'normal', NOW())`,
      [device_id],
    );

    await pool.query(
      `INSERT INTO silent_mode_settings (device_id, enabled, start_time, end_time, updated_at)
       VALUES (?, ?, ?, ?, NOW())
       ON DUPLICATE KEY UPDATE enabled=VALUES(enabled), start_time=VALUES(start_time), end_time=VALUES(end_time), updated_at=NOW()`,
      [device_id, enabled ? 1 : 0, start_time, end_time],
    );

    res.json({
      success: true,
      message: "Silent mode configured",
      silent_mode: {
        device_id,
        enabled: enabled ? 1 : 0,
        start_time,
        end_time,
      },
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.get("/api/silent-mode/:device_id", async (req, res) => {
  try {
    const [rows] = await pool.query(
      `SELECT * FROM silent_mode_settings WHERE device_id = ?`,
      [req.params.device_id],
    );
    if (rows.length === 0) {
      return res.json({
        success: true,
        device_id: req.params.device_id,
        silent_mode: null,
        message: "No silent mode configured",
      });
    }
    res.json({
      success: true,
      device_id: req.params.device_id,
      silent_mode: rows[0],
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.patch("/api/silent-mode/:device_id", async (req, res) => {
  const { device_id } = req.params;
  const updates = req.body;

  try {
    const [rows] = await pool.query(
      `SELECT * FROM silent_mode_settings WHERE device_id = ?`,
      [device_id],
    );
    if (rows.length === 0)
      return res
        .status(404)
        .json({ success: false, message: "No silent mode settings found" });

    const enabled =
      updates.enabled !== undefined
        ? updates.enabled
          ? 1
          : 0
        : rows[0].enabled;
    const start_time = updates.start_time || rows[0].start_time;
    const end_time = updates.end_time || rows[0].end_time;

    await pool.query(
      `UPDATE silent_mode_settings SET enabled = ?, start_time = ?, end_time = ? WHERE device_id = ?`,
      [enabled, start_time, end_time, device_id],
    );

    const [updated] = await pool.query(
      `SELECT * FROM silent_mode_settings WHERE device_id = ?`,
      [device_id],
    );
    res.json({
      success: true,
      message: "Silent mode updated",
      silent_mode: updated[0],
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.delete("/api/silent-mode/:device_id", async (req, res) => {
  try {
    await pool.query(`DELETE FROM silent_mode_settings WHERE device_id = ?`, [
      req.params.device_id,
    ]);
    res.json({
      success: true,
      message: `Silent mode deleted for device ${req.params.device_id}`,
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

// ═══════════════════════════════════════════════════════════════════
// HEARTBEAT INTERVAL ENDPOINTS
// ═══════════════════════════════════════════════════════════════════

app.get("/api/heartbeat-interval/:device_id", async (req, res) => {
  try {
    const [rows] = await pool.query(
      `SELECT * FROM heartbeat_settings WHERE device_id = ?`,
      [req.params.device_id],
    );
    if (rows.length === 0) {
      return res.json({
        success: true,
        device_id: req.params.device_id,
        heartbeat_interval: { interval_seconds: 60 },
      });
    }
    res.json({
      success: true,
      device_id: req.params.device_id,
      heartbeat_interval: rows[0],
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.patch("/api/heartbeat-interval/:device_id", async (req, res) => {
  const { device_id } = req.params;
  const { interval_seconds } = req.body;

  if (
    !interval_seconds ||
    typeof interval_seconds !== "number" ||
    interval_seconds < 5
  ) {
    return res.status(400).json({
      success: false,
      message: "interval_seconds must be a number (min 5)",
    });
  }

  try {
    await pool.query(
      `INSERT INTO heartbeat_settings (device_id, interval_seconds, updated_at)
       VALUES (?, ?, NOW())
       ON DUPLICATE KEY UPDATE interval_seconds=VALUES(interval_seconds), updated_at=NOW()`,
      [device_id, interval_seconds],
    );

    const [updated] = await pool.query(
      `SELECT * FROM heartbeat_settings WHERE device_id = ?`,
      [device_id],
    );
    res.json({
      success: true,
      message: `Heartbeat interval updated`,
      heartbeat_interval: updated[0],
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

// ═══════════════════════════════════════════════════════════════════
// DASHBOARD ENDPOINTS
// ═══════════════════════════════════════════════════════════════════

app.post("/api/dashboard/panic", async (req, res) => {
  const { device_id } = req.body;
  if (!device_id)
    return res
      .status(400)
      .json({ success: false, message: "device_id is required" });

  try {
    await pool.query(
      `UPDATE devices SET status = 'panic' WHERE device_id = ?`,
      [device_id],
    );
    const [result] = await pool.query(
      `INSERT INTO panic_events (device_id, status, timestamp, device_ip) VALUES (?, 'panic', ?, 'dashboard')`,
      [device_id, Date.now()],
    );
    await pool.query(
      `INSERT INTO device_commands (device_id, command) VALUES (?, 'panic')`,
      [device_id],
    );

    res.json({
      success: true,
      message: "Panic triggered from dashboard",
      event_id: result.insertId,
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

app.post("/api/dashboard/reset", async (req, res) => {
  const { device_id } = req.body;
  if (!device_id)
    return res
      .status(400)
      .json({ success: false, message: "device_id is required" });

  try {
    await pool.query(
      `UPDATE devices SET status = 'normal' WHERE device_id = ?`,
      [device_id],
    );
    const [result] = await pool.query(
      `INSERT INTO panic_events (device_id, status, timestamp, device_ip) VALUES (?, 'off', ?, 'dashboard')`,
      [device_id, Date.now()],
    );
    await pool.query(
      `INSERT INTO device_commands (device_id, command) VALUES (?, 'reset')`,
      [device_id],
    );

    res.json({
      success: true,
      message: "Panic reset from dashboard",
      event_id: result.insertId,
    });
  } catch (err) {
    res.status(500).json({ success: false, error: err.message });
  }
});

// ═══════════════════════════════════════════════════════════════════
// START SERVER
// ═══════════════════════════════════════════════════════════════════

app.listen(PORT, "0.0.0.0", () => {
  console.log(`╔═══════════════════════════════════════════════╗`);
  console.log(`║  Panic Button IoT Server                     ║`);
  console.log(`║  Running at http://localhost:${PORT}             ║`);
  console.log(`║  MySQL Connected & HMAC Auth Enabled         ║`);
  console.log(`╚═══════════════════════════════════════════════╝`);
});

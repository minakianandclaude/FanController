# VanFan Controller API Reference

REST API for controlling the ESP32-S3 roof fan via HTTP. All endpoints are JSON-based.

**Base URL:** `http://vanfan.local` (mDNS) or `http://<device-ip>`
**Port:** 80

## Discovery

The controller advertises itself via mDNS:
- **Hostname:** `vanfan.local`
- **Service:** `_http._tcp` on port 80
- **Instance name:** "VanFan Controller"

Verify connectivity: `ping vanfan.local`

---

## Endpoints

### GET /api/v1/status

Returns the current fan state.

**Response:**
```json
{
  "running": false,
  "speed": 20,
  "direction": "exhaust",
  "mode": "manual"
}
```

| Field       | Type    | Description                              |
|-------------|---------|------------------------------------------|
| `running`   | boolean | Whether the fan is currently on          |
| `speed`     | integer | Current speed setting (1-100)            |
| `direction` | string  | `"exhaust"` or `"intake"`                |
| `mode`      | string  | `"manual"` (only supported mode)         |

---

### POST /api/v1/speed

Set the fan speed. Turns the fan on if it's off.

**Request:**
```json
{
  "speed": 75
}
```

| Field   | Type    | Required | Constraints |
|---------|---------|----------|-------------|
| `speed` | integer | yes      | 1-100       |

**Response:** Updated fan state (same format as `/api/v1/status`).

**Errors:**
- `400` — Missing or non-numeric `speed` field
- `422` — Speed out of range (< 1 or > 100)

---

### POST /api/v1/direction

Set the fan direction. The motor ramps down, coasts for 2 seconds, then ramps up in the new direction.

**Request:**
```json
{
  "direction": "intake"
}
```

| Field       | Type   | Required | Values                      |
|-------------|--------|----------|-----------------------------|
| `direction` | string | yes      | `"exhaust"` or `"intake"`   |

**Response:** Updated fan state.

**Errors:**
- `400` — Missing or non-string `direction` field
- `422` — Invalid direction value

---

### POST /api/v1/mode

Set the operating mode.

**Request:**
```json
{
  "mode": "manual"
}
```

| Field  | Type   | Required | Values       |
|--------|--------|----------|--------------|
| `mode` | string | yes      | `"manual"`   |

**Response:** Updated fan state.

**Errors:**
- `400` — Missing or non-string `mode` field
- `422` — Unsupported mode

---

### POST /api/v1/set

Combined endpoint — set multiple parameters atomically in a single request. Turns the fan on if it's off and at least one field is provided.

**Request:**
```json
{
  "speed": 50,
  "direction": "exhaust"
}
```

All fields are optional. Only provided fields are changed.

| Field       | Type    | Required | Constraints                 |
|-------------|---------|----------|-----------------------------|
| `speed`     | integer | no       | 1-100                       |
| `direction` | string  | no       | `"exhaust"` or `"intake"`   |

**Response:** Updated fan state.

**Errors:**
- `400` — Invalid JSON
- `422` — Out-of-range speed or invalid direction

**Notes:**
- Sending `{}` returns current state without changes
- This is the recommended endpoint for the Pi app — set speed and direction in one call

---

### POST /api/v1/toggle

Toggle the fan on/off. When turning on, resumes the last speed and direction.

**Request:** Empty body or `{}`.

**Response:** Updated fan state.

---

### POST /api/v1/stop

Emergency stop. Immediately brakes the motor (no ramp).

**Request:** Empty body or `{}`.

**Response:** Updated fan state with `"running": false`.

---

### GET /api/v1/events

Server-Sent Events (SSE) stream. Streams real-time fan state changes.

**Headers sent:**
```
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive
```

**Event format:**
```
data: {"running":true,"speed":40,"direction":"exhaust","mode":"manual","source":"button"}

```

Each event is a JSON object on a single `data:` line, followed by two newlines.

| Field    | Type   | Description                                      |
|----------|--------|--------------------------------------------------|
| `source` | string | What triggered the change: `"button"`, `"api"`, or `"startup"` |

**Behavior:**
- Sends the current state immediately on connection
- Streams updates whenever the fan state changes (from buttons or API)
- Sends `: keepalive` comments every 15 seconds to prevent timeout
- Maximum 4 concurrent SSE clients
- Dead clients are automatically removed on failed sends

**Example (curl):**
```bash
curl -N http://vanfan.local/api/v1/events
```

**Example (Python):**
```python
import requests

with requests.get('http://vanfan.local/api/v1/events', stream=True) as r:
    for line in r.iter_lines(decode_unicode=True):
        if line.startswith('data: '):
            state = json.loads(line[6:])
            print(f"Fan: {state}")
```

---

### POST /api/v1/ota/update

Upload new firmware binary for over-the-air update. *(Available in Phase 5)*

**Status:** Returns `501 Not Implemented`.

**Future usage:**
```bash
curl -X POST --data-binary @firmware/vanfan.bin http://vanfan.local/api/v1/ota/update
```

---

### GET /api/v1/info

Returns device information.

**Response:**
```json
{
  "version": "0.1.0",
  "uptime_s": 3600,
  "free_heap": 350000,
  "chip": {
    "model": "esp32s3",
    "cores": 2,
    "revision": 0.01
  }
}
```

| Field       | Type    | Description                    |
|-------------|---------|--------------------------------|
| `version`   | string  | Firmware version               |
| `uptime_s`  | integer | Seconds since boot             |
| `free_heap` | integer | Free heap memory in bytes      |
| `chip`      | object  | Chip hardware info             |

---

## Error Responses

All errors return JSON:
```json
{
  "error": 400,
  "message": "Invalid JSON"
}
```

| Code | Meaning                  |
|------|--------------------------|
| 400  | Bad request (malformed JSON, missing required field) |
| 422  | Validation error (out-of-range value, invalid enum)  |
| 501  | Not implemented (OTA stub)                           |

---

## Integration Notes for Pi Application

### Recommended Patterns

1. **Use `/api/v1/set` for control** — it handles speed + direction atomically and auto-starts the fan.

2. **Use SSE for state sync** — subscribe to `/api/v1/events` to keep the Pi UI in sync with both API and physical button changes. No polling needed.

3. **Handle disconnects** — the ESP32 may restart (OTA, power cycle). Reconnect SSE with a short delay. On reconnect, the first event contains the full current state.

4. **Speed values** — buttons cycle through 20/40/60/80/100, but the API accepts any value 1-100 for finer control.

5. **Direction changes are slow** — a direction flip involves ramping down (up to 5s), coasting 2s, then ramping up (up to 2s). The API responds immediately with the target state; the motor catches up physically.

### Startup Sequence

```
1. Discover device via mDNS (vanfan.local) or known IP
2. GET /api/v1/info — verify device is reachable, check version
3. GET /api/v1/status — get initial state
4. Connect to /api/v1/events — subscribe to real-time updates
5. Use POST endpoints for control as needed
```

### WiFi Resilience

- The fan continues running if WiFi drops — buttons always work
- The ESP32 auto-reconnects with exponential backoff (1s to 60s)
- SSE connections will drop on WiFi loss — reconnect when the device is reachable again

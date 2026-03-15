#!/usr/bin/env python3
"""Test BLE provisioning - discover characteristics and test protocol."""
import asyncio
import struct
from bleak import BleakClient, BleakScanner


def encode_varint(value):
    result = bytearray()
    while value > 0x7F:
        result.append((value & 0x7F) | 0x80)
        value >>= 7
    result.append(value & 0x7F)
    return bytes(result)


def encode_varint_field(field_number, value):
    tag = encode_varint((field_number << 3) | 0)
    val = encode_varint(value)
    return tag + val


def encode_bytes_field(field_number, data):
    tag = encode_varint((field_number << 3) | 2)
    length = encode_varint(len(data))
    return tag + length + data


def build_session_request():
    """Security0 session command."""
    sec0_payload = encode_varint_field(1, 0)  # msg = S0_Session_Command
    sec_ver = encode_varint_field(2, 0)       # sec_ver = SecScheme0
    sec0_field = encode_bytes_field(10, sec0_payload)  # sec0 oneof
    return sec_ver + sec0_field


def build_config_request(ssid, password):
    """WiFi config set command."""
    cmd_set_config = (
        encode_bytes_field(1, ssid.encode()) +
        encode_bytes_field(2, password.encode())
    )
    return (
        encode_varint_field(1, 2) +            # msg = TypeCmdSetConfig = 2
        encode_bytes_field(12, cmd_set_config)  # cmd_set_config (field 12)
    )


def build_apply_request():
    """WiFi apply config command."""
    return encode_varint_field(1, 4)  # msg = TypeCmdApplyConfig = 4


async def main():
    print("Scanning for VANFAN...")
    device = await BleakScanner.find_device_by_name("VANFAN", timeout=10)
    if not device:
        print("Device not found!")
        return

    print(f"Found: {device.name} ({device.address})")

    async with BleakClient(device) as client:
        print(f"Connected: {client.is_connected}")
        print(f"MTU: {client.mtu_size}")
        print()

        # Enumerate all services and characteristics
        endpoint_map = {}
        for service in client.services:
            print(f"Service: {service.uuid}")
            for char in service.characteristics:
                props = ", ".join(char.properties)
                print(f"  Char: {char.uuid} [{props}]")
                for desc in char.descriptors:
                    try:
                        val = await client.read_gatt_descriptor(desc.handle)
                        desc_str = val.decode('utf-8', errors='replace')
                        print(f"    Desc {desc.uuid}: {desc_str}")
                        if desc.uuid == "00002901-0000-1000-8000-00805f9b34fb":
                            endpoint_map[desc_str] = char
                    except Exception as e:
                        print(f"    Desc {desc.uuid}: (error: {e})")

        print()
        print(f"Endpoint map: {list(endpoint_map.keys())}")

        if 'prov-session' not in endpoint_map or 'prov-config' not in endpoint_map:
            print("ERROR: Could not find prov-session and/or prov-config endpoints!")
            return

        session_char = endpoint_map['prov-session']
        config_char = endpoint_map['prov-config']

        print(f"\nprov-session: {session_char.uuid}")
        print(f"prov-config:  {config_char.uuid}")

        # Step 1: Security0 session
        print("\n--- Step 1: Session request ---")
        session_req = build_session_request()
        print(f"Writing {len(session_req)} bytes: {session_req.hex()}")
        await client.write_gatt_char(session_char, session_req, response=True)
        session_resp = await client.read_gatt_char(session_char)
        print(f"Response: {session_resp.hex()}")

        # Step 2: WiFi config
        ssid = "RAGNAR"
        password = "test1234"  # placeholder
        print(f"\n--- Step 2: Set config (SSID={ssid}) ---")
        config_req = build_config_request(ssid, password)
        print(f"Writing {len(config_req)} bytes: {config_req.hex()}")
        await client.write_gatt_char(config_char, config_req, response=True)
        config_resp = await client.read_gatt_char(config_char)
        print(f"Response: {config_resp.hex()}")

        # Step 3: Apply config
        print("\n--- Step 3: Apply config ---")
        apply_req = build_apply_request()
        print(f"Writing {len(apply_req)} bytes: {apply_req.hex()}")
        await client.write_gatt_char(config_char, apply_req, response=True)
        apply_resp = await client.read_gatt_char(config_char)
        print(f"Response: {apply_resp.hex()}")

        print("\nDone! Check serial output for provisioning events.")


if __name__ == "__main__":
    asyncio.run(main())

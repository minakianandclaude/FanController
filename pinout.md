# VanFan Controller — ESP32-S3 Pinout

## BTS7960 Motor Driver

| Signal | GPIO | Direction | Description |
|--------|------|-----------|-------------|
| RPWM   | 5    | Output    | Forward (exhaust) PWM — LEDC CH0, 25kHz, 8-bit |
| LPWM   | 6    | Output    | Reverse (intake) PWM — LEDC CH1, 25kHz, 8-bit |
| R_EN   | 7    | Output    | Right enable — driven HIGH on init |
| L_EN   | 15   | Output    | Left enable — driven HIGH on init |
| R_IS   | 4    | Input     | Right current sense (ADC, Phase 6) |
| L_IS   | 3    | Input     | Left current sense (ADC, Phase 6) |

## Buttons

| Signal    | GPIO | Direction | Description |
|-----------|------|-----------|-------------|
| Speed     | 9    | Input     | Internal pull-up, active low |
| Direction | 10   | Input     | Internal pull-up, active low |

## LED Lighting (MOSFET PWM)

| Signal    | GPIO | Direction | Description |
|-----------|------|-----------|-------------|
| Zone 1    | 11   | Output    | LEDC CH2, Timer 1, 1kHz, 8-bit |
| Zone 2    | 12   | Output    | LEDC CH3, Timer 1, 1kHz, 8-bit |
| Zone 3    | 13   | Output    | LEDC CH4, Timer 1, 1kHz, 8-bit |

## Light Button

| Signal       | GPIO | Direction | Description |
|--------------|------|-----------|-------------|
| Light button | 14   | Input     | Internal pull-up, active low |

## Reserved / In Use by System

| GPIO | Function |
|------|----------|
| 19   | USB_D- (USB-Serial/JTAG) |
| 20   | USB_D+ (USB-Serial/JTAG) |

## Pin Configuration

All pin assignments are configurable via Kconfig under `VanFan Controller` menu. Defaults defined in `main/Kconfig.projbuild`.

# Clauled

A small desk gadget built on an ESP32-C3 with an OLED screen that shows your Claude subscription usage in real time. It reads straight from the Anthropic API response headers, the same numbers behind the usage page on claude.ai.

## Why

The claude.ai usage page is fine, but you have to go look at it. I wanted a thing on my desk that just shows me at a glance how much headroom I have left. So now there is a little screen that updates every minute and I never have to think about it.

## What you need

- ESP32-C3 Mini (also sold as ESP32-C3 SuperMini)
- SSD1306 OLED, 128x64 pixels, I2C. These are the common ones with the blue and yellow split screen. Important: it needs to be the SSD1306 controller, not the SH1106. They look the same from the outside but need different drivers.
- Four jumper wires
- USB-C cable
- A Claude subscription (Pro, Max, Team, or Enterprise)

## Wiring

| OLED pin | ESP32-C3 pin |
|----------|--------------|
| GND      | GND          |
| VCC      | 3.3V         |
| SDA      | GPIO 4       |
| SCL      | GPIO 5       |

## Getting your token

The device authenticates using a Claude Code OAuth token. To get one:

1. Install Claude Code if you do not have it yet: `npm install -g @anthropic-ai/claude-code`
2. Run `claude setup-token` in a terminal
3. Log in through the browser when it opens
4. Copy the token it prints to your terminal

The token starts with `sk-ant-oat01-` and stays valid for a year. Copy it the moment you see it because it is not saved anywhere you can retrieve it from later.

Requires a Claude Pro, Max, Team, or Enterprise subscription.

## Setup

Two options. Pick whichever suits you.

### Option A: no coding needed

1. Flash the firmware (see the Flashing section below)
2. On first boot the device creates a WiFi network called `ClaudeMonitor`. Connect to it from your phone or laptop.
3. Open `192.168.4.1` in your browser
4. Enter your home WiFi name and password, then paste your token
5. Hit save. The device reboots, connects to your network, and starts showing usage.

The config page stays available at the device IP address shown in the footer of the screen, so you can always go back and change things.

### Option B: hardcode everything before flashing

Open `src/main.cpp` and fill in the values near the top:

```cpp
#define WIFI_SSID       "MyHomeNetwork"
#define WIFI_PASSWORD   "supersecret"
#define OAUTH_TOKEN     "sk-ant-oat01-..."
#define OAUTH_CLIENT_ID "9d1c250a-..."
```

The WiFi pair is what matters for skipping the portal. If both SSID and password are set, the device connects straight to your network on boot. The token can be left blank and added later through the browser. If the screen says "Add your token" after connecting, that is what it is waiting for.

To get the `OAUTH_CLIENT_ID`, run this in a terminal on any machine that has Claude Code installed:

```bash
node -e "
const fs = require('fs');
const b = fs.readFileSync(require('which').sync('claude'));
const m = b.toString('latin1').match(/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/g);
const id = m && m.find(x => x.startsWith('9d1c'));
console.log(id || 'not found');
"
```

This is the Claude Code application ID, not yours. Every installation has the same value. It is not a secret, but Anthropic can change it between versions so extracting it from your own binary keeps things in sync. The build will refuse to compile if you leave it blank.

Leave the WiFi fields blank and it falls back to the portal, so both options work from the same build.

> [!WARNING]
> If the token is hardcoded it is stored as plain text in the firmware binary. Fine for your own device, but do not share the compiled binary with anyone else.

## Flashing

This is a PlatformIO project. Open it in VS Code with the PlatformIO extension, or run these commands from a terminal:

```
platformio.exe run --target erase       # wipe the flash clean
platformio.exe run --target uploadfs    # upload the config web page
platformio.exe run --target upload      # upload the firmware
platformio.exe device monitor           # watch serial output (optional)
```

The erase step is worth doing the first time, or any time the device behaves unexpectedly. The device stores its settings in flash, and a leftover file from a previous flash can quietly override your compile-time credentials. If it keeps booting into setup mode when it should not, erase and reflash.

Run `uploadfs` at least once so the config page lands on the device. After that, `upload` is enough unless you change the page.

If the upload fails with a chip mismatch, check your board is set to `esp32-c3-devkitm-1` in PlatformIO.

## What shows on the screen

The screen always shows two bar pages and cycles through them automatically. You can also flip manually with the BOOT button on the board.

```
Current session          16%
####........................
       Resets in 1h 21m
```

Title on the left, percentage on the right, bar fills as usage climbs, and the reset countdown sits centered underneath. The footer shows the next poll countdown on the left and the device IP on the right.

The two main pages are always on:

- Current session (the 5 hour rolling window)
- Weekly, all models (7 day window)

If your plan does not return a usage percentage for a window, the bar shows the reset time instead so you still get something useful.

## Config page

Once the device is on your network, open its IP address in any browser. The page is mobile-friendly and works from your phone.

### Usage

![Usage section](docs/config-usage.png)

At the top of the page you see live usage for both windows. Current session on the left, weekly all models on the right. The percentage and bar update every time you hit Refresh or Poll now. The reset time shows how long until that window resets.

### WiFi

![WiFi section](docs/config-wifi.png)

Enter your home network name and password here. The SSID field pre-fills with the currently saved network. Leave the password blank to keep the existing one. Saving a new WiFi network reboots the device immediately.

### Claude OAuth token

![OAuth section](docs/config-oauth.png)

Paste your `sk-ant-oat01-` token in the access token field. Once saved, the field shows "access token saved" in green so you can confirm it landed. Leave it blank when saving other settings to keep the existing token. The refresh token is optional and only needed if you want the device to renew the access token automatically when it expires.

### Polling and display

![Polling section](docs/config-polling.png)

Poll interval controls how often the device calls the API. Every 60 seconds is the default and costs one minimal API call per minute. Page cycle time controls how quickly the OLED flips between pages. Set it to Manual if you prefer to use the BOOT button yourself.

### Show on screen

![Show on screen section](docs/config-screen.png)

Weekly Sonnet only is for Max plan users who have a separate Sonnet bucket. Device uptime shows how long the device has been running since its last boot. Both are off by default.

### Actions

![Actions section](docs/config-actions.png)

Poll now fires an immediate API call and refreshes the usage display. Refresh reloads the status from the device without polling the API again. Save settings writes your changes to flash. Factory reset wipes everything and returns the device to first-boot setup mode.

## Troubleshooting

**Garbage on screen, or everything shifted sideways by a couple of pixels.** Almost always an SH1106 module, not an SSD1306. This firmware is written for the SSD1306. If you are sure you have an SSD1306 and still see noise, try lowering the I2C clock speed by adding `Wire.setClock(100000);` after `Wire.begin(SDA_PIN, SCL_PIN);` in `src/main.cpp`.

**Screen stays blank.** Flash an I2C scanner sketch and confirm the display responds at `0x3C`. Check VCC is on 3.3V and that SDA and SCL are not swapped.

**Keeps going back to setup mode.** Both SSID and password need to be present. Check the serial monitor on boot for `[cfg] configured=yes`. If it says no, the WiFi details are not sticking. Run the erase step first and try again.

**Screen says "Add your token".** WiFi is working but no token is set. Open the device IP in a browser and paste your `sk-ant-oat01-` token into the config page.

**Bars show dashes instead of numbers.** The token is not working. Open the serial monitor and look for `[poll] 5h=.. 7d=..`. If you see 401 errors, the token is wrong or expired and needs replacing.

**Reset countdown shows dashes.** The device syncs time over NTP when it connects to WiFi. If your network blocks NTP the countdown will not work, but the bars still show whatever data the API returns.

**Save button spins forever when entering WiFi credentials.** When you save new WiFi details from the setup portal, the device reboots and joins your home network. The `ClaudeMonitor` network disappears with it, so the browser never gets a response back. The page handles this and shows "Saved, reconnecting". Reconnect your device to your home WiFi and open the IP shown on the OLED.

## A note on the API

This reads Anthropic's own usage headers from normal API responses. No scraping, no reverse engineering, no workarounds. Just numbers Anthropic already sends back on every request, shown somewhere more convenient.

## License

MIT.
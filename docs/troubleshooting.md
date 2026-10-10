# 🧯 Troubleshooting

[← Back to the README](../README.md)

## Test each piece

| Test | How |
|---|---|
| The fire | In Home Assistant, open the STRETCH Fire device and press **Spark**. |
| Home Assistant + webhook | From any computer: `curl -X POST https://<your-home-assistant>/api/webhook/<your-webhook-id>` |
| The whole chain | Book a class yourself (then cancel it) and watch the fire. |

> [!NOTE]
> Home Assistant replies "OK" to *any* webhook address, valid or not, so a successful `curl` alone doesn't prove anything ran. Check the automation's **Traces** in Home Assistant.

## A booking came in but no sparks

Work down the list:

1. **Is the fire on, running Hearth or Fire2012?** Sparks play on top of the fire, so they don't show while it's off.
2. **Did the booking email arrive?** Look for "New booking for..." in the inbox.
3. **Did the inbox rule fire?** Check its conditions and that the webhook step is still there.
4. **Did the automation run?** Home Assistant → Settings → Automations → your automation → Traces.
5. **Is the fire online?** If its controls show "unavailable", it may be unplugged, or its network address may have changed.
6. **Can the internet reach Home Assistant?** If your internet or proxy is down, outside services can't call the webhook.

## Other problems

| What you see | Likely cause and fix |
|---|---|
| Nothing lights up | Grounds not all connected; data wired to DOUT instead of DIN; level shifter's pin 1 not grounded. |
| Wrong colours (green or blue fire) | Change `rgb_order` in `stretch-fire.yaml` (try `RGB` or `BRG`). |
| Flickers wildly or the board keeps restarting | The power supply is too small, or the wires are too thin. Check the **Uptime** sensor for restarts. |
| Computer doesn't see the board | Charge-only USB cable. Swap it. |
| Fire "unavailable" after a router restart | It got a new network address. Give it a fixed (reserved) address in your router. |
| Sparks look faint | Brightness is low in Home Assistant; the sparks scale with it. |

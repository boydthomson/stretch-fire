# ✨ Making bookings spark the fire

[← Back to the README](../README.md)

The fire sparks whenever Home Assistant presses its **Spark** button. All you need is something that tells Home Assistant "a booking just happened".

## The Home Assistant side (everyone needs this)

1. Make up a long random **webhook ID**. Treat it like a password, because anyone who knows it can make your fire spark:
   ```bash
   python3 -c "import secrets;print('booking-'+secrets.token_urlsafe(18))"
   ```
2. In Home Assistant, go to **Settings → Automations → Create automation → ⋮ → Edit in YAML**, paste [`home-assistant/automation.yaml`](../home-assistant/automation.yaml), and put your ID in `webhook_id`.
3. Your webhook address is:
   ```
   https://<your-home-assistant-address>/api/webhook/<your-webhook-id>
   ```
   Home Assistant has to be reachable from the internet for outside services to call it, for example through [Home Assistant Cloud](https://www.nabucasa.com) or your own reverse proxy.

> [!NOTE]
> The automation is in `queued` mode, so if three bookings land at once, the fire does three bursts in a row rather than one.

## How STRETCH does it: Momence → Missive → Home Assistant

This route uses no booking-system API at all, only the notification emails the booking system already sends.

**1. Turn on booking emails in Momence.**
*Settings → Class settings → Notifications →* "Send me an email when someone signs up for a class". Each booking now sends an email titled **"New booking for CLASS NAME"**.

**2. Add a rule in Missive** (the studio's shared inbox):

| | |
|---|---|
| **When** | an incoming email |
| **From** | `momence@mail.momence.com` |
| **To** | the studio's inbox address |
| **Subject** | starts with `New booking for` |
| **Then** | Webhook → **POST** → your webhook address |

The subject condition matters: the customer's own confirmation email comes from the same sender, but with a different subject, so it won't double-count.

That's it. The fire sparks a second or two after each booking.

## Other ways to connect

| Your setup | How |
|---|---|
| **Booking system has webhooks** | Point its "new booking" webhook straight at your Home Assistant webhook address. |
| **Booking system works with Zapier or Make** | Trigger: *new booking*. Action: *Webhooks → POST* to your webhook address. |
| **Only booking emails, inbox without rules** | Use Home Assistant's [IMAP integration](https://www.home-assistant.io/integrations/imap/) to watch for the subject line, then press the Spark button in an automation. Nothing has to be exposed to the internet. |
| **Want extra logic** (first-timers only, opening hours, logging) | Put [n8n](https://n8n.io) or Node-RED in the middle, or add conditions to the Home Assistant automation. |

## Fancier ideas

- **Bigger celebration for bigger news:** use the `esphome.stretch_fire_spark` action with `count: 3` for a first-time client or a sold-out class.
- **Only during opening hours:** add a time condition to the automation.
- **Wake the fire up:** have the automation turn the light on before sparking, so bookings still show when the fire is off.

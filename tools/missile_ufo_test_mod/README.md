# Coop: Missile UFO (test aid)

Optional, **disabled by default**. Install by copying this folder into your OpenXcom
**user** mods directory, then enable it in *Options -> Mods*:

    <user dir>/mods/Coop_Missile_UFO_Test/

On this machine that is
`C:\Users\bentl\OneDrive\Documents\OpenXcom\mods\Coop_Missile_UFO_Test\`
(the folder that already holds `XComFiles`).

Do **not** put it in `bin/x64/Release/standard/` - that is a protected location with a
hardcoded allowlist of the mods shipped with OXCE; anything else there is rejected with
`Invalid standard mod '<name>', skipping.` in `openxcom.log`.

## Why it exists

`Base::damageFacilities()` — the "partial destruction" path where a UFO **bombards** an
X-Com base from the geoscape (facilities destroyed or replaced, the base *survives*, and
**no battlescape happens at all**) — only runs for a UFO whose rules set
`missilePower != 0`:

* `missilePower > 0` → partial destruction (facilities lost, base survives)
* `missilePower < 0` → nuke (base destroyed outright)

**No vanilla ruleset defines `missilePower`**, so that path is unreachable on vanilla data
and cannot be triggered by the automated harness either. This mod gives retaliation
Battleships and Terror Ships a missile payload so the path can be exercised by hand.

## How to test the JOINT replication

1. Enable the mod, start a **CO-OP (JOINT)** campaign with two players.
2. Get the shared base marked for retaliation (or use the harness
   `spawn_base_assault` command) so a Battleship attacks it.
3. When it hits, the **host** loses facilities and sees "base damaged but survived".
4. The **client** must show the same dialog and its copy of the shared base must lose
   exactly the same facilities — that is the `base_damaged` broadcast working. Before the
   fix the client silently kept the facilities the host had lost.

Note the base needs a garrison of **zero** soldiers/vehicles to be irrelevant here: the
missile branch short-circuits before the garrison check, so it fires regardless.

# ![logo](https://raw.githubusercontent.com/azerothcore/azerothcore.github.io/master/images/logo-github.png) AzerothCore

# Dead Means Dead<br><sub>Creature Respawn Customization Module</sub>

## What is this?
This module allows an AzerothCore server administrator to disable or customize creature respawn times in dungeons, raids, or in the world.

## Defaults
By default, the module will disable respawns in dungeons and raids - "dead" means "**DEAD**". :skull:

## How it works
When a creature spawned from the `creature` table dies, the module computes a new respawn time from the creature's original respawn delay and the configured multipliers, and saves it to `creature_respawn`. That row is what the core reads when the map is created, so the adjusted time survives instance unloads and server restarts. An instance reset clears it, as it clears all respawn timers.

Only the absolute respawn time is changed; the creature's respawn delay (`creature.spawntimesecs`) is never modified, in memory or in the database.

Summons, pets and script-created creatures are ignored. By default, kills made by other NPCs or by the environment are ignored too (`DeadMeansDead.Filter.KilledByPlayer`); kills by a player's pet, guardian, totem or charmed unit count as kills by that player.

## Configuration
If you'd rather respawn times be longer, shorter, or applied in different map/area types, please edit the configuration file. To do this, find the `mod_dead_means_dead.conf.dist` file inside your `etc/modules/` directory. Make a copy of it called `mod_dead_means_dead.conf`. Make all configuration changes in the `.conf` file only.

The configuration options are all well-described inside the configuration file.

You may reload the configuration by issuing the `.reload config` command from a GM character or the `reload config` command from the worldserver console. Note that existing dead creatures will not be updated if you change your settings, but any newly-dead creatures will use the updated values.

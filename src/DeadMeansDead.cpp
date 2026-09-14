/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include <algorithm>
#include <vector>

namespace
{
    // Respawn time used when the multiplier resolves to 0 ("never respawn")
    constexpr uint32 RESPAWN_DISABLED_SECS = 10 * YEAR;

    struct DeadMeansDeadOptions
    {
        bool enable = true;
        bool announce = true;

        bool enableDungeons = true;
        bool enableRaids = true;
        bool enableWorld = false;

        float multiplierGlobal = 1.0f;
        float multiplierDungeon = 0.0f;
        float multiplierRaid = 0.0f;
        float multiplierWorld = 1.0f;

        uint32 respawnTimeOriginalMin = 300;
        uint32 respawnTimeOriginalMax = 86400;
        uint32 respawnTimeAdjustedMin = 300;
        uint32 respawnTimeAdjustedMax = 86400;

        bool filterKilledByPlayer = true;
        std::vector<uint32> filterAlwaysInstanceIDs;
        std::vector<uint32> filterNeverInstanceIDs;
        std::vector<uint32> filterAlwaysCreatureIDs;
        std::vector<uint32> filterNeverCreatureIDs;

        void Load()
        {
            enable = sConfigMgr->GetOption<bool>("DeadMeansDead.Enable", true);
            announce = sConfigMgr->GetOption<bool>("DeadMeansDead.Announce", true);

            enableDungeons = sConfigMgr->GetOption<bool>("DeadMeansDead.Enable.Dungeons", true);
            enableRaids = sConfigMgr->GetOption<bool>("DeadMeansDead.Enable.Raids", true);
            enableWorld = sConfigMgr->GetOption<bool>("DeadMeansDead.Enable.World", false);

            multiplierGlobal = sConfigMgr->GetOption<float>("DeadMeansDead.RespawnTime.Multiplier.Global", 1.0f);
            multiplierDungeon = sConfigMgr->GetOption<float>("DeadMeansDead.RespawnTime.Multiplier.Dungeons", 0.0f);
            multiplierRaid = sConfigMgr->GetOption<float>("DeadMeansDead.RespawnTime.Multiplier.Raids", 0.0f);
            multiplierWorld = sConfigMgr->GetOption<float>("DeadMeansDead.RespawnTime.Multiplier.World", 1.0f);

            respawnTimeOriginalMin = sConfigMgr->GetOption<uint32>("DeadMeansDead.RespawnTime.Original.Min", 300);
            respawnTimeOriginalMax = sConfigMgr->GetOption<uint32>("DeadMeansDead.RespawnTime.Original.Max", 86400);
            respawnTimeAdjustedMin = sConfigMgr->GetOption<uint32>("DeadMeansDead.RespawnTime.Adjusted.Min", 300);
            respawnTimeAdjustedMax = sConfigMgr->GetOption<uint32>("DeadMeansDead.RespawnTime.Adjusted.Max", 86400);

            filterKilledByPlayer = sConfigMgr->GetOption<bool>("DeadMeansDead.Filter.KilledByPlayer", true);

            filterAlwaysInstanceIDs = ParseIdList("DeadMeansDead.Filter.AlwaysAdjust.InstanceID");
            filterNeverInstanceIDs = ParseIdList("DeadMeansDead.Filter.NeverAdjust.InstanceID");
            filterAlwaysCreatureIDs = ParseIdList("DeadMeansDead.Filter.AlwaysAdjust.CreatureID");
            filterNeverCreatureIDs = ParseIdList("DeadMeansDead.Filter.NeverAdjust.CreatureID");
        }

    private:
        // Space-delimited list; tokens that are not valid unsigned integers are
        // reported and skipped
        static std::vector<uint32> ParseIdList(std::string const& option)
        {
            std::string const str = sConfigMgr->GetOption<std::string>(option, "");

            std::vector<uint32> list;
            for (std::string_view token : Acore::Tokenize(str, ' ', false))
            {
                if (Optional<uint32> value = Acore::StringTo<uint32>(token))
                    list.push_back(*value);
                else
                    LOG_ERROR("module", "DeadMeansDead: option {} contains invalid value '{}', ignored", option, token);
            }

            return list;
        }
    };

    DeadMeansDeadOptions options;

    bool Contains(std::vector<uint32> const& list, uint32 value)
    {
        return std::find(list.begin(), list.end(), value) != list.end();
    }

    enum MapType
    {
        MAP_TYPE_DUNGEON,
        MAP_TYPE_RAID,
        MAP_TYPE_BATTLEGROUND_OR_ARENA,
        MAP_TYPE_WORLD
    };

    MapType GetMapType(Map const* map)
    {
        if (map->IsRaid())
            return MAP_TYPE_RAID;
        if (map->IsDungeon())
            return MAP_TYPE_DUNGEON;
        if (map->IsBattlegroundOrArena())
            return MAP_TYPE_BATTLEGROUND_OR_ARENA;
        return MAP_TYPE_WORLD;
    }

    bool ShouldAdjust(Creature const* creature, Unit const* killer)
    {
        // Only DB spawns have a respawn timer worth persisting; summons, pets and
        // script-created creatures are ignored
        if (!creature->GetSpawnId())
            return false;

        Map const* map = creature->GetMap();
        if (!map)
            return false;

        if (Contains(options.filterNeverInstanceIDs, map->GetId()))
            return false;

        if (!Contains(options.filterAlwaysInstanceIDs, map->GetId()))
        {
            switch (GetMapType(map))
            {
                case MAP_TYPE_RAID:
                    if (!options.enableRaids)
                        return false;
                    break;
                case MAP_TYPE_DUNGEON:
                    if (!options.enableDungeons)
                        return false;
                    break;
                case MAP_TYPE_WORLD:
                    if (!options.enableWorld)
                        return false;
                    break;
                default:
                    return false;
            }
        }

        if (Contains(options.filterNeverCreatureIDs, creature->GetEntry()))
            return false;

        if (Contains(options.filterAlwaysCreatureIDs, creature->GetEntry()))
            return true;

        // Pets, guardians, totems and charmed units count as a kill by their
        // controlling player
        if (options.filterKilledByPlayer && (!killer || !killer->GetCharmerOrOwnerPlayerOrPlayerItself()))
            return false;

        uint32 originalDelay = creature->GetRespawnDelay();
        if (originalDelay < options.respawnTimeOriginalMin || originalDelay > options.respawnTimeOriginalMax)
            return false;

        return true;
    }

    uint32 ComputeRespawnTime(Creature const* creature)
    {
        float multiplier = 1.0f;
        switch (GetMapType(creature->GetMap()))
        {
            case MAP_TYPE_RAID:
                multiplier = options.multiplierRaid;
                break;
            case MAP_TYPE_DUNGEON:
                multiplier = options.multiplierDungeon;
                break;
            case MAP_TYPE_WORLD:
                multiplier = options.multiplierWorld;
                break;
            default:
                break;
        }

        uint32 respawnTime = uint32(float(creature->GetRespawnDelay()) * options.multiplierGlobal * multiplier);

        // 0 means "never respawn" and bypasses the adjusted min/max clamp
        if (respawnTime == 0)
            return RESPAWN_DISABLED_SECS;

        return std::clamp(respawnTime, options.respawnTimeAdjustedMin, options.respawnTimeAdjustedMax);
    }
}

class DeadMeansDead_WorldScript : public WorldScript
{
public:
    DeadMeansDead_WorldScript() : WorldScript("DeadMeansDead_WorldScript") { }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        options.Load();
    }
};

class DeadMeansDead_PlayerScript : public PlayerScript
{
public:
    DeadMeansDead_PlayerScript() : PlayerScript("DeadMeansDead_PlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        if (options.enable && options.announce)
            ChatHandler(player->GetSession()).PSendSysMessage("DeadMeansDead is enabled.");
    }
};

class DeadMeansDead_UnitScript : public UnitScript
{
public:
    DeadMeansDead_UnitScript() : UnitScript("DeadMeansDead_UnitScript") { }

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        if (!options.enable)
            return;

        Creature* creature = unit->ToCreature();
        if (!creature || !ShouldAdjust(creature, killer))
            return;

        // Only the absolute respawn time is changed. The creature's respawn delay is
        // deliberately left untouched: it is what Creature::SaveToDB() writes back to
        // creature.spawntimesecs, so altering it would let any GM command that saves
        // the creature persist the adjusted value into the world database. Natural
        // corpse decay (RemoveCorpse(false)) keeps the time set here, and
        // SaveRespawnTime() persists it across map unloads and server restarts.
        creature->SetRespawnTime(ComputeRespawnTime(creature));
        creature->SaveRespawnTime();
    }
};

void AddDeadMeansDeadScripts()
{
    new DeadMeansDead_WorldScript();
    new DeadMeansDead_PlayerScript();
    new DeadMeansDead_UnitScript();
}

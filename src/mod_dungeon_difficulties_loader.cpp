/*
 * mod-dungeon-difficulties loader (modules/CMakeLists.txt calls Add<module>Scripts).
 */

void AddSC_dungeon_difficulties_heroic_scaling();   // HeroicSpellScaling.cpp: config, heroic spell damage hook
void AddSC_dungeon_difficulties_command();          // DungeonDifficultiesCommand.cpp: .dd

void Addmod_dungeon_difficultiesScripts()
{
    AddSC_dungeon_difficulties_heroic_scaling();
    AddSC_dungeon_difficulties_command();
}

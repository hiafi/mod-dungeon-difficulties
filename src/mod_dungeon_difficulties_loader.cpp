/*
 * mod-dungeon-difficulties loader (modules/CMakeLists.txt calls Add<module>Scripts).
 */

void AddSC_dungeon_difficulties_heroic_scaling();   // HeroicSpellScaling.cpp: config, heroic spell damage hook
void AddSC_dungeon_difficulties_command();          // DungeonDifficultiesCommand.cpp: .dd
void AddSC_dungeon_difficulties_flex();             // FlexScale.cpp: the flex 5-10 scaler's hooks

void Addmod_dungeon_difficultiesScripts()
{
    AddSC_dungeon_difficulties_heroic_scaling();
    AddSC_dungeon_difficulties_command();
    AddSC_dungeon_difficulties_flex();
}

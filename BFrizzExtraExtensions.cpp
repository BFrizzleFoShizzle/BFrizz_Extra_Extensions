#include <kenshi/Dialogue.h>
#include <Debug.h>
#include <core/Functions.h>
#include <kenshi/Character.h>
#include <kenshi/GameData.h>
#include <kenshi/Gear.h>
#include <kenshi/CharStats.h>
#include <kenshi/Inventory.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Platoon.h>
#include <kenshi/WorldEventStateQuery.h>
#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>
#include <kenshi/Faction.h>

enum itemTypeExtended
{
	VARIABLE = 1000
};

// WorldEventStateQuery objects don't store a ref to their gamedata so we need this to get it in WorldEventStateQuery::isTrue
// I'm currently not doing garbage collection on this but that should just cause a small memory leak when reloading, not enough to care about
boost::unordered_map<WorldEventStateQuery*, GameData*> queryMap;
// probably unnecessary  but I don't want to take the risk
boost::mutex mapLock;

WorldEventStateQuery* (*getFromData_orig)(GameData* d);
WorldEventStateQuery* getFromData_hook(GameData* d)
{
	WorldEventStateQuery* query = getFromData_orig(d);

	// lock + add to map
	boost::lock_guard<boost::mutex> lock(mapLock);
	queryMap.emplace(query, d);

	return query;
}

enum ExtendedDialogConditionEnum
{
	DC_IS_SLEEPING = 1000,
	DC_HAS_SHORT_TERM_TAG,
	DC_IS_ALLY_BECAUSE_OF_DISGUISE,
	DC_STAT_LEVEL_UNMODIFIED,
	DC_STAT_LEVEL_MODIFIED,
	DC_WEAPON_LEVEL,
	DC_ARMOUR_LEVEL
};

static const float SQUAD_CHECK_RADIUS = 900.0f;

// TODO remove?
static bool DialogCompare(int val1, int val2, ComparisonEnum compareBy)
{

	if (compareBy == ComparisonEnum::CE_EQUALS && val1 == val2)
		return true;
	if (compareBy == ComparisonEnum::CE_LESS_THAN && val1 < val2)
		return true;
	if (compareBy == ComparisonEnum::CE_MORE_THAN && val1 > val2)
		return true;

	return false;
}

static bool DialogCompare(int val1, DialogLineData::DialogCondition* condition)
{

	if (condition->compareBy == ComparisonEnum::CE_EQUALS && val1 == condition->value)
		return true;
	if (condition->compareBy == ComparisonEnum::CE_LESS_THAN && val1 < condition->value)
		return true;
	if (condition->compareBy == ComparisonEnum::CE_MORE_THAN && val1 > condition->value)
		return true;

	return false;
}

static bool checkCondition(Character* characterCheck, Character* characterTarget, DialogLineData::DialogCondition* condition)
{
	switch (condition->key)
	{
		case DC_IS_SLEEPING:
			if (!DialogCompare(characterCheck->inSomething == UseStuffState::IN_BED, condition))
				return false;
			break;
		case DC_IS_ALLY_BECAUSE_OF_DISGUISE:
			if (!DialogCompare((characterCheck->isAlly(characterTarget, true) && !characterCheck->isAlly(characterTarget, false)), condition))
				return false;
			break;
		case DC_WEAPON_LEVEL:
		{
			// Note: value is -1 if unarmed
			// this check often doesn't check equipped weapons on back
			Weapon* weapon = characterCheck->getCurrentWeapon();
			// this seems to be the same
			if (!weapon)
				weapon = characterCheck->getThePreferredWeapon();
			int level = weapon == nullptr ? -1 : weapon->getLevel();
			if (!weapon)
			{
				lektor<InventorySection*> sections;
				characterCheck->inventory->getAllSectionsOfType(sections, AttachSlot::ATTACH_WEAPON);
				for (int i = 0; i < sections.size(); ++i)
				{
					const Ogre::vector<InventorySection::SectionItem>::type& items = sections[i]->getItems();
					for (int j = 0; j < items.size(); ++j)
						if (weapon = dynamic_cast<Weapon*>(items[j].item))
							level = std::max(level, weapon->getLevel());
				}

				// cleanup
				free(sections.stuff);
			}

			if (!DialogCompare(level, condition))
				return false;
			break;
		}
		case DC_ARMOUR_LEVEL:
		{
			// Note: value is -1 if unarmoured
			lektor<Item*> armour;
			armour.maxSize = 0;
			armour.count = 0;
			armour.stuff = nullptr;
			characterCheck->getInventory()->getEquippedArmour(armour);
			bool hasMatch = false;
			// check if any equipped armour meets condition
			for (int i = 0; i < armour.size(); ++i)
				if (DialogCompare(armour[i]->getLevel(), condition))
					hasMatch = true;
			// unarmoured
			if (armour.size() == 0)
				hasMatch = DialogCompare(-1, condition);
			// garbage collect
			if (armour.stuff)
				free(armour.stuff);
			// return false if no armour matches
			if (!hasMatch)
				return false;
			break;
		}
	}
	return true;
}

bool (*DialogLineData_checkConditions_orig)(DialogLineData* thisptr, Dialogue* dialog, Character* target, bool isWordswap);
bool DialogLineData_checkConditions_hook(DialogLineData* thisptr, Dialogue* dialog, Character* target, bool isWordswap)
{
	for (DialogLineData::DialogCondition** condition = thisptr->conditions.begin(); condition < thisptr->conditions.end(); ++condition)
	{
		// T_ME behaviour - do I have memory tag for target
		Character* characterCheck = dialog->getCharacter();
		// I'm sometimes getting NO TARGET?
		Character* characterTarget = target;

		if ((*condition)->who != TalkerEnum::T_ME && (*condition)->who != TalkerEnum::T_WHOLE_SQUAD)
		{
			// swap
			Character* temp = characterTarget;
			characterTarget = characterCheck;
			characterCheck = temp;
		}

		if (!characterCheck)
		{
			ErrorLog("NO SPEAKER");
			break;
		}

		if (!characterTarget)
		{
			ErrorLog("NO TARGET");
			// TODO is this a problem?
			//break;
		}

		if ((*condition)->who == TalkerEnum::T_WHOLE_SQUAD)
		{
			// with above branch, characterCheck will be "me"
			ActivePlatoon* activePlatoon = characterCheck->getPlatoon();
			lektor<RootObject*> characters;
			// couldn't find T_WHOLE_SQUAD radius but interjection radius is similar and appears to be 900
			activePlatoon->getCharactersInArea(characters, characterCheck->getPosition(), SQUAD_CHECK_RADIUS, false);

			// if any
			bool found = false;
			for (int i = 0; i < characters.size(); ++i)
			{
				Character* squadChar = dynamic_cast<Character*>(characters[i]);
				if (squadChar)
				{
					if (checkCondition(squadChar, characterTarget, *condition))
						// condition is met -  move on to the next condition
						found = true;
					//break;
				}
			}

			// cleanup
			if (characters.stuff)
				free(characters.stuff);

			if (!found)
				return false;
		}
		else
		{
			if (!checkCondition(characterCheck, characterTarget, *condition))
				return false;
		}
	}
	return DialogLineData_checkConditions_orig(thisptr, dialog, target, isWordswap);
}

bool checkTag(ExtendedDialogConditionEnum dialogCondition, Character* conditionCheck, Character* conditionTarget, ComparisonEnum compareBy, int tag, int value)
{
	if (dialogCondition == ExtendedDialogConditionEnum::DC_HAS_SHORT_TERM_TAG)
	{
		return DialogCompare(conditionCheck->getCharacterMemoryTag(conditionTarget, (CharacterPerceptionTags_ShortTerm)tag), value, compareBy);
	}
	// both of these can be done together
	else if (dialogCondition == ExtendedDialogConditionEnum::DC_STAT_LEVEL_MODIFIED
		|| dialogCondition == ExtendedDialogConditionEnum::DC_STAT_LEVEL_UNMODIFIED)
	{
		// swap between enum behaviour
		bool unmodified = dialogCondition == ExtendedDialogConditionEnum::DC_STAT_LEVEL_UNMODIFIED;

		float stat = conditionCheck->getStats()->getStat((StatsEnumerated)tag, unmodified);
		return DialogCompare((int)stat, value, compareBy);
	}

	// not our problem
	return true;
}

bool (*checkTags_orig)(DialogLineData* thisptr, Character* me, Character* target);
bool checkTags_hook(DialogLineData* thisptr, Character* me, Character* target)
{
	// TAG CONDITIONS
	for (int i = 0; i < thisptr->conditions.size(); ++i)
	{
		// optimization - only do a full check if there's an actual extended condition
		if (thisptr->conditions[i]->key == ExtendedDialogConditionEnum::DC_HAS_SHORT_TERM_TAG
			|| thisptr->conditions[i]->key == DC_STAT_LEVEL_UNMODIFIED
			|| thisptr->conditions[i]->key == DC_STAT_LEVEL_MODIFIED)
		{
			ogre_unordered_map<std::string, Ogre::vector<GameDataReference>::type>::type::iterator iter = thisptr->data->objectReferences.find("conditions");

			if (iter != thisptr->data->objectReferences.end())
			{
				for (int i = 0; i < iter->second.size(); ++i)
				{
					ExtendedDialogConditionEnum dialogCondition = (ExtendedDialogConditionEnum)iter->second[i].ptr->idata.find("condition name")->second;
					ComparisonEnum compareBy = (ComparisonEnum)iter->second[i].ptr->idata.find("compare by")->second;
					TalkerEnum who = (TalkerEnum)iter->second[i].ptr->idata.find("who")->second;
					int tag = iter->second[i].ptr->idata.find("tag")->second;
					int value = iter->second[i].values[0];

					// T_ME behaviour - do I have tag for target
					Character* conditionCheck = me;
					// Note: target can sometimes be null, seems to happen on interjection nodes
					Character* conditionTarget = target;
					if (who != TalkerEnum::T_ME && who != TalkerEnum::T_WHOLE_SQUAD)
					{
						// swap
						Character* temp = conditionTarget;
						conditionTarget = conditionCheck;
						conditionCheck = temp;
					}

					if (who == TalkerEnum::T_WHOLE_SQUAD)
					{
						ActivePlatoon* platoon = conditionCheck->getPlatoon();
						if (platoon)
						{
							lektor<RootObject*> characters;
							// couldn't find T_WHOLE_SQUAD radius but interjection radius is similar and appears to be 900
							platoon->getCharactersInArea(characters, conditionCheck->getPosition(), SQUAD_CHECK_RADIUS, false);

							bool found = false;
							for (int i = 0; i < characters.size(); ++i)
							{
								Character* squadChar = dynamic_cast<Character*>(characters[i]);
								// if any
								if (squadChar)
								{
									if (checkTag(dialogCondition, squadChar, conditionTarget, compareBy, tag, value))
										// condition is met -  move on to the next condition
										found = true;
									//break;
								}
							}

							// cleanup
							if (characters.stuff)
								free(characters.stuff);

							if (!found)
								return false;
						}
					}
					else
					{
						if (!checkTag(dialogCondition, conditionCheck, conditionTarget, compareBy, tag, value))
							return false;
					}
				}
			}
			// above loop should check all conditions
			break;
		}
	}

	// VARIABLES
	ogre_unordered_map<std::string, Ogre::vector<GameDataReference>::type>::type::iterator iter = thisptr->getGameData()->objectReferences.find("variable equals");
	if (iter != thisptr->getGameData()->objectReferences.end())
	{
		for (Ogre::vector<GameDataReference>::type::iterator variableIter = iter->second.begin(); variableIter != iter->second.end(); ++variableIter)
		{
			ogre_unordered_map<std::string, int>::type::iterator valueIter = variableIter->ptr->idata.find("value");
			if (valueIter != variableIter->ptr->idata.end() && !(valueIter->second == variableIter->values[0]))
				return false;
		}
	}

	iter = thisptr->getGameData()->objectReferences.find("variable less than");
	if (iter != thisptr->getGameData()->objectReferences.end())
	{
		for (Ogre::vector<GameDataReference>::type::iterator variableIter = iter->second.begin(); variableIter != iter->second.end(); ++variableIter)
		{
			ogre_unordered_map<std::string, int>::type::iterator valueIter = variableIter->ptr->idata.find("value");
			if (valueIter != variableIter->ptr->idata.end() && !(valueIter->second < variableIter->values[0]))
				return false;
		}
	}

	iter = thisptr->getGameData()->objectReferences.find("variable greater than");
	if (iter != thisptr->getGameData()->objectReferences.end())
	{
		for (Ogre::vector<GameDataReference>::type::iterator variableIter = iter->second.begin(); variableIter != iter->second.end(); ++variableIter)
		{
			ogre_unordered_map<std::string, int>::type::iterator valueIter = variableIter->ptr->idata.find("value");
			if (valueIter != variableIter->ptr->idata.end() && !(valueIter->second > variableIter->values[0]))
				return false;
		}
	}

	// VANILLA TAGS
	return checkTags_orig(thisptr, me, target);
}

// returns num items taken
int takeItems(Character* giver, Character* taker, GameData* itemData, int count)
{
	int countTaken = 0;
	while (countTaken < count)
	{
		Item* item = giver->inventory->getItem(itemData);

		// stop when the character no longer has instances of the item
		if (item == nullptr)
			return countTaken;

		const int countRemaining = count - countTaken;
		// if stack has more items than we're removing, split and return
		if (item->quantity > countRemaining)
		{
			// part of stack is moved, split by creating new instance
			item->quantity -= countRemaining;
			Item* newItem = ou->theFactory->copyItem(item);
			newItem->quantity = countRemaining;
			taker->giveItem(newItem, true, false);
			return count;
		}

		// whole stack is moved
		countTaken += item->quantity;

		giver->dropItem(item);
		taker->giveItem(item, true, false);
	}

	// count is reached
	return count;
}

// returns num items destroyed
int destroyItems(Character* target, GameData* itemData, int count)
{
	while (true)
	{
		Item* item = target->inventory->getItem(itemData);

		// stop when the character no longer has instances of the item
		if (item == nullptr)
			return count;

		if (item->quantity > count)
		{
			item->quantity -= count;
			return count;
		}

		count -= item->quantity;

		// get rid of inventory references or something, no idea if this is needed but it seems like a good idea
		target->dropItem(item);
		ou->destroy(item, false, "Destroy item event");
	}
	return count;
}

void doRefAction(const std::string& action, Ogre::vector<GameDataReference>::type& ref, Dialogue* thisptr)
{
	if (ref.size() == 0)
	{
		ErrorLog("Missing references for \"" + action + "\"");
	}

	if (action == "take item" || action == "take item from squad")
	{
		for (Ogre::vector<GameDataReference>::type::iterator itemIter = ref.begin(); itemIter != ref.end(); ++itemIter)
		{
			Character* giver = thisptr->getConversationTarget().getCharacter();
			Character* taker = thisptr->me;
			if (giver != nullptr && taker != nullptr)
			{
				if (action == "take item")
				{
					takeItems(giver, taker, itemIter->ptr, itemIter->values[0]);
				}
				else
				{
					ActivePlatoon* activePlatoon = giver->getPlatoon();
					lektor<RootObject*> characters;
					// couldn't find T_WHOLE_SQUAD radius but interjection radius is similar and appears to be 900
					activePlatoon->getCharactersInArea(characters, taker->getPosition(), SQUAD_CHECK_RADIUS, false);

					int itemsLeft = itemIter->values[0];
					for (int c = 0; c < characters.size(); ++c)
					{
						Character* squadChar = dynamic_cast<Character*>(characters[c]);
						if (squadChar)
							itemsLeft -= takeItems(giver, taker, itemIter->ptr, itemsLeft);
						if (itemsLeft == 0)
							break;
					}

					// cleanup
					free(characters.stuff);
				}
			}
		}
	}
	else if (action == "destroy item" || action == "destroy item from squad")
	{

		for (Ogre::vector<GameDataReference>::type::iterator itemIter = ref.begin(); itemIter != ref.end(); ++itemIter)
		{
			Character* target = thisptr->getConversationTarget().getCharacter();
			if (target != nullptr)
			{
				if (action == "destroy item")
				{
					destroyItems(target, itemIter->ptr, itemIter->values[0]);
				}
				else
				{
					ActivePlatoon* activePlatoon = target->getPlatoon();
					lektor<RootObject*> characters;
					// couldn't find T_WHOLE_SQUAD radius but interjection radius is similar and appears to be 900
					activePlatoon->getCharactersInArea(characters, target->getPosition(), SQUAD_CHECK_RADIUS, false);

					int itemsLeft = itemIter->values[0];
					for (int c = 0; c < characters.size(); ++c)
					{
						Character* squadChar = dynamic_cast<Character*>(characters[c]);
						if (squadChar)
							itemsLeft -= destroyItems(target, itemIter->ptr, itemsLeft);
						if (itemsLeft == 0)
							break;
					}

					// cleanup
					free(characters.stuff);
				}
			}
		}
	}
}

bool checkWorldStateVariable(const std::string &condition, Ogre::vector<GameDataReference>::type& ref)
{
	for (Ogre::vector<GameDataReference>::type::iterator variableIter = ref.begin(); variableIter != ref.end(); ++variableIter)
	{
		int targetVal = variableIter->values[0];
		ogre_unordered_map<std::string, int>::type::iterator valueIter = variableIter->ptr->idata.find("value");

		if (valueIter != variableIter->ptr->idata.end())
		{
			if(condition == "variable equals")
				return targetVal == valueIter->second;
			if (condition == "variable less than")
				return targetVal < valueIter->second;
			if (condition == "variable greater than")
				return targetVal > valueIter->second;
		}
		else
		{
			ErrorLog("WorldStates: Variable is missing value");
		}
	}
	ErrorLog("Invalid world state condition: " + condition);
	return false;
}

bool (*WorldEventStateQuery_isTrue_orig)(WorldEventStateQuery* thisptr);
bool WorldEventStateQuery_isTrue_hook(WorldEventStateQuery* thisptr)
{
	// regular conditions
	bool state = WorldEventStateQuery_isTrue_orig(thisptr);

	GameData* gameData;
	// prevent race conditions
	{
		boost::lock_guard<boost::mutex> lock(mapLock);
		gameData = queryMap[thisptr];
	}

	// our new conditions
	ogre_unordered_map<std::string, Ogre::vector<GameDataReference>::type>::type::iterator iter = gameData->objectReferences.find("variable equals");
	if (iter != gameData->objectReferences.end() && !checkWorldStateVariable("variable equals", iter->second))
		return false;

	iter = gameData->objectReferences.find("variable less than");
	if (iter != gameData->objectReferences.end() && !checkWorldStateVariable("variable less than", iter->second))
		return false;

	iter = gameData->objectReferences.find("variable greater than");
	if (iter != gameData->objectReferences.end() && !checkWorldStateVariable("variable greater than", iter->second))
		return false;

	return state;
}

void changeWorldStateVariable(const std::string& action, Ogre::vector<GameDataReference>::type& ref)
{
	if (ref.size() == 0)
	{
		ErrorLog("Missing references for \"" + action + "\"");
	}

	for (Ogre::vector<GameDataReference>::type::iterator variableIter = ref.begin(); variableIter != ref.end(); ++variableIter)
	{
		ogre_unordered_map<std::string, int>::type::iterator valueIter = variableIter->ptr->idata.find("value");

		if (valueIter != variableIter->ptr->idata.end())
		{
			if(action == "set variable")
				valueIter->second = variableIter->values[0];
			else if (action == "add to variable")
				valueIter->second += variableIter->values[0];
		}
		else
		{
			ErrorLog("Missing parameter: value");
		}
	}
}

void (*_doActions_orig)(Dialogue* thisptr, DialogLineData* dialogLine);
void _doActions_hook(Dialogue* thisptr, DialogLineData* dialogLine)
{
	// DIALOGUE EFFECTS
	ogre_unordered_map<std::string, Ogre::vector<GameDataReference>::type>::type::iterator iter = dialogLine->getGameData()->objectReferences.find("take item");
	if (iter != dialogLine->getGameData()->objectReferences.end())
		doRefAction("take item", iter->second, thisptr);

	iter = dialogLine->getGameData()->objectReferences.find("take item from squad");
	if (iter != dialogLine->getGameData()->objectReferences.end())
		doRefAction("take item from squad", iter->second, thisptr);

	iter = dialogLine->getGameData()->objectReferences.find("destroy item");
	if (iter != dialogLine->getGameData()->objectReferences.end())
		doRefAction("destroy item", iter->second, thisptr);

	iter = dialogLine->getGameData()->objectReferences.find("destroy item from squad");
	if (iter != dialogLine->getGameData()->objectReferences.end())
		doRefAction("destroy item from squad", iter->second, thisptr);

	// VARIABLE EFFECTS
	iter = dialogLine->getGameData()->objectReferences.find("set variable");
	if (iter != dialogLine->getGameData()->objectReferences.end())
		changeWorldStateVariable("set variable", iter->second);

	iter = dialogLine->getGameData()->objectReferences.find("add to variable");
	if (iter != dialogLine->getGameData()->objectReferences.end())
		changeWorldStateVariable("add to variable", iter->second);

	// continue
	_doActions_orig(thisptr, dialogLine);
}

// this is a convenient place to hook into the save system
// not 100% sure this is the best way to save data - data here is written to "quick.save"
void (*saveGameState_orig)(FactionManager* thisptr, GameDataContainer* container);
void saveGameState_hook(FactionManager* thisptr, GameDataContainer* container)
{
	// find VARIABLE gamedata
	ogre_unordered_map<std::string, GameData*>::type::iterator iter = ou->gamedata.gamedataSID.begin();
	for (; iter != ou->gamedata.gamedataSID.end(); ++iter)
	{
		if (iter->second->type == (itemType)VARIABLE)
		{
			// create new GameData in save
			GameData* newGameData2 = container->createNewData(iter->second->type, iter->second->stringID, iter->second->name);
			if (newGameData2)
				newGameData2->updateFrom(iter->second, false);
		}
	}
	saveGameState_orig(thisptr, container);
}

// save data is loaded into ou->savedata but the dialogue/world state reference is to the object in ou->gamedata
// so we copy the value over on load
// this probably isn't the ideal function to hook but as far as I can tell it's only called when loading a save game
void (*loadAllPlatoons_orig)(GameWorld* thisptr);
void loadAllPlatoons_hook(GameWorld* thisptr)
{
	loadAllPlatoons_orig(thisptr);

	ogre_unordered_map<std::string, GameData*>::type::iterator iter = ou->savedata.gamedataSID.begin();
	for (; iter != ou->savedata.gamedataSID.end(); ++iter)
	{
		if (iter->second->type == (itemType)VARIABLE)
		{
			ou->gamedata.updateData(iter->second, false);
		}
	}
}

__declspec(dllexport) void startPlugin()
{
	// there's no obvious way to track which GameData is associated with which WorldEventStateQuery/List so we make our own
	// NOTE: there's no garage collection...
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&WorldEventStateQuery::getFromData), &getFromData_hook, &getFromData_orig))
		DebugLog("WorldStates: Could not hook function!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&WorldEventStateQuery::isTrue), &WorldEventStateQuery_isTrue_hook, &WorldEventStateQuery_isTrue_orig))
		DebugLog("WorldStates: Could not hook function!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&DialogLineData::checkConditions), &DialogLineData_checkConditions_hook, &DialogLineData_checkConditions_orig))
		ErrorLog("Dialogue Extensions: could not install hook!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&DialogLineData::checkTags), &checkTags_hook, &checkTags_orig))
		ErrorLog("Dialogue Extensions: could not install hook!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Dialogue::_doActions), &_doActions_hook, &_doActions_orig))
		DebugLog("Dialogue Extensions: Could not hook function!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&FactionManager::saveGameState), &saveGameState_hook, &saveGameState_orig))
		DebugLog("WorldStates: Could not hook function!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&GameWorld::loadAllPlatoons), &loadAllPlatoons_hook, &loadAllPlatoons_orig))
		DebugLog("WorldStates: Could not hook function!");
}